#include "gas/core/BrushPresets.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>

namespace gas {

namespace {

struct NumberField {
    const char* key;
    double BrushSettings::*member;
    double min, max;
};

constexpr NumberField kNumberFields[] = {
    {"diameter", &BrushSettings::diameter, 1.0, BrushSettings::kMaxDiameter},
    {"opacity", &BrushSettings::opacity, 0.0, 1.0},
    {"hardness", &BrushSettings::hardness, 0.0, 1.0},
    {"spacing", &BrushSettings::spacing, 0.01, 10.0},
    {"roundness", &BrushSettings::roundness, 0.05, 1.0},
    {"angle", &BrushSettings::angle, -360.0, 360.0},
    {"pressure_size", &BrushSettings::pressureSize, 0.0, 1.0},
    {"pressure_opacity", &BrushSettings::pressureOpacity, 0.0, 1.0},
    {"size_jitter", &BrushSettings::sizeJitter, 0.0, 1.0},
    {"opacity_jitter", &BrushSettings::opacityJitter, 0.0, 1.0},
    {"angle_jitter", &BrushSettings::angleJitter, 0.0, 1.0},
    {"scatter", &BrushSettings::scatter, 0.0, 4.0},
    {"smoothing", &BrushSettings::smoothing, 0.0, 1.0},
    {"grain", &BrushSettings::grain, 0.0, 1.0},
    {"grain_scale", &BrushSettings::grainScale, 0.5, 64.0},
    {"particles", &BrushSettings::particles, 0.0, 1.0},
    {"mix", &BrushSettings::mix, 0.0, 1.0},
};

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r");
    return s.substr(first, last - first + 1);
}

std::optional<double> parseNumber(std::string_view s) {
    double v = 0.0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || ptr != s.data() + s.size() || !std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string toBase64(const std::vector<std::uint8_t>& data) {
    std::string out;
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::uint32_t n = (std::uint32_t{data[i]} << 16) |
                                (i + 1 < data.size() ? std::uint32_t{data[i + 1]} << 8 : 0) |
                                (i + 2 < data.size() ? std::uint32_t{data[i + 2]} : 0);
        out += kBase64[(n >> 18) & 63];
        out += kBase64[(n >> 12) & 63];
        out += i + 1 < data.size() ? kBase64[(n >> 6) & 63] : '=';
        out += i + 2 < data.size() ? kBase64[n & 63] : '=';
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> fromBase64(std::string_view s) {
    std::vector<std::uint8_t> out;
    std::uint32_t acc = 0;
    int bits = 0;
    for (char ch : s) {
        if (ch == '=') {
            break;
        }
        const char* p = std::char_traits<char>::find(kBase64, 64, ch);
        if (!p) {
            return std::nullopt;
        }
        acc = (acc << 6) | static_cast<std::uint32_t>(p - kBase64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>(acc >> bits));
        }
    }
    return out;
}

// "W H base64" -> tip; nullopt if malformed.
std::optional<BrushTip> parseTip(std::string_view s) {
    const auto sp1 = s.find(' ');
    const auto sp2 = s.find(' ', sp1 == std::string_view::npos ? sp1 : sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
        return std::nullopt;
    }
    int w = 0, h = 0;
    const std::string_view ws = s.substr(0, sp1), hs = s.substr(sp1 + 1, sp2 - sp1 - 1);
    if (std::from_chars(ws.data(), ws.data() + ws.size(), w).ec != std::errc() ||
        std::from_chars(hs.data(), hs.data() + hs.size(), h).ec != std::errc() || w <= 0 || h <= 0 ||
        w > BrushTip::kMaxSize || h > BrushTip::kMaxSize) {
        return std::nullopt;
    }
    auto data = fromBase64(s.substr(sp2 + 1));
    if (!data || data->size() != static_cast<std::size_t>(w) * static_cast<std::size_t>(h)) {
        return std::nullopt;
    }
    return BrushTip{w, h, std::move(*data)};
}

std::string formatNumber(double v) {
    char buf[32];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v); // shortest round-trip form
    return ec == std::errc() ? std::string(buf, ptr) : std::string("0");
}

BrushPreset make(std::string name, BrushSettings s) { return {std::move(name), s}; }

} // namespace

