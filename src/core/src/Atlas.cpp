#include "gas/core/Atlas.h"

#include "gas/core/Blend.h"
#include "gas/core/Document.h"
#include "gas/core/Slicing.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <optional>
#include <system_error>

namespace fs = std::filesystem;

namespace gas {

std::string_view atlasFormatName(AtlasFormat format) {
    static constexpr std::string_view names[kAtlasFormatCount] = {"JSON (hash)", "Cocos Creator plist",
                                                                  "Unity sprite sheet (.meta)"};
    const auto i = static_cast<int>(format);
    return i >= 0 && i < kAtlasFormatCount ? names[i] : names[0];
}

std::string_view atlasPresetName(AtlasPreset preset) {
    static constexpr std::string_view names[kAtlasPresetCount] = {"Unity", "Unity (pixel art)", "Cocos Creator",
                                                                  "Generic JSON"};
    const auto i = static_cast<int>(preset);
    return i >= 0 && i < kAtlasPresetCount ? names[i] : names[0];
}

AtlasSettings atlasPreset(AtlasPreset preset) {
    AtlasSettings s;
    switch (preset) {
    case AtlasPreset::Unity:
        s.format = AtlasFormat::UnityMeta;
        s.padding = 2;
        s.extrude = 1;
        break;
    case AtlasPreset::UnityPixelArt:
        s.format = AtlasFormat::UnityMeta;
        s.padding = 2;
        s.extrude = 0; // point filtering never samples outside the sprite
        s.pointFilter = true;
        s.pixelsPerUnit = 16;
        break;
    case AtlasPreset::CocosCreator:
        s.format = AtlasFormat::CocosPlist;
        s.padding = 2;
        s.extrude = 1;
        break;
    case AtlasPreset::Generic:
        s.format = AtlasFormat::JsonHash;
        s.padding = 2;
        s.powerOfTwo = false;
        break;
    }
    return s;
}

// ------------------------------------------------------------ MaxRects

namespace {

class MaxRects {
public:
    MaxRects(int width, int height) : free_{{0, 0, width, height}} {}

    // Best short side fit; returns the top-left of the placed rect.
    std::optional<Point> insert(int w, int h) {
        const IRect* best = nullptr;
        int bestShort = 0, bestLong = 0;
        for (const IRect& f : free_) {
            if (f.w < w || f.h < h) {
                continue;
            }
            const int dw = f.w - w, dh = f.h - h;
            const int s = std::min(dw, dh), l = std::max(dw, dh);
            if (!best || s < bestShort || (s == bestShort && l < bestLong) ||
                (s == bestShort && l == bestLong && (f.y < best->y || (f.y == best->y && f.x < best->x)))) {
                best = &f;
                bestShort = s;
                bestLong = l;
            }
        }
        if (!best) {
            return std::nullopt;
        }
        const IRect placed{best->x, best->y, w, h};
        split(placed);
        return Point{placed.x, placed.y};
    }

private:
    void split(const IRect& used) {
        std::vector<IRect> next;
        next.reserve(free_.size() + 4);
        for (const IRect& f : free_) {
            if (f.intersected(used).empty()) {
                next.push_back(f);
                continue;
            }
            if (used.x > f.x) {
                next.push_back({f.x, f.y, used.x - f.x, f.h});
            }
            if (used.right() < f.right()) {
                next.push_back({used.right(), f.y, f.right() - used.right(), f.h});
            }
            if (used.y > f.y) {
                next.push_back({f.x, f.y, f.w, used.y - f.y});
            }
            if (used.bottom() < f.bottom()) {
                next.push_back({f.x, used.bottom(), f.w, f.bottom() - used.bottom()});
            }
        }
        // Drop free rects contained in another one.
        free_.clear();
        for (std::size_t i = 0; i < next.size(); ++i) {
            bool contained = false;
            for (std::size_t j = 0; j < next.size() && !contained; ++j) {
                if (i == j) {
                    continue;
                }
                const IRect& a = next[i];
                const IRect& b = next[j];
                const bool inside = a.x >= b.x && a.y >= b.y && a.right() <= b.right() && a.bottom() <= b.bottom();
                contained = inside && (a != b || j < i); // keep one of two equal rects
            }
            if (!contained) {
                free_.push_back(next[i]);
            }
        }
    }

