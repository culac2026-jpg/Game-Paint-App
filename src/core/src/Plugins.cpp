#include <gas/core/Plugins.h>

#include <gas/core/Document.h>
#include <gas/core/Slicing.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace gas {

namespace fs = std::filesystem;

// ------------------------------------------------------------ shared libraries

struct PluginHost::Library {
#ifdef _WIN32
    HMODULE handle = nullptr;
    ~Library() {
        if (handle) {
            FreeLibrary(handle);
        }
    }
    void* symbol(const char* name) const {
        return reinterpret_cast<void*>(GetProcAddress(handle, name));
    }
#else
    void* handle = nullptr;
    ~Library() {
        if (handle) {
            dlclose(handle);
        }
    }
    void* symbol(const char* name) const { return dlsym(handle, name); }
#endif
};

namespace {

std::string str(const char* s, std::size_t maxLen = 4096) {
    if (!s) {
        return {};
    }
    std::size_t n = 0;
    while (n < maxLen && s[n] != '\0') {
        ++n;
    }
    return std::string(s, n);
}

std::vector<std::string> splitChoices(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t bar = s.find('|', start);
        out.push_back(s.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
        if (bar == std::string::npos) {
            break;
        }
        start = bar + 1;
    }
    return out;
}

template <typename Fn>
Fn symbolAs(void* p) {
    Fn fn;
    static_assert(sizeof(fn) == sizeof(p));
    std::memcpy(&fn, &p, sizeof(fn)); // object-to-function pointer without a cast warning
    return fn;
}

std::string errorText(const char* buffer, std::size_t size) {
    std::string s = str(buffer, size);
    return s.empty() ? std::string("The plugin reported an error") : s;
}

} // namespace

std::vector<double> PluginFilter::defaults() const {
    std::vector<double> out;
    for (const PluginParam& p : params) {
        out.push_back(p.defaultValue);
    }
    return out;
}

PluginHost::PluginHost() = default;

PluginHost::~PluginHost() {
    // Drop the callbacks before the code they point into goes away.
    filters_.clear();
    exporters_.clear();
    libraries_.clear();
}

void PluginHost::loadDirectory(const fs::path& directory) {
#ifdef _WIN32
    const fs::path ext = ".dll";
#elif defined(__APPLE__)
    const fs::path ext = ".dylib";
#else
    const fs::path ext = ".so";
#endif
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ext) {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        loadLibrary(f);
    }
}

bool PluginHost::loadLibrary(const fs::path& file) {
    PluginInfo info;
    info.file = file;
    info.name = file.stem().string();
    auto lib = std::make_unique<Library>();
#ifdef _WIN32
    // Resolve the plugin's own dependencies next to it first.
    lib->handle = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
    lib->handle = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (!lib->handle) {
        info.error = "Cannot load the library";
        plugins_.push_back(std::move(info));
        return false;
    }
    void* versionSym = lib->symbol("gas_plugin_api_version");
    void* initSym = lib->symbol("gas_plugin_init");
    if (!versionSym || !initSym) {
        info.error = "Not a GameArt Studio plugin (gas_plugin_init missing)";
        plugins_.push_back(std::move(info));
        return false;
    }
    const std::uint32_t version = symbolAs<GasPluginApiVersionFn>(versionSym)();
    if (version == 0 || version > GAS_PLUGIN_API_VERSION) {
        info.error = "Needs plugin API version " + std::to_string(version) + " (this editor has " +
                     std::to_string(GAS_PLUGIN_API_VERSION) + ")";
        plugins_.push_back(std::move(info));
        return false;
    }
    if (!runInit(info, symbolAs<GasPluginInitFn>(initSym))) {
        plugins_.push_back(std::move(info));
        return false;
    }
    libraries_.push_back(std::move(lib));
    plugins_.push_back(std::move(info));
    return true;
}

bool PluginHost::registerPlugin(const std::string& name, GasPluginInitFn init) {
    PluginInfo info;
    info.name = name;
    const bool ok = init && runInit(info, init);
    if (!init) {
        info.error = "No init function";
    }
    plugins_.push_back(std::move(info));
    return ok;
}