const std::vector<BrushPreset>& builtinBrushPresets() {
    static const std::vector<BrushPreset> presets = [] {
        std::vector<BrushPreset> p;
        BrushSettings s;

        // The everyday media first (like a paint program's brush menu).
        s = {};
        s.diameter = 12;
        s.hardness = 0.8;
        p.push_back(make("Brush", s));

        s = {};
        s.diameter = 22;
        s.hardness = 0.7;
        s.spacing = 0.04;
        s.roundness = 0.25;
        s.angle = 45;
        s.pressureSize = 0.6;
        s.smoothing = 0.3;
        p.push_back(make("Calligraphy Brush", s));

        s = {};
        s.diameter = 14;
        s.hardness = 1.0;
        s.spacing = 0.03;
        s.roundness = 0.12;
        s.angle = 45;
        s.smoothing = 0.3;
        p.push_back(make("Calligraphy Pen", s));

        s = {};
        s.diameter = 40;
        s.hardness = 0.0;
        s.spacing = 0.1;
        s.particles = 0.08;
        p.push_back(make("Airbrush", s));

        s = {};
        s.shape = BrushShape::Bristle;
        s.followDirection = true;
        s.diameter = 26;
        s.hardness = 0.6;
        s.spacing = 0.04;
        s.mix = 0.2;
        p.push_back(make("Oil Brush", s));

        s = {};
        s.diameter = 16;
        s.hardness = 0.9;
        s.spacing = 0.05;
        s.grain = 0.85;
        s.grainScale = 2.5;
        p.push_back(make("Crayon", s));

        s = {};
        s.diameter = 18;
        s.hardness = 0.95;
        s.opacity = 0.8;
        s.spacing = 0.04;
        s.roundness = 0.6;
        s.angle = 30;
        p.push_back(make("Marker", s));

        s = {};
        s.diameter = 4;
        s.hardness = 0.7;
        s.opacity = 0.85;
        s.spacing = 0.05;
        s.grain = 0.6;
        s.grainScale = 1.2;
        s.pressureOpacity = 0.8;
        p.push_back(make("Natural Pencil", s));

        s = {};
        s.diameter = 36;
        s.hardness = 0.1;
        s.opacity = 0.45;
        s.spacing = 0.06;
        s.mix = 0.35;
        s.grain = 0.25;
        s.grainScale = 6;
        p.push_back(make("Watercolor", s));

        s = {};
        s.diameter = 12;
        s.hardness = 1.0;
        p.push_back(make("Hard Round", s));

        s = {};
        s.diameter = 40;
        s.hardness = 0.0;
        p.push_back(make("Soft Round", s));

        s = {};
        s.diameter = 8;
        s.hardness = 0.9;
        s.spacing = 0.05;
        s.pressureSize = 1.0;
        s.smoothing = 0.5;
        p.push_back(make("Ink Pen", s));

        s = {};
        s.diameter = 4;
        s.hardness = 0.5;
        s.opacity = 0.8;
        s.pressureOpacity = 0.8;
        s.opacityJitter = 0.3;
        p.push_back(make("Sketch Pencil", s));

        s = {};
        s.diameter = 6;
        s.hardness = 0.6;
        s.spacing = 0.6;
        s.scatter = 2.0;
        s.sizeJitter = 0.7;
        s.opacityJitter = 0.5;
        p.push_back(make("Spray", s));

        s = {};
        s.shape = BrushShape::Square;
        s.diameter = 16;
        s.hardness = 1.0;
        s.spacing = 0.05;
        p.push_back(make("Square Marker", s));

        s = {};
        s.shape = BrushShape::Star;
        s.diameter = 18;
        s.hardness = 0.9;
        s.spacing = 1.2;
        s.scatter = 1.5;
        s.sizeJitter = 0.5;
        s.angleJitter = 1.0;
        p.push_back(make("Star Scatter", s));

        s = {};
        s.diameter = 24;
        s.hardness = 0.7;
        s.spacing = 0.15;
        s.roundness = 0.6;
        s.angleJitter = 1.0;
        s.sizeJitter = 0.3;
        s.opacityJitter = 0.6;
        p.push_back(make("Chalk", s));
        return p;
    }();
    return presets;
}

std::string serializeBrushPresets(const std::vector<BrushPreset>& presets) {
    std::string out = "# GameArt Studio brush presets\n";
    for (const BrushPreset& p : presets) {
        out += "\n[preset]\nname = " + p.name + "\n";
        for (const NumberField& f : kNumberFields) {
            out += std::string(f.key) + " = " + formatNumber(p.settings.*f.member) + "\n";
        }
        out += std::string("follow_direction = ") + (p.settings.followDirection ? "1" : "0") + "\n";
        out += "blend_mode = " + std::to_string(static_cast<int>(p.settings.blendMode)) + "\n";
        out += "shape = " + std::to_string(static_cast<int>(p.settings.shape)) + "\n";
        if (const BrushTip* tip = p.settings.tip.tip.get(); tip && !tip->empty()) {
            out += "tip = " + std::to_string(tip->width) + " " + std::to_string(tip->height) + " " +
                   toBase64(tip->alpha) + "\n";
        }
    }
    return out;
}

std::vector<BrushPreset> parseBrushPresets(std::string_view text) {
    std::vector<BrushPreset> result;
    std::optional<BrushPreset> current;
    const auto flush = [&] {
        if (current && !current->name.empty()) {
            result.push_back(std::move(*current));
        }
        current.reset();
    };
    while (!text.empty()) {
        const auto eol = text.find('\n');
        const std::string_view line = trim(text.substr(0, eol));
        text = eol == std::string_view::npos ? std::string_view() : text.substr(eol + 1);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line == "[preset]") {
            flush();
            current.emplace();
            continue;
        }
        const auto eq = line.find('=');
        if (!current || eq == std::string_view::npos) {
            continue;
        }
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        if (key == "name") {
            current->name = std::string(value);
            continue;
        }
        if (key == "tip") {
            if (auto tip = parseTip(value)) {
                current->settings.tip.tip = std::make_shared<const BrushTip>(std::move(*tip));
            }
            continue;
        }
        const std::optional<double> v = parseNumber(value);
        if (!v) {
            continue;
        }
        if (key == "follow_direction") {
            current->settings.followDirection = *v != 0.0;
        } else if (key == "shape") {
            const int shape = static_cast<int>(*v);
            current->settings.shape =
                shape >= 0 && shape < kBrushShapeCount ? static_cast<BrushShape>(shape) : BrushShape::Round;
        } else if (key == "blend_mode") {
            const int mode = static_cast<int>(*v);
            current->settings.blendMode =
                mode >= 0 && mode < kBlendModeCount ? static_cast<BlendMode>(mode) : BlendMode::Normal;
        } else {
            for (const NumberField& f : kNumberFields) {
                if (key == f.key) {
                    current->settings.*f.member = std::clamp(*v, f.min, f.max);
                }
            }
        }
    }
    flush();
    return result;
}

} // namespace gas
