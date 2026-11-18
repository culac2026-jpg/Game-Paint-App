#include "gas/core/ProjectIO.h"

#include "gas/core/Document.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <system_error>

namespace gas {

namespace {

namespace fs = std::filesystem;

constexpr std::array<char, 8> kMagic{'G', 'A', 'S', 'P', 'R', 'O', 'J', '\x1A'};
constexpr std::size_t kTileBytes = sizeof(TiledImage::Tile);
constexpr std::size_t kMaxNameLength = 1024;

constexpr std::uint32_t fourcc(const char (&s)[5]) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[3])) << 24;
}
constexpr std::uint32_t kChunkDoc = fourcc("DOCM");
constexpr std::uint32_t kChunkLayer = fourcc("LAYR");
constexpr std::uint32_t kChunkMask = fourcc("LMSK");
constexpr std::uint32_t kChunkLayerProps = fourcc("LPRP");
constexpr std::uint32_t kChunkEffects = fourcc("LEFX");
constexpr std::uint32_t kChunkSlices = fourcc("SLCS");
constexpr std::uint32_t kChunkEnd = fourcc("END ");

std::uint32_t crc32Of(const std::uint8_t* data, std::size_t size) {
    return static_cast<std::uint32_t>(crc32_z(crc32_z(0L, Z_NULL, 0), data, size));
}

// ------------------------------------------------------------ byte streams

class ByteWriter {
public:
    std::vector<std::uint8_t> bytes;

    void u8(std::uint8_t v) { bytes.push_back(v); }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v));
        u8(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        for (int shift = 0; shift < 32; shift += 8) {
            u8(static_cast<std::uint8_t>(v >> shift));
        }
    }
    void f32(float v) {
        std::uint32_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        u32(bits);
    }
    void raw(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + size);
    }
    void chunk(std::uint32_t tag, const ByteWriter& payload) {
        u32(tag);
        u32(static_cast<std::uint32_t>(payload.bytes.size()));
        raw(payload.bytes.data(), payload.bytes.size());
    }
};

// Reads past the end set ok = false and return zeros, so parsing code can
// check once at the end of each record.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool ok() const { return ok_; }
    std::size_t remaining() const { return size_ - pos_; }

    std::uint8_t u8() { return take(1) ? data_[pos_ - 1] : 0; }
    std::uint16_t u16() {
        const std::uint16_t lo = u8();
        return static_cast<std::uint16_t>(lo | u8() << 8);
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int shift = 0; shift < 32; shift += 8) {
            v |= static_cast<std::uint32_t>(u8()) << shift;
        }
        return v;
    }
    float f32() {
        const std::uint32_t bits = u32();
        float v;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }
    const std::uint8_t* bytes(std::size_t n) { return take(n) ? data_ + pos_ - n : nullptr; }

private:
    bool take(std::size_t n) {
        if (!ok_ || n > size_ - pos_) {
            ok_ = false;
            return false;
        }
        pos_ += n;
        return true;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

bool isEmptyTile(const TiledImage::Tile& t) {
    return std::all_of(t.pixels.begin(), t.pixels.end(), [](Rgba8 p) { return p.a == 0; });
}

std::vector<std::uint8_t> deflate(const void* data, std::size_t size) {
    uLongf outSize = compressBound(static_cast<uLong>(size));
    std::vector<std::uint8_t> out(outSize);
    if (compress2(out.data(), &outSize, static_cast<const Bytef*>(data), static_cast<uLong>(size),
                  Z_DEFAULT_COMPRESSION) != Z_OK) {
        return {};
    }
    out.resize(outSize);
    return out;
}

LoadResult fail(std::string message) { return {nullptr, std::move(message)}; }

// u32 tileCount, then per non-empty tile: u32 tx, u32 ty, u32 size, zlib data.
bool writeTiles(ByteWriter& w, const TiledImage& image) {
    ByteWriter tiles;
    std::uint32_t count = 0;
    for (int ty = 0; ty < image.tilesY(); ++ty) {
        for (int tx = 0; tx < image.tilesX(); ++tx) {
            const TiledImage::Tile* t = image.tile(tx, ty);
            if (!t || isEmptyTile(*t)) {
                continue;
            }
            const std::vector<std::uint8_t> packed = deflate(t->pixels.data(), kTileBytes);
            if (packed.empty()) {
                return false;
            }
            tiles.u32(static_cast<std::uint32_t>(tx));
            tiles.u32(static_cast<std::uint32_t>(ty));
            tiles.u32(static_cast<std::uint32_t>(packed.size()));
            tiles.raw(packed.data(), packed.size());
            ++count;
        }
    }
    w.u32(count);
    w.raw(tiles.bytes.data(), tiles.bytes.size());
    return true;
}

// Returns an error message, or empty on success.
std::string readTiles(ByteReader& r, TiledImage& image) {
    const std::uint32_t count = r.u32();
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t tx = r.u32(), ty = r.u32(), packedSize = r.u32();
        const std::uint8_t* packed = r.bytes(packedSize);
        if (!r.ok()) {
            return "Invalid tile record";
        }
        if (tx >= static_cast<std::uint32_t>(image.tilesX()) || ty >= static_cast<std::uint32_t>(image.tilesY())) {
            return "Tile outside the canvas";
        }
        auto tile = std::make_shared<TiledImage::Tile>();
        uLongf outSize = static_cast<uLongf>(kTileBytes);
        if (uncompress(reinterpret_cast<Bytef*>(tile->pixels.data()), &outSize, packed,
                       static_cast<uLong>(packedSize)) != Z_OK ||
            outSize != kTileBytes) {
            return "Corrupted tile data";
        }
        image.setTile(static_cast<int>(tx), static_cast<int>(ty), std::move(tile));
    }
    image.takeDirtyTiles();
    return {};
}

} // namespace