    std::vector<IRect> free_;
};

int nextPow2(int v) {
    int p = 1;
    while (p < v) {
        p *= 2;
    }
    return p;
}

struct Item {
    std::size_t sprite;
    int w, h; // occupied size in the bin (trimmed + extrusion + padding)
};

struct Placement {
    std::size_t item;
    Point pos;
};

// Packs `items` into a w x h bin; with `all`, fails unless every item fits.
std::vector<Placement> packBin(const std::vector<Item>& items, const std::vector<std::size_t>& order, int w, int h,
                               bool all, std::vector<std::size_t>* left) {
    MaxRects bin(w, h);
    std::vector<Placement> out;
    for (std::size_t k : order) {
        if (const auto p = bin.insert(items[k].w, items[k].h)) {
            out.push_back({k, *p});
        } else if (all) {
            return {};
        } else if (left) {
            left->push_back(k);
        }
    }
    return out;
}

} // namespace

Atlas packAtlas(const std::vector<AtlasSprite>& sprites, const AtlasSettings& in) {
    AtlasSettings s = in;
    s.maxWidth = std::clamp(s.maxWidth, 1, Document::kMaxSize);
    s.maxHeight = std::clamp(s.maxHeight, 1, Document::kMaxSize);
    s.padding = std::clamp(s.padding, 0, 256);
    s.border = std::clamp(s.border, 0, 256);
    s.extrude = std::clamp(s.extrude, 0, 64);

    Atlas atlas;
    atlas.frames.resize(sprites.size());
    std::vector<std::string> taken;
    std::vector<Item> items;
    for (std::size_t i = 0; i < sprites.size(); ++i) {
        const AtlasSprite& sp = sprites[i];
        AtlasFrame& f = atlas.frames[i];
        f.name = uniqueName(sp.name.empty() ? "sprite" : sp.name, taken);
        taken.push_back(f.name);
        f.sourceWidth = std::max(1, sp.width);
        f.sourceHeight = std::max(1, sp.height);
        f.pivot = sp.pivot;
        f.duration = sp.duration;
        f.source = {0, 0, f.sourceWidth, f.sourceHeight};
        if (s.trim && !s.grid && sp.width > 0 && sp.height > 0 &&
            sp.pixels.size() == static_cast<std::size_t>(sp.width) * static_cast<std::size_t>(sp.height)) {
            int x0 = sp.width, y0 = sp.height, x1 = -1, y1 = -1;
            for (int y = 0; y < sp.height; ++y) {
                for (int x = 0; x < sp.width; ++x) {
                    if (sp.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(sp.width) +
                                  static_cast<std::size_t>(x)].a > 0) {
                        x0 = std::min(x0, x);
                        x1 = std::max(x1, x);
                        y0 = std::min(y0, y);
                        y1 = std::max(y1, y);
                    }
                }
            }
            f.source = x1 < 0 ? IRect{0, 0, 1, 1} : IRect{x0, y0, x1 - x0 + 1, y1 - y0 + 1}; // empty -> 1x1
            f.trimmed = f.source != IRect{0, 0, f.sourceWidth, f.sourceHeight};
        }
        f.frame.w = f.source.w;
        f.frame.h = f.source.h;
        items.push_back({i, f.source.w + 2 * s.extrude + s.padding, f.source.h + 2 * s.extrude + s.padding});
    }

    // The bin is the page minus its borders, plus one padding (the last row/column needs none).
    const int binMaxW = s.maxWidth - 2 * s.border + s.padding;
    const int binMaxH = s.maxHeight - 2 * s.border + s.padding;
    for (const Item& it : items) {
        if (it.w > binMaxW || it.h > binMaxH) {
            const AtlasFrame& f = atlas.frames[it.sprite];
            atlas.error = "Sprite \"" + f.name + "\" (" + std::to_string(f.source.w) + "x" + std::to_string(f.source.h) +
                          ") does not fit on a " + std::to_string(s.maxWidth) + "x" + std::to_string(s.maxHeight) +
                          " page";
            atlas.frames.clear();
            return atlas;
        }
    }