bool PluginHost::runInit(PluginInfo& info, GasPluginInitFn init) {
    const std::size_t filtersBefore = filters_.size(), exportersBefore = exporters_.size();
    GasHost host{};
    host.apiVersion = GAS_PLUGIN_API_VERSION;
    host.structSize = sizeof(GasHost);
    host.context = this;
    host.registerFilter = &PluginHost::hostRegisterFilter;
    host.registerExporter = &PluginHost::hostRegisterExporter;
    host.log = &PluginHost::hostLog;
    host.setPluginName = &PluginHost::hostSetName;
    current_ = &info;
    const std::int32_t result = init(&host);
    current_ = nullptr;
    if (result != 0) {
        // Forget whatever it registered before failing.
        filters_.resize(filtersBefore);
        exporters_.resize(exportersBefore);
        info.error = "Initialisation failed (code " + std::to_string(result) + ")";
        return false;
    }
    for (std::size_t i = filtersBefore; i < filters_.size(); ++i) {
        filters_[i].plugin = info.name;
    }
    for (std::size_t i = exportersBefore; i < exporters_.size(); ++i) {
        exporters_[i].plugin = info.name;
    }
    info.filters = static_cast<int>(filters_.size() - filtersBefore);
    info.exporters = static_cast<int>(exporters_.size() - exportersBefore);
    info.loaded = true;
    return true;
}

int32_t PluginHost::hostRegisterFilter(GasHost* host, const GasFilter* filter) {
    auto* self = static_cast<PluginHost*>(host->context);
    if (!self->current_ || !filter || filter->structSize < offsetof(GasFilter, user) + sizeof(void*) ||
        !filter->apply) {
        return 1;
    }
    PluginFilter f;
    f.id = str(filter->id, 256);
    f.name = str(filter->name, 256);
    f.description = str(filter->description);
    if (f.id.empty() || f.name.empty() || self->findFilter(f.id) || filter->paramCount < 0 ||
        filter->paramCount > 64 || (filter->paramCount > 0 && !filter->params)) {
        self->current_->log.push_back("Filter rejected: " + (f.id.empty() ? std::string("(no id)") : f.id));
        return 1;
    }
    for (int i = 0; i < filter->paramCount; ++i) {
        const GasParam& src = filter->params[i];
        PluginParam p;
        p.key = str(src.key, 256);
        p.label = str(src.label, 256);
        if (p.label.empty()) {
            p.label = p.key;
        }
        p.type = src.type >= GAS_PARAM_INT && src.type <= GAS_PARAM_CHOICE ? static_cast<GasParamType>(src.type)
                                                                            : GAS_PARAM_FLOAT;
        p.minValue = std::isfinite(src.minValue) ? src.minValue : 0;
        p.maxValue = std::isfinite(src.maxValue) ? std::max(src.maxValue, p.minValue) : p.minValue;
        if (p.type == GAS_PARAM_BOOL) {
            p.minValue = 0;
            p.maxValue = 1;
        } else if (p.type == GAS_PARAM_COLOR) {
            p.minValue = 0;
            p.maxValue = 4294967295.0;
        } else if (p.type == GAS_PARAM_CHOICE) {
            p.choices = splitChoices(str(src.choices));
            p.minValue = 0;
            p.maxValue = static_cast<double>(p.choices.size()) - 1;
        }
        p.defaultValue = std::clamp(std::isfinite(src.defaultValue) ? src.defaultValue : p.minValue, p.minValue,
                                    p.maxValue);
        f.params.push_back(std::move(p));
    }
    f.apply = filter->apply;
    f.user = filter->user;
    self->filters_.push_back(std::move(f));
    return 0;
}

int32_t PluginHost::hostRegisterExporter(GasHost* host, const GasExporter* exporter) {
    auto* self = static_cast<PluginHost*>(host->context);
    if (!self->current_ || !exporter || exporter->structSize < offsetof(GasExporter, user) + sizeof(void*) ||
        !exporter->write) {
        return 1;
    }
    PluginExporter e;
    e.id = str(exporter->id, 256);
    e.name = str(exporter->name, 256);
    e.fileFilter = str(exporter->fileFilter, 256);
    e.extension = str(exporter->extension, 32);
    if (e.id.empty() || e.name.empty() || self->findExporter(e.id)) {
        self->current_->log.push_back("Exporter rejected: " + (e.id.empty() ? std::string("(no id)") : e.id));
        return 1;
    }
    if (!e.extension.empty() && e.extension.front() != '.') {
        e.extension.insert(e.extension.begin(), '.');
    }
    e.write = exporter->write;
    e.user = exporter->user;
    self->exporters_.push_back(std::move(e));
    return 0;
}

void PluginHost::hostLog(GasHost* host, const char* message) {
    auto* self = static_cast<PluginHost*>(host->context);
    if (self->current_ && self->current_->log.size() < 100) {
        self->current_->log.push_back(str(message, 1024));
    }
}