// ------------------------------------------------------------ atomic write

IoResult writeFileAtomic(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return {false, "Cannot write to this folder"};
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            out.close();
            std::error_code ignored;
            fs::remove(tmp, ignored);
            return {false, "Failed to write the file (disk full?)"};
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec); // replaces an existing target
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return {false, "Failed to replace the file: " + ec.message()};
    }
    return {true, {}};
}

// ------------------------------------------------------------ project save

IoResult saveProject(const Document& doc, const fs::path& path) {
    ByteWriter w;
    w.raw(kMagic.data(), kMagic.size());
    w.u32(kProjectFormatVersion);

    ByteWriter header;
    header.u32(static_cast<std::uint32_t>(doc.width()));
    header.u32(static_cast<std::uint32_t>(doc.height()));
    header.u32(static_cast<std::uint32_t>(doc.activeLayerIndex()));
    header.u32(static_cast<std::uint32_t>(doc.layerCount()));
    w.chunk(kChunkDoc, header);

    for (int i = 0; i < doc.layerCount(); ++i) {
        const Layer& layer = doc.layer(i);
        ByteWriter lw;
        const std::string name = layer.props.name.substr(0, kMaxNameLength);
        lw.u16(static_cast<std::uint16_t>(name.size()));
        lw.raw(name.data(), name.size());
        lw.u8(static_cast<std::uint8_t>((layer.props.visible ? 1 : 0) | (layer.props.locked ? 2 : 0)));
        lw.f32(static_cast<float>(layer.props.opacity));
        if (!writeTiles(lw, layer.image)) {
            return {false, "Compression failed"};
        }
        if (lw.bytes.size() > 0xFFFFFFFFull) {
            return {false, "Layer \"" + name + "\" is too large to save"};
        }
        w.chunk(kChunkLayer, lw);

        if (layer.mask) {
            ByteWriter mw;
            mw.u32(static_cast<std::uint32_t>(i));
            mw.u8(layer.props.maskEnabled ? 1 : 0);
            if (!writeTiles(mw, *layer.mask)) {
                return {false, "Compression failed"};
            }
            w.chunk(kChunkMask, mw);
        }
        if (layer.props.blendMode != BlendMode::Normal) {
            ByteWriter pw;
            pw.u32(static_cast<std::uint32_t>(i));
            pw.u8(static_cast<std::uint8_t>(layer.props.blendMode));
            w.chunk(kChunkLayerProps, pw);
        }
        if (!layer.props.effects.empty()) {
            ByteWriter ew;
            ew.u32(static_cast<std::uint32_t>(i));
            ew.u16(static_cast<std::uint16_t>(std::min<std::size_t>(layer.props.effects.size(), 0xFFFF)));
            for (std::size_t k = 0; k < layer.props.effects.size() && k < 0xFFFF; ++k) {
                const Effect& e = layer.props.effects[k];
                ew.u8(static_cast<std::uint8_t>(e.type));
                ew.u8(static_cast<std::uint8_t>((e.enabled ? 1 : 0) | (e.smooth ? 2 : 0)));
                ew.u8(e.color.r);
                ew.u8(e.color.g);
                ew.u8(e.color.b);
                ew.u8(e.color.a);
                const double values[] = {e.opacity, e.size,       e.offsetX,   e.offsetY,
                                         e.hue,     e.saturation, e.lightness, e.contrast};
                ew.u16(static_cast<std::uint16_t>(std::size(values)));
                for (double v : values) {
                    ew.f32(static_cast<float>(v));
                }
            }
            w.chunk(kChunkEffects, ew);
        }
    }
    if (!doc.slices().empty()) {
        ByteWriter sw;
        sw.u32(static_cast<std::uint32_t>(doc.slices().size()));
        for (const Slice& s : doc.slices()) {
            const std::size_t n = std::min<std::size_t>(s.name.size(), 0xFFFF);
            sw.u16(static_cast<std::uint16_t>(n));
            sw.raw(s.name.data(), n);
            for (int v : {s.rect.x, s.rect.y, s.rect.w, s.rect.h}) {
                sw.u32(static_cast<std::uint32_t>(v));
            }
            sw.f32(static_cast<float>(s.pivot.x));
            sw.f32(static_cast<float>(s.pivot.y));
        }
        w.chunk(kChunkSlices, sw);
    }
    w.chunk(kChunkEnd, ByteWriter{});
    w.u32(crc32Of(w.bytes.data(), w.bytes.size()));
    return writeFileAtomic(path, w.bytes);
}