    if (s.grid) {
        // Equal cells, input order, as many rows per page as fit.
        int cellW = 1, cellH = 1;
        for (const Item& it : items) {
            cellW = std::max(cellW, it.w);
            cellH = std::max(cellH, it.h);
        }
        const int fitCols = std::max(1, binMaxW / cellW), fitRows = std::max(1, binMaxH / cellH);
        const int n = static_cast<int>(items.size());
        int cols = s.columns > 0 ? s.columns : static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n))));
        cols = std::clamp(cols, 1, fitCols);
        const int perPage = cols * fitRows;
        for (int first = 0; first < n; first += perPage) {
            const int count = std::min(perPage, n - first);
            const int rows = (count + cols - 1) / cols;
            const int usedCols = std::min(cols, count);
            AtlasPage page;
            page.width = std::max(1, usedCols * cellW - s.padding + 2 * s.border);
            page.height = std::max(1, rows * cellH - s.padding + 2 * s.border);
            if (s.powerOfTwo) {
                page.width = std::min(nextPow2(page.width), s.maxWidth);
                page.height = std::min(nextPow2(page.height), s.maxHeight);
            }
            page.pixels.assign(static_cast<std::size_t>(page.width) * static_cast<std::size_t>(page.height), Rgba8{});
            for (int k = 0; k < count; ++k) {
                const std::size_t i = static_cast<std::size_t>(first + k);
                AtlasFrame& f = atlas.frames[i];
                const AtlasSprite& sp = sprites[i];
                f.page = static_cast<int>(atlas.pages.size());
                f.frame.x = s.border + (k % cols) * cellW + s.extrude;
                f.frame.y = s.border + (k / cols) * cellH + s.extrude;
                if (sp.pixels.size() != static_cast<std::size_t>(sp.width) * static_cast<std::size_t>(sp.height)) {
                    continue;
                }
                for (int y = -s.extrude; y < f.frame.h + s.extrude; ++y) {
                    const int sy = f.source.y + std::clamp(y, 0, f.frame.h - 1);
                    for (int x = -s.extrude; x < f.frame.w + s.extrude; ++x) {
                        const int sx = f.source.x + std::clamp(x, 0, f.frame.w - 1);
                        if (sx < sp.width && sy < sp.height) {
                            page.pixels[static_cast<std::size_t>(f.frame.y + y) * static_cast<std::size_t>(page.width) +
                                        static_cast<std::size_t>(f.frame.x + x)] =
                                sp.pixels[static_cast<std::size_t>(sy) * static_cast<std::size_t>(sp.width) +
                                          static_cast<std::size_t>(sx)];
                        }
                    }
                }
            }
            atlas.pages.push_back(std::move(page));
        }
        return atlas;
    }

    std::vector<std::size_t> remaining(items.size());
    std::iota(remaining.begin(), remaining.end(), std::size_t{0});
    std::stable_sort(remaining.begin(), remaining.end(), [&](std::size_t a, std::size_t b) {
        const int ma = std::max(items[a].w, items[a].h), mb = std::max(items[b].w, items[b].h);
        return ma != mb ? ma > mb : items[a].w * items[a].h > items[b].w * items[b].h;
    });

    const auto pageSide = [&](int binSide) { return binSide - s.padding + 2 * s.border; };
    const auto binSide = [&](int page) { return page - 2 * s.border + s.padding; };

    while (!remaining.empty()) {
        long long area = 0;
        int minW = 1, minH = 1;
        for (std::size_t k : remaining) {
            area += static_cast<long long>(items[k].w) * items[k].h;
            minW = std::max(minW, items[k].w);
            minH = std::max(minH, items[k].h);
        }
        // Candidate page sizes, smallest area first; the first that holds every sprite wins.
        std::vector<std::pair<int, int>> sizes; // bin sizes
        if (s.powerOfTwo) {
            for (int w = 1; w <= s.maxWidth; w *= 2) {
                for (int h = 1; h <= s.maxHeight; h *= 2) {
                    const int bw = binSide(w), bh = binSide(h);
                    if (bw >= minW && bh >= minH && static_cast<long long>(bw) * bh >= area) {
                        sizes.push_back({bw, bh});
                    }
                }
            }
        } else {
            const int start = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(area))));
            for (double side = start;; side = std::ceil(side * 1.05)) {
                const int bw = std::clamp(std::max(static_cast<int>(side), minW), 1, binMaxW);
                const int bh = std::clamp(std::max(static_cast<int>(side), minH), 1, binMaxH);
                sizes.push_back({bw, bh});
                if (bw == binMaxW && bh == binMaxH) {
                    break;
                }
                // Also try the full width (or height) with a shorter page, for long strips.
                if (bw == binMaxW || bh == binMaxH) {
                    sizes.push_back({binMaxW, bh});
                    sizes.push_back({bw, binMaxH});
                }
            }
        }
        std::stable_sort(sizes.begin(), sizes.end(), [](auto a, auto b) {
            const long long aa = static_cast<long long>(a.first) * a.second;
            const long long bb = static_cast<long long>(b.first) * b.second;
            return aa != bb ? aa < bb : std::abs(a.first - a.second) < std::abs(b.first - b.second);
        });
        std::vector<Placement> placed;
        std::vector<std::size_t> left;
        for (const auto& [bw, bh] : sizes) {
            placed = packBin(items, remaining, bw, bh, true, nullptr);
            if (!placed.empty()) {
                break;
            }
        }
        if (placed.empty()) {
            // Too many for one page: fill a full-size page, the rest go on the next ones.
            placed = packBin(items, remaining, binMaxW, binMaxH, false, &left);
        }

        // Shrink the page to what was used.
        int usedW = 1, usedH = 1;
        for (const Placement& p : placed) {
            usedW = std::max(usedW, p.pos.x + items[p.item].w);
            usedH = std::max(usedH, p.pos.y + items[p.item].h);
        }
        AtlasPage page;
        page.width = std::max(1, pageSide(usedW));
        page.height = std::max(1, pageSide(usedH));
        if (s.powerOfTwo) {
            page.width = std::min(nextPow2(page.width), s.maxWidth);
            page.height = std::min(nextPow2(page.height), s.maxHeight);
        }
        page.pixels.assign(static_cast<std::size_t>(page.width) * static_cast<std::size_t>(page.height), Rgba8{});
        const int pageIndex = static_cast<int>(atlas.pages.size());
        for (const Placement& p : placed) {
            const std::size_t i = items[p.item].sprite;
            AtlasFrame& f = atlas.frames[i];
            const AtlasSprite& sp = sprites[i];
            f.page = pageIndex;
            f.frame.x = s.border + p.pos.x + s.extrude;
            f.frame.y = s.border + p.pos.y + s.extrude;
            const bool hasPixels =
                sp.pixels.size() == static_cast<std::size_t>(sp.width) * static_cast<std::size_t>(sp.height) &&
                sp.width > 0 && sp.height > 0;
            if (!hasPixels) {
                continue;
            }
            // Copy the trimmed pixels; extruded pixels repeat the nearest edge pixel.
            for (int y = -s.extrude; y < f.frame.h + s.extrude; ++y) {
                const int sy = f.source.y + std::clamp(y, 0, f.frame.h - 1);
                for (int x = -s.extrude; x < f.frame.w + s.extrude; ++x) {
                    const int sx = f.source.x + std::clamp(x, 0, f.frame.w - 1);
                    if (sx >= sp.width || sy >= sp.height) {
                        continue;
                    }
                    page.pixels[static_cast<std::size_t>(f.frame.y + y) * static_cast<std::size_t>(page.width) +
                                static_cast<std::size_t>(f.frame.x + x)] =
                        sp.pixels[static_cast<std::size_t>(sy) * static_cast<std::size_t>(sp.width) +
                                  static_cast<std::size_t>(sx)];
                }
            }
        }
        atlas.pages.push_back(std::move(page));
        remaining = std::move(left);
    }
    return atlas;
}