void PluginHost::hostSetName(GasHost* host, const char* name) {
    auto* self = static_cast<PluginHost*>(host->context);
    if (self->current_ && name && *name) {
        self->current_->name = str(name, 128);
    }
}

const PluginFilter* PluginHost::findFilter(const std::string& id) const {
    const auto it = std::find_if(filters_.begin(), filters_.end(), [&](const PluginFilter& f) { return f.id == id; });
    return it == filters_.end() ? nullptr : &*it;
}

const PluginExporter* PluginHost::findExporter(const std::string& id) const {
    const auto it =
        std::find_if(exporters_.begin(), exporters_.end(), [&](const PluginExporter& e) { return e.id == id; });
    return it == exporters_.end() ? nullptr : &*it;
}

// ------------------------------------------------------------ running filters

IoResult applyPluginFilter(Document& doc, const PluginFilter& filter, std::vector<double> values) {
    if (!filter.apply) {
        return {false, "The filter has no function"};
    }
    const std::size_t given = values.size();
    values.resize(filter.params.size());
    for (std::size_t i = 0; i < filter.params.size(); ++i) {
        const PluginParam& p = filter.params[i];
        if (i >= given) {
            values[i] = p.defaultValue;
        }
        double v = std::isfinite(values[i]) ? values[i] : p.defaultValue;
        if (p.type != GAS_PARAM_FLOAT) {
            v = std::round(v);
        }
        values[i] = std::clamp(v, p.minValue, p.maxValue);
    }
    // Work on the selection bounds (the document keeps pixels outside the
    // selection itself), else the whole canvas.
    IRect region{0, 0, doc.width(), doc.height()};
    if (!doc.selection().isEmpty()) {
        if (const std::optional<IRect> b = doc.selection().bounds()) {
            region = *b;
        }
    }
    if (region.empty()) {
        return {false, "Nothing to filter"};
    }
    TiledImage* image = doc.beginEdit();
    if (!image) {
        return {false, "The active layer can't be edited (locked, hidden or a vector layer)"};
    }
    std::vector<Rgba8> pixels = copyPixels(*image, region);
    const std::vector<Rgba8> before = pixels;
    GasImage view{region.w, region.h, region.w * 4, reinterpret_cast<std::uint8_t*>(pixels.data()), region.x,
                  region.y};
    char error[512] = {};
    if (filter.apply(filter.user, &view, values.data(), error, static_cast<int32_t>(sizeof(error))) != 0) {
        doc.revertEdit();
        doc.endEdit(filter.name);
        return {false, errorText(error, sizeof(error))};
    }
    for (int y = 0; y < region.h; ++y) {
        for (int x = 0; x < region.w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(region.w) +
                                  static_cast<std::size_t>(x);
            Rgba8 c = pixels[i];
            if (c.a == 0) {
                c = {0, 0, 0, 0}; // keep the "alpha 0 = all zero" invariant
            }
            if (!(c == before[i])) {
                image->setPixel({region.x + x, region.y + y}, c);
            }
        }
    }
    doc.endEdit(filter.name);
    return {true, {}};
}

// ------------------------------------------------------------ running exporters