// ------------------------------------------------------------ project load

LoadResult loadProject(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail("Cannot open the file");
    }
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return fail("Failed to read the file");
    }
    if (data.size() < kMagic.size() + 4 + 4 ||
        std::memcmp(data.data(), kMagic.data(), kMagic.size()) != 0) {
        return fail("Not a GameArt Studio project");
    }
    const std::size_t bodySize = data.size() - 4;
    ByteReader trailer(data.data() + bodySize, 4);
    if (trailer.u32() != crc32Of(data.data(), bodySize)) {
        return fail("The file is damaged (checksum mismatch)");
    }

    ByteReader r(data.data() + kMagic.size(), bodySize - kMagic.size());
    const std::uint32_t version = r.u32();
    if (version == 0) {
        return fail("Invalid project format version");
    }
    if (version > kProjectFormatVersion) {
        return fail("This project was saved by a newer version of GameArt Studio (format " +
                    std::to_string(version) + ")");
    }

    int width = 0, height = 0, active = 0;
    std::uint32_t expectedLayers = 0;
    bool haveHeader = false, haveEnd = false;
    std::vector<Layer> layers;
    std::vector<Slice> slices;

    while (r.remaining() > 0 && !haveEnd) {
        const std::uint32_t tag = r.u32();
        const std::uint32_t size = r.u32();
        const std::uint8_t* payload = r.bytes(size);
        if (!r.ok()) {
            return fail("The file is truncated");
        }
        ByteReader c(payload, size);

        if (tag == kChunkDoc) {
            const std::uint32_t w = c.u32(), h = c.u32(), a = c.u32(), n = c.u32();
            if (!c.ok() || haveHeader) {
                return fail("Invalid document header");
            }
            if (w == 0 || h == 0 || w > static_cast<std::uint32_t>(Document::kMaxSize) ||
                h > static_cast<std::uint32_t>(Document::kMaxSize)) {
                return fail("Invalid canvas size");
            }
            if (n == 0 || n > static_cast<std::uint32_t>(Document::kMaxLayers) || a >= n) {
                return fail("Invalid layer count");
            }
            width = static_cast<int>(w);
            height = static_cast<int>(h);
            active = static_cast<int>(a);
            expectedLayers = n;
            haveHeader = true;
        } else if (tag == kChunkLayer) {
            if (!haveHeader || layers.size() >= expectedLayers) {
                return fail("Unexpected layer data");
            }
            Layer layer(LayerProps{}, TiledImage(width, height));
            const std::uint16_t nameLength = c.u16();
            const std::uint8_t* name = c.bytes(nameLength);
            const std::uint8_t flags = c.u8();
            const float opacity = c.f32();
            if (!c.ok()) {
                return fail("Invalid layer record");
            }
            layer.props.name.assign(reinterpret_cast<const char*>(name), nameLength);
            layer.props.visible = (flags & 1) != 0;
            layer.props.locked = (flags & 2) != 0;
            layer.props.opacity = std::isfinite(opacity) ? std::clamp(static_cast<double>(opacity), 0.0, 1.0) : 1.0;
            if (std::string error = readTiles(c, layer.image); !error.empty()) {
                return fail(error);
            }
            layers.push_back(std::move(layer));
        } else if (tag == kChunkMask) {
            const std::uint32_t index = c.u32();
            const std::uint8_t flags = c.u8();
            if (!c.ok() || index >= layers.size() || layers[index].mask) {
                return fail("Invalid layer mask");
            }
            TiledImage mask(width, height);
            if (std::string error = readTiles(c, mask); !error.empty()) {
                return fail(error);
            }
            layers[index].mask = std::move(mask);
            layers[index].props.maskEnabled = (flags & 1) != 0;
        } else if (tag == kChunkLayerProps) {
            const std::uint32_t index = c.u32();
            const std::uint8_t mode = c.u8();
            if (!c.ok() || index >= layers.size()) {
                return fail("Invalid layer properties");
            }
            layers[index].props.blendMode = mode < kBlendModeCount ? static_cast<BlendMode>(mode) : BlendMode::Normal;
        } else if (tag == kChunkEffects) {
            const std::uint32_t index = c.u32();
            const std::uint16_t count = c.u16();
            if (!c.ok() || index >= layers.size()) {
                return fail("Invalid layer effects");
            }
            std::vector<Effect> effects;
            for (std::uint16_t k = 0; k < count; ++k) {
                Effect e;
                const std::uint8_t type = c.u8();
                const std::uint8_t flags = c.u8();
                e.color.r = c.u8();
                e.color.g = c.u8();
                e.color.b = c.u8();
                e.color.a = c.u8();
                const std::uint16_t floats = c.u16();
                double* fields[] = {&e.opacity, &e.size,       &e.offsetX,   &e.offsetY,
                                    &e.hue,     &e.saturation, &e.lightness, &e.contrast};
                for (std::uint16_t f = 0; f < floats; ++f) {
                    const float v = c.f32();
                    if (f < std::size(fields)) {
                        *fields[f] = v;
                    }
                }
                if (!c.ok()) {
                    return fail("Invalid layer effects");
                }
                if (type >= kEffectTypeCount) {
                    continue; // written by a newer version
                }
                e.type = static_cast<EffectType>(type);
                e.enabled = (flags & 1) != 0;
                e.smooth = (flags & 2) != 0;
                effects.push_back(sanitized(e));
            }
            layers[index].props.effects = std::move(effects);
        } else if (tag == kChunkSlices) {
            const std::uint32_t count = c.u32();
            if (!c.ok() || count > c.remaining()) {
                return fail("Invalid slices");
            }
            for (std::uint32_t k = 0; k < count; ++k) {
                Slice s;
                const std::uint16_t nameLength = c.u16();
                const std::uint8_t* name = c.bytes(nameLength);
                s.rect.x = static_cast<std::int32_t>(c.u32());
                s.rect.y = static_cast<std::int32_t>(c.u32());
                s.rect.w = static_cast<std::int32_t>(c.u32());
                s.rect.h = static_cast<std::int32_t>(c.u32());
                s.pivot.x = c.f32();
                s.pivot.y = c.f32();
                if (!c.ok()) {
                    return fail("Invalid slices");
                }
                s.name.assign(reinterpret_cast<const char*>(name), nameLength);
                slices.push_back(std::move(s));
            }
        } else if (tag == kChunkEnd) {
            haveEnd = true;
        }
        // Unknown chunks: skipped (forward compatibility).
    }

    if (!haveHeader || !haveEnd) {
        return fail("The file is incomplete");
    }
    if (layers.size() != expectedLayers) {
        return fail("Layer count does not match the header");
    }
    auto document = std::make_unique<Document>(width, height, std::move(layers), active);
    document->restoreSlices(std::move(slices)); // clipped/sanitised like any edit
    return {std::move(document), {}};
}