std::vector<AtlasSprite> spritesFromSlices(const Document& document) {
    const TiledImage image = document.flattenedImage();
    std::vector<AtlasSprite> out;
    for (const Slice& slice : document.slices()) {
        out.push_back({slice.name, slice.rect.w, slice.rect.h, copyPixels(image, slice.rect), slice.pivot});
    }
    return out;
}

std::vector<AtlasSprite> spritesFromFrames(const Document& document, const std::string& baseName, PointF pivot) {
    std::vector<AtlasSprite> out;
    const int n = document.frameCount();
    const std::size_t digits = std::max<std::size_t>(2, std::to_string(n - 1).size());
    const auto number = [digits](int f) {
        std::string s = std::to_string(f);
        s.insert(0, digits - std::min(digits, s.size()), '0');
        return s;
    };
    std::vector<TiledImage> frames;
    for (int f = 0; f < n; ++f) {
        frames.push_back(document.flattenedFrame(f));
    }
    const auto duration = [&](int f) { return document.frames()[static_cast<std::size_t>(f)].duration; };
    if (document.slices().empty()) {
        const IRect all{0, 0, document.width(), document.height()};
        for (int f = 0; f < n; ++f) {
            out.push_back({baseName + "_" + number(f), all.w, all.h, copyPixels(frames[static_cast<std::size_t>(f)], all),
                           pivot, duration(f)});
        }
        return out;
    }
    for (const Slice& slice : document.slices()) {
        for (int f = 0; f < n; ++f) {
            out.push_back({slice.name + "_" + number(f), slice.rect.w, slice.rect.h,
                           copyPixels(frames[static_cast<std::size_t>(f)], slice.rect), slice.pivot, duration(f)});
        }
    }
    return out;
}

std::vector<FrameTag> tagsForFrameSprites(const Document& document) {
    if (document.slices().empty()) {
        return document.tags();
    }
    std::vector<FrameTag> out;
    const int n = document.frameCount();
    for (std::size_t k = 0; k < document.slices().size(); ++k) {
        const int base = static_cast<int>(k) * n;
        for (const FrameTag& t : document.tags()) {
            out.push_back({document.slices()[k].name + "_" + t.name, base + t.from, base + t.to, t.direction});
        }
    }
    return out;
}

// ------------------------------------------------------------ metadata

namespace {

std::string num(double v) {
    if (std::abs(v - std::round(v)) < 1e-9) {
        return std::to_string(static_cast<long long>(std::llround(v)));
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    std::string s = buf;
    std::replace(s.begin(), s.end(), ',', '.'); // in case of a comma-decimal C locale
    return s;
}

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out += ch;
        }
    }
    return out + "\"";
}

std::string xmlText(const std::string& s) {
    std::string out;
    for (const char ch : s) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += ch;
        }
    }
    return out;
}

