#pragma once

#include <gas/core/ProjectIO.h>
#include <gas/plugin/GasPlugin.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gas {

class Document;

struct PluginParam {
    std::string key;
    std::string label;
    GasParamType type = GAS_PARAM_FLOAT;
    double minValue = 0, maxValue = 1, defaultValue = 0;
    std::vector<std::string> choices;
};

struct PluginFilter {
    std::string id, name, description;
    std::string plugin; // display name of the plugin that registered it
    std::vector<PluginParam> params;
    int32_t (*apply)(void*, GasImage*, const double*, char*, int32_t) = nullptr;
    void* user = nullptr;

    std::vector<double> defaults() const;
};

struct PluginExporter {
    std::string id, name, fileFilter, extension;
    std::string plugin;
    int32_t (*write)(void*, const GasDocument*, const char*, char*, int32_t) = nullptr;
    void* user = nullptr;
};

struct PluginInfo {
    std::filesystem::path file; // empty for in-process plugins
    std::string name;
    bool loaded = false;
    std::string error;          // why it was not loaded
    std::vector<std::string> log;
    int filters = 0, exporters = 0;
};

// Loads plugins (shared libraries exporting gas_plugin_init, see GasPlugin.h)
// and keeps what they register. Libraries stay loaded until the host is
// destroyed, so registered callbacks remain valid.
class PluginHost {
public:
    PluginHost();
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // Every .dll (.so / .dylib elsewhere) in `directory`, in name order.
    void loadDirectory(const std::filesystem::path& directory);
    bool loadLibrary(const std::filesystem::path& file);
    // A plugin linked into the process (built-ins, tests).
    bool registerPlugin(const std::string& name, GasPluginInitFn init);

    const std::vector<PluginInfo>& plugins() const { return plugins_; }
    const std::vector<PluginFilter>& filters() const { return filters_; }
    const std::vector<PluginExporter>& exporters() const { return exporters_; }
    const PluginFilter* findFilter(const std::string& id) const;
    const PluginExporter* findExporter(const std::string& id) const;

private:
    struct Library;
    bool runInit(PluginInfo& info, GasPluginInitFn init);
    static int32_t hostRegisterFilter(GasHost* host, const GasFilter* filter);
    static int32_t hostRegisterExporter(GasHost* host, const GasExporter* exporter);
    static void hostLog(GasHost* host, const char* message);
    static void hostSetName(GasHost* host, const char* name);

    std::vector<std::unique_ptr<Library>> libraries_;
    std::vector<PluginInfo> plugins_;
    std::vector<PluginFilter> filters_;
    std::vector<PluginExporter> exporters_;
    PluginInfo* current_ = nullptr; // plugin being initialised
};

// Runs `filter` on the active layer (or its mask) of the active frame, inside
// the selection, as one undo step named after the filter. `values` are
// clamped to the parameter ranges (missing ones take the default).
IoResult applyPluginFilter(Document& document, const PluginFilter& filter, std::vector<double> values);
IoResult runPluginExporter(const Document& document, const PluginExporter& exporter,
                           const std::filesystem::path& path);

} // namespace gas