namespace {

struct ExportContext {
    const Document* doc = nullptr;
    std::deque<std::string> strings; // names handed out (stable addresses)
    std::vector<std::unique_ptr<std::vector<Rgba8>>> images; // rendered buffers
};

const ExportContext& ctxOf(const GasDocument* d) { return *static_cast<const ExportContext*>(d->host); }

int32_t exLayerInfo(const GasDocument* d, int32_t index, GasLayerInfo* out) {
    const ExportContext& ctx = ctxOf(d);
    if (!out || index < 0 || index >= ctx.doc->layerCount()) {
        return 1;
    }
    const Layer& l = ctx.doc->layer(index);
    auto& strings = const_cast<ExportContext&>(ctx).strings;
    strings.push_back(l.props.name);
    *out = {strings.back().c_str(), l.props.visible ? 1 : 0, static_cast<float>(l.props.opacity),
            l.vector ? 1 : 0, l.mask ? 1 : 0};
    return 0;
}

int32_t exSliceInfo(const GasDocument* d, int32_t index, GasSliceInfo* out) {
    const ExportContext& ctx = ctxOf(d);
    if (!out || index < 0 || index >= static_cast<int32_t>(ctx.doc->slices().size())) {
        return 1;
    }
    const Slice& s = ctx.doc->slices()[static_cast<std::size_t>(index)];
    auto& strings = const_cast<ExportContext&>(ctx).strings;
    strings.push_back(s.name);
    *out = {strings.back().c_str(), s.rect.x, s.rect.y, s.rect.w, s.rect.h, s.pivot.x, s.pivot.y,
            s.border.left, s.border.top, s.border.right, s.border.bottom};
    return 0;
}

int32_t exFrameDuration(const GasDocument* d, int32_t frame) {
    const ExportContext& ctx = ctxOf(d);
    if (frame < 0 || frame >= ctx.doc->frameCount()) {
        return -1;
    }
    return ctx.doc->frames()[static_cast<std::size_t>(frame)].duration;
}

int32_t handOut(const GasDocument* d, const TiledImage& image, GasImage* out) {
    auto& ctx = const_cast<ExportContext&>(ctxOf(d));
    auto buffer = std::make_unique<std::vector<Rgba8>>(copyPixels(image, {0, 0, image.width(), image.height()}));
    *out = {image.width(), image.height(), image.width() * 4, reinterpret_cast<std::uint8_t*>(buffer->data()), 0,
            0};
    ctx.images.push_back(std::move(buffer));
    return 0;
}

int32_t exRenderFrame(const GasDocument* d, int32_t frame, GasImage* out) {
    const ExportContext& ctx = ctxOf(d);
    if (!out || frame < 0 || frame >= ctx.doc->frameCount()) {
        return 1;
    }
    return handOut(d, ctx.doc->flattenedFrame(frame), out);
}

int32_t exRenderLayer(const GasDocument* d, int32_t layer, int32_t frame, GasImage* out) {
    const ExportContext& ctx = ctxOf(d);
    if (!out || layer < 0 || layer >= ctx.doc->layerCount() || frame < 0 || frame >= ctx.doc->frameCount()) {
        return 1;
    }
    const Layer& l = ctx.doc->layer(layer);
    const TiledImage& img = frame == ctx.doc->activeFrame() || static_cast<std::size_t>(frame) >= l.cels.size()
                                ? l.image
                                : l.cels[static_cast<std::size_t>(frame)];
    return handOut(d, img, out);
}

int32_t exWritePng(const GasDocument*, const GasImage* image, int32_t x, int32_t y, int32_t w, int32_t h,
                   const char* utf8Path) {
    if (!image || !image->pixels || !utf8Path || image->width <= 0 || image->height <= 0 ||
        image->stride < image->width * 4) {
        return 1;
    }
    if (w <= 0 || h <= 0) {
        x = y = 0;
        w = image->width;
        h = image->height;
    }
    if (x < 0 || y < 0 || x + w > image->width || y + h > image->height) {
        return 1;
    }
    std::vector<Rgba8> pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int row = 0; row < h; ++row) {
        std::memcpy(pixels.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(w),
                    image->pixels + static_cast<std::ptrdiff_t>(y + row) * image->stride + x * 4,
                    static_cast<std::size_t>(w) * 4);
    }
    const std::string path(utf8Path);
    return writeFileAtomic(fs::path(std::u8string(path.begin(), path.end())), encodePng(w, h, pixels)).ok ? 0 : 1;
}

} // namespace

IoResult runPluginExporter(const Document& doc, const PluginExporter& exporter, const fs::path& path) {
    if (!exporter.write) {
        return {false, "The exporter has no function"};
    }
    ExportContext ctx;
    ctx.doc = &doc;
    GasDocument view{};
    view.structSize = sizeof(GasDocument);
    view.width = doc.width();
    view.height = doc.height();
    view.layerCount = doc.layerCount();
    view.frameCount = doc.frameCount();
    view.activeFrame = doc.activeFrame();
    view.sliceCount = static_cast<int32_t>(doc.slices().size());
    view.host = &ctx;
    view.layerInfo = &exLayerInfo;
    view.sliceInfo = &exSliceInfo;
    view.frameDuration = &exFrameDuration;
    view.renderFrame = &exRenderFrame;
    view.renderLayer = &exRenderLayer;
    view.writePng = &exWritePng;
    const std::u8string u8 = path.u8string();
    const std::string utf8(u8.begin(), u8.end());
    char error[512] = {};
    if (exporter.write(exporter.user, &view, utf8.c_str(), error, static_cast<int32_t>(sizeof(error))) != 0) {
        return {false, errorText(error, sizeof(error))};
    }
    return {true, {}};
}

} // namespace gas