// Plain YAML scalar when safe, otherwise single-quoted.
std::string yamlString(const std::string& s) {
    const bool plain = !s.empty() && std::all_of(s.begin(), s.end(), [](char ch) {
        const auto c = static_cast<unsigned char>(ch);
        return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c >= 0x80;
    }) && !std::isdigit(static_cast<unsigned char>(s[0])) && s[0] != '-' && s[0] != '.';
    if (plain) {
        return s;
    }
    std::string out = "'";
    for (const char ch : s) {
        out += ch;
        if (ch == '\'') {
            out += '\'';
        }
    }
    return out + "'";
}

std::uint64_t splitMix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

std::uint64_t hashOf(const std::string& s, std::uint64_t seed) {
    std::uint64_t h = 1469598103934665603ull ^ seed; // FNV-1a, then mixed
    for (const char ch : s) {
        h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
    }
    return splitMix(h);
}

// 32 hex digits, stable for the same text, so re-exports keep Unity's references.
std::string hex128(const std::string& s) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", static_cast<unsigned long long>(hashOf(s, 1)),
                  static_cast<unsigned long long>(hashOf(s, 2)));
    return buf;
}

std::vector<std::size_t> framesOnPage(const Atlas& atlas, int page) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < atlas.frames.size(); ++i) {
        if (atlas.frames[i].page == page) {
            out.push_back(i);
        }
    }
    return out;
}

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

} // namespace

std::string atlasJson(const Atlas& atlas, int page, const std::string& imageFile) {
    const AtlasPage& pg = atlas.pages.at(static_cast<std::size_t>(page));
    std::string o = "{\n  \"frames\": {\n";
    const std::vector<std::size_t> frames = framesOnPage(atlas, page);
    for (std::size_t k = 0; k < frames.size(); ++k) {
        const AtlasFrame& f = atlas.frames[frames[k]];
        o += "    " + jsonString(f.name) + ": {\n";
        o += "      \"frame\": {\"x\": " + num(f.frame.x) + ", \"y\": " + num(f.frame.y) + ", \"w\": " + num(f.frame.w) +
             ", \"h\": " + num(f.frame.h) + "},\n";
        o += "      \"rotated\": false,\n";
        o += std::string("      \"trimmed\": ") + (f.trimmed ? "true" : "false") + ",\n";
        o += "      \"spriteSourceSize\": {\"x\": " + num(f.source.x) + ", \"y\": " + num(f.source.y) +
             ", \"w\": " + num(f.source.w) + ", \"h\": " + num(f.source.h) + "},\n";
        o += "      \"sourceSize\": {\"w\": " + num(f.sourceWidth) + ", \"h\": " + num(f.sourceHeight) + "},\n";
        o += "      \"pivot\": {\"x\": " + num(f.pivot.x) + ", \"y\": " + num(f.pivot.y) + "}";
        o += f.duration > 0 ? ",\n      \"duration\": " + num(f.duration) + "\n" : "\n";
        o += k + 1 < frames.size() ? "    },\n" : "    }\n";
    }
    o += "  },\n  \"meta\": {\n";
    o += "    \"app\": \"GameArt Studio\",\n    \"version\": \"1.0\",\n";
    o += "    \"image\": " + jsonString(imageFile) + ",\n";
    o += "    \"format\": \"RGBA8888\",\n";
    o += "    \"size\": {\"w\": " + num(pg.width) + ", \"h\": " + num(pg.height) + "},\n";
    o += "    \"scale\": \"1\"";
    if (!atlas.tags.empty()) {
        static constexpr const char* kDirections[kTagDirectionCount] = {"forward", "reverse", "pingpong"};
        o += ",\n    \"frameTags\": [\n";
        for (std::size_t k = 0; k < atlas.tags.size(); ++k) {
            const FrameTag& t = atlas.tags[k];
            o += "      {\"name\": " + jsonString(t.name) + ", \"from\": " + num(t.from) + ", \"to\": " + num(t.to) +
                 ", \"direction\": \"" + kDirections[static_cast<int>(t.direction)] + "\"}";
            o += k + 1 < atlas.tags.size() ? ",\n" : "\n";
        }
        o += "    ]";
    }
    o += "\n  }\n}\n";
    return o;
}