// ------------------------------------------------------------ PNG

std::vector<std::uint8_t> encodePng(int width, int height, const std::vector<Rgba8>& pixels) {
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    // Filter type 0 (None) on every row; zlib does the compression.
    std::vector<std::uint8_t> raw;
    raw.reserve(h * (1 + w * 4));
    for (std::size_t y = 0; y < h; ++y) {
        raw.push_back(0);
        const auto* row = reinterpret_cast<const std::uint8_t*>(pixels.data() + y * w);
        raw.insert(raw.end(), row, row + w * 4);
    }
    const std::vector<std::uint8_t> idat = deflate(raw.data(), raw.size());
    if (idat.empty()) {
        return {};
    }

    std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const auto be32 = [&out](std::uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<std::uint8_t>(v >> shift));
        }
    };
    const auto chunk = [&](const char (&type)[5], const std::vector<std::uint8_t>& body) {
        be32(static_cast<std::uint32_t>(body.size()));
        const std::size_t typeAt = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), body.begin(), body.end());
        be32(crc32Of(out.data() + typeAt, 4 + body.size())); // CRC covers type + data
    };

    std::vector<std::uint8_t> ihdr;
    for (const std::uint32_t v : {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)}) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            ihdr.push_back(static_cast<std::uint8_t>(v >> shift));
        }
    }
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit, RGBA, deflate, adaptive filter, no interlace
    chunk("IHDR", ihdr);
    chunk("IDAT", idat);
    chunk("IEND", {});
    return out;
}

IoResult exportPng(const Document& doc, const fs::path& path) {
    const std::vector<std::uint8_t> png = encodePng(doc.width(), doc.height(), doc.flatten());
    if (png.empty()) {
        return {false, "PNG encoding failed"};
    }
    return writeFileAtomic(path, png);
}

} // namespace gas
