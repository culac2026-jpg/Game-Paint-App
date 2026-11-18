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

        s = {};
        s.diameter = 12;
        s.hardness = 1.0;
        p.push_back(make("Hard Round", s));

        s = {};
        s.diameter = 40;
        s.hardness = 0.0;
        p.push_back(make("Soft Round", s));

        s = {};
        s.diameter = 80;
        s.hardness = 0.0;
        s.opacity = 0.35;
        s.pressureOpacity = 1.0;
        p.push_back(make("Airbrush", s));

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
        s.diameter = 20;
        s.hardness = 0.95;
        s.spacing = 0.04;
        s.roundness = 0.2;
        s.angle = 45;
        s.pressureSize = 0.6;
        s.smoothing = 0.3;
        p.push_back(make("Calligraphy", s));

        s = {};
        s.diameter = 6;
        s.hardness = 0.6;
        s.spacing = 0.6;
        s.scatter = 2.0;
        s.sizeJitter = 0.7;
        s.opacityJitter = 0.5;
        p.push_back(make("Spray", s));

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
        const std::optional<double> v = parseNumber(value);
        if (!v) {
            continue;
        }
        if (key == "follow_direction") {
            current->settings.followDirection = *v != 0.0;
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