std::string cocosPlist(const Atlas& atlas, int page, const std::string& imageFile) {
    const AtlasPage& pg = atlas.pages.at(static_cast<std::size_t>(page));
    const auto rect = [](int x, int y, int w, int h) {
        return "{{" + num(x) + "," + num(y) + "},{" + num(w) + "," + num(h) + "}}";
    };
    std::string o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                    "<plist version=\"1.0\">\n<dict>\n  <key>frames</key>\n  <dict>\n";
    for (std::size_t i : framesOnPage(atlas, page)) {
        const AtlasFrame& f = atlas.frames[i];
        // Offset of the trimmed rect's centre from the source centre, y up.
        const double ox = f.source.x + f.source.w / 2.0 - f.sourceWidth / 2.0;
        const double oy = f.sourceHeight / 2.0 - (f.source.y + f.source.h / 2.0);
        o += "    <key>" + xmlText(f.name) + "</key>\n    <dict>\n";
        o += "      <key>frame</key>\n      <string>" + rect(f.frame.x, f.frame.y, f.frame.w, f.frame.h) + "</string>\n";
        o += "      <key>offset</key>\n      <string>{" + num(ox) + "," + num(oy) + "}</string>\n";
        o += "      <key>rotated</key>\n      <false/>\n";
        o += "      <key>sourceColorRect</key>\n      <string>" + rect(f.source.x, f.source.y, f.source.w, f.source.h) +
             "</string>\n";
        o += "      <key>sourceSize</key>\n      <string>{" + num(f.sourceWidth) + "," + num(f.sourceHeight) +
             "}</string>\n";
        o += "    </dict>\n";
    }
    o += "  </dict>\n  <key>metadata</key>\n  <dict>\n";
    o += "    <key>format</key>\n    <integer>2</integer>\n";
    o += "    <key>realTextureFileName</key>\n    <string>" + xmlText(imageFile) + "</string>\n";
    o += "    <key>size</key>\n    <string>{" + num(pg.width) + "," + num(pg.height) + "}</string>\n";
    o += "    <key>textureFileName</key>\n    <string>" + xmlText(imageFile) + "</string>\n";
    o += "  </dict>\n</dict>\n</plist>\n";
    return o;
}

std::string unityMeta(const Atlas& atlas, int page, const AtlasSettings& s, const std::string& imageFile) {
    const AtlasPage& pg = atlas.pages.at(static_cast<std::size_t>(page));
    const std::vector<std::size_t> frames = framesOnPage(atlas, page);
    const int maxSize = std::clamp(nextPow2(std::max({pg.width, pg.height, 32})), 32, 16384);
    const std::string filter = s.pointFilter ? "0" : "1";
    const std::string compression = s.pointFilter ? "0" : "1";
    const auto internalId = [](std::size_t k) { return std::to_string(21300000 + 2 * static_cast<long long>(k)); };

    std::string o = "fileFormatVersion: 2\nguid: " + hex128("gas-atlas:" + imageFile) + "\nTextureImporter:\n";
    o += "  internalIDToNameTable:\n";
    for (std::size_t k = 0; k < frames.size(); ++k) {
        o += "  - first:\n      213: " + internalId(k) + "\n    second: " + yamlString(atlas.frames[frames[k]].name) + "\n";
    }
    o += "  externalObjects: {}\n  serializedVersion: 12\n";
    o += "  mipmaps:\n    mipMapMode: 0\n    enableMipMap: 0\n    sRGBTexture: 1\n    linearTexture: 0\n";
    o += "  isReadable: 0\n  streamingMipmaps: 0\n";
    o += "  textureFormat: 1\n  maxTextureSize: " + std::to_string(maxSize) + "\n";
    o += "  textureSettings:\n    serializedVersion: 2\n    filterMode: " + filter +
         "\n    aniso: 1\n    mipBias: 0\n    wrapU: 1\n    wrapV: 1\n    wrapW: 1\n";
    o += "  nPOTScale: 0\n  lightmap: 0\n  compressionQuality: 50\n";
    o += "  spriteMode: 2\n  spriteExtrude: 1\n  spriteMeshType: 1\n  alignment: 0\n";
    o += "  spritePivot: {x: 0.5, y: 0.5}\n  spritePixelsToUnits: " + std::to_string(std::max(1, s.pixelsPerUnit)) + "\n";
    o += "  spriteBorder: {x: 0, y: 0, z: 0, w: 0}\n  spriteGenerateFallbackPhysicsShape: 1\n";
    o += "  alphaUsage: 1\n  alphaIsTransparency: 1\n  spriteTessellationDetail: -1\n";
    o += "  textureType: 8\n  textureShape: 1\n  singleChannelComponent: 0\n";
    o += "  platformSettings:\n  - serializedVersion: 3\n    buildTarget: DefaultTexturePlatform\n";
    o += "    maxTextureSize: " + std::to_string(maxSize) + "\n    resizeAlgorithm: 0\n    textureFormat: -1\n";
    o += "    textureCompression: " + compression + "\n    compressionQuality: 50\n    crunchedCompression: 0\n";
    o += "    allowsAlphaSplitting: 0\n    overridden: 0\n";
    o += "  spriteSheet:\n    serializedVersion: 2\n    sprites:\n";
    for (std::size_t k = 0; k < frames.size(); ++k) {
        const AtlasFrame& f = atlas.frames[frames[k]];
        // Unity rects start at the bottom-left; the pivot is relative to the
        // packed (trimmed) rect with y up, so it stays on the same source pixel.
        const double px = (f.pivot.x * f.sourceWidth - f.source.x) / f.frame.w;
        const double py = 1.0 - (f.pivot.y * f.sourceHeight - f.source.y) / f.frame.h;
        o += "    - serializedVersion: 2\n      name: " + yamlString(f.name) + "\n";
        o += "      rect:\n        serializedVersion: 2\n        x: " + num(f.frame.x) +
             "\n        y: " + num(pg.height - f.frame.y - f.frame.h) + "\n        width: " + num(f.frame.w) +
             "\n        height: " + num(f.frame.h) + "\n";
        o += "      alignment: 9\n      pivot: {x: " + num(px) + ", y: " + num(py) + "}\n";
        o += "      border: {x: 0, y: 0, z: 0, w: 0}\n      outline: []\n      physicsShape: []\n";
        o += "      tessellationDetail: 0\n      bones: []\n";
        o += "      spriteID: " + hex128("gas-sprite:" + imageFile + "/" + f.name) + "\n";
        o += "      internalID: " + internalId(k) + "\n";
        o += "      vertices: []\n      indices: \n      edges: []\n      weights: []\n";
    }
    o += "    outline: []\n    physicsShape: []\n    bones: []\n    spriteID: \n    internalID: 0\n";
    o += "    vertices: []\n    indices: \n    edges: []\n    weights: []\n    secondaryTextures: []\n";
    o += "    nameFileIdTable:\n";
    for (std::size_t k = 0; k < frames.size(); ++k) {
        o += "      " + yamlString(atlas.frames[frames[k]].name) + ": " + internalId(k) + "\n";
    }
    o += "  spritePackingTag: \n  pSDRemoveMatte: 0\n  userData: \n  assetBundleName: \n  assetBundleVariant: \n";
    return o;
}

// ------------------------------------------------------------ files

fs::path atlasPagePath(const fs::path& pngPath, int page, int pageCount) {
    if (pageCount <= 1) {
        return pngPath;
    }
    fs::path p = pngPath;
    p.replace_filename(fromUtf8(utf8(pngPath.stem()) + "_" + std::to_string(page) + ".png"));
    return p;
}

namespace {

IoResult writeText(const fs::path& path, const std::string& text, std::vector<fs::path>* written) {
    IoResult r = writeFileAtomic(path, std::vector<std::uint8_t>(text.begin(), text.end()));
    if (r.ok && written) {
        written->push_back(path);
    }
    return r;
}

IoResult writePng(const fs::path& path, int w, int h, const std::vector<Rgba8>& pixels, std::vector<fs::path>* written) {
    const std::vector<std::uint8_t> png = encodePng(w, h, pixels);
    if (png.empty()) {
        return {false, "PNG encoding failed"};
    }
    IoResult r = writeFileAtomic(path, png);
    if (r.ok && written) {
        written->push_back(path);
    }
    return r;
}

IoResult makeFolder(const fs::path& folder) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec || !fs::is_directory(folder)) {
        return {false, "Cannot create the folder: " + ec.message()};
    }
    return {true, {}};
}

} // namespace

IoResult exportAtlas(const Atlas& atlas, const AtlasSettings& settings, const fs::path& pngPath,
                     std::vector<fs::path>* written) {
    if (!atlas.ok()) {
        return {false, atlas.error};
    }
    if (atlas.pages.empty()) {
        return {false, "Nothing to export (no slices)"};
    }
    if (pngPath.has_parent_path()) {
        if (IoResult r = makeFolder(pngPath.parent_path()); !r.ok) {
            return r;
        }
    }
    const int count = static_cast<int>(atlas.pages.size());
    for (int i = 0; i < count; ++i) {
        const AtlasPage& pg = atlas.pages[static_cast<std::size_t>(i)];
        const fs::path png = atlasPagePath(pngPath, i, count);
        if (IoResult r = writePng(png, pg.width, pg.height, pg.pixels, written); !r.ok) {
            return r;
        }
        const std::string imageFile = utf8(png.filename());
        IoResult r;
        switch (settings.format) {
        case AtlasFormat::JsonHash: {
            fs::path meta = png;
            r = writeText(meta.replace_extension(".json"), atlasJson(atlas, i, imageFile), written);
            break;
        }
        case AtlasFormat::CocosPlist: {
            fs::path meta = png;
            r = writeText(meta.replace_extension(".plist"), cocosPlist(atlas, i, imageFile), written);
            break;
        }
        case AtlasFormat::UnityMeta: {
            fs::path meta = png;
            meta += ".meta";
            r = writeText(meta, unityMeta(atlas, i, settings, imageFile), written);
            break;
        }
        }
        if (!r.ok) {
            return r;
        }
    }
    return {true, {}};
}

IoResult exportFramePngs(const Document& document, const fs::path& folder, const std::string& baseName, int scale,
                         std::vector<fs::path>* written) {
    if (IoResult r = makeFolder(folder); !r.ok) {
        return r;
    }
    scale = std::clamp(scale, 1, 16);
    const int n = document.frameCount();
    const std::size_t digits = std::max<std::size_t>(2, std::to_string(n - 1).size());
    const int w = document.width() * scale, h = document.height() * scale;
    for (int f = 0; f < n; ++f) {
        const std::vector<Rgba8> src = copyPixels(document.flattenedFrame(f), {0, 0, document.width(), document.height()});
        std::vector<Rgba8> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                out[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
                    src[static_cast<std::size_t>(y / scale) * static_cast<std::size_t>(document.width()) +
                        static_cast<std::size_t>(x / scale)];
            }
        }
        std::string number = std::to_string(f);
        number.insert(0, digits - std::min(digits, number.size()), '0');
        if (IoResult r = writePng(folder / fromUtf8(safeFileName(baseName) + "_" + number + ".png"), w, h, out, written);
            !r.ok) {
            return r;
        }
    }
    return {true, {}};
}

std::string safeFileName(const std::string& name) {
    std::string out;
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        out += (c < 0x20 || std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos) ? '_' : ch;
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) {
        out.pop_back(); // Windows strips these
    }
    return out.empty() ? "sprite" : out;
}

IoResult exportSlicePngs(const Document& document, const fs::path& folder, bool trim, int scale,
                         std::vector<fs::path>* written) {
    if (document.slices().empty()) {
        return {false, "Nothing to export (no slices)"};
    }
    if (IoResult r = makeFolder(folder); !r.ok) {
        return r;
    }
    scale = std::clamp(scale, 1, 16);
    const TiledImage image = document.flattenedImage();
    std::vector<std::string> taken;
    for (const Slice& slice : document.slices()) {
        IRect rect = slice.rect;
        if (trim) {
            const IRect t = trimmedRect(image, rect);
            rect = t.empty() ? IRect{rect.x, rect.y, 1, 1} : t;
        }
        const std::vector<Rgba8> src = copyPixels(image, rect);
        const int w = rect.w * scale, h = rect.h * scale;
        std::vector<Rgba8> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                out[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
                    src[static_cast<std::size_t>(y / scale) * static_cast<std::size_t>(rect.w) +
                        static_cast<std::size_t>(x / scale)];
            }
        }
        // Case-insensitive uniqueness: Windows file names ignore case.
        std::string lower = safeFileName(slice.name);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        const std::string unique = uniqueName(lower, taken);
        taken.push_back(unique);
        const std::string fileName = safeFileName(slice.name) + unique.substr(lower.size()) + ".png";
        if (IoResult r = writePng(folder / fromUtf8(fileName), w, h, out, written); !r.ok) {
            return r;
        }
    }
    return {true, {}};
}

IoResult exportLayerPngs(const Document& document, const fs::path& folder, std::vector<fs::path>* written) {
    if (IoResult r = makeFolder(folder); !r.ok) {
        return r;
    }
    std::vector<std::string> taken;
    const auto w = static_cast<std::size_t>(document.width());
    for (int i = 0; i < document.layerCount(); ++i) {
        const Layer& layer = document.layer(i);
        const TiledImage& img = document.renderedLayer(i);
        // renderedLayer() includes the mask only when it renders effects.
        const TiledImage* mask = (&img == &layer.image && layer.mask && layer.props.maskEnabled) ? &*layer.mask : nullptr;
        const double opacity = layer.props.opacity;
        std::vector<Rgba8> pixels(w * static_cast<std::size_t>(document.height()));
        for (int y = 0; y < document.height(); ++y) {
            for (int x = 0; x < document.width(); ++x) {
                Rgba8 p = img.pixel({x, y});
                const double m = mask ? maskValue(mask->pixel({x, y})) / 255.0 : 1.0;
                p.a = static_cast<std::uint8_t>(std::lround(p.a * opacity * m));
                if (p.a == 0) {
                    p = {};
                }
                pixels[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] = p;
            }
        }
        std::string lower = safeFileName(document.layer(i).props.name);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        const std::string unique = uniqueName(lower, taken);
        taken.push_back(unique);
        const std::string fileName =
            safeFileName(document.layer(i).props.name) + unique.substr(lower.size()) + ".png";
        if (IoResult r = writePng(folder / fromUtf8(fileName), document.width(), document.height(), pixels, written);
            !r.ok) {
            return r;
        }
    }
    return {true, {}};
}

} // namespace gas
