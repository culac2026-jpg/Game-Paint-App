#include <gas/core/SampleProjects.h>

#include <gas/core/Brush.h>
#include <gas/core/Document.h>
#include <gas/core/Effects.h>
#include <gas/core/Pencil.h>
#include <gas/core/Selection.h>
#include <gas/core/Vector.h>

#include <cmath>
#include <numbers>

namespace gas {

namespace {

void stroke(Document& doc, StrokeTool& tool, const std::vector<PointF>& points, const std::string& label) {
    TiledImage* img = doc.beginEdit();
    if (!img || points.empty()) {
        if (img) {
            doc.endEdit(label);
        }
        return;
    }
    tool.begin(*img, points.front(), 1.0);
    for (std::size_t i = 1; i < points.size(); ++i) {
        // Pressure ramps so pressure dynamics are exercised as well.
        tool.moveTo(*img, points[i], 0.4 + 0.6 * static_cast<double>(i) / static_cast<double>(points.size()));
    }
    tool.end();
    doc.endEdit(label);
}

std::vector<PointF> wave(PointF from, PointF to, double amplitude, double periods, int samples) {
    std::vector<PointF> out;
    for (int i = 0; i <= samples; ++i) {
        const double t = static_cast<double>(i) / samples;
        out.push_back({from.x + (to.x - from.x) * t,
                       from.y + (to.y - from.y) * t + amplitude * std::sin(t * periods * 2 * std::numbers::pi)});
    }
    return out;
}

void setName(Document& doc, const std::string& name) {
    LayerProps p = doc.activeLayer().props;
    p.name = name;
    doc.setLayerProps(doc.activeLayerIndex(), p);
}

// 64x64 pixel-art character: 4 layers, 6 frames, tags, crisp outline effect.
SampleProject pixelHero() {
    auto doc = std::make_unique<Document>(64, 64);
    PencilTool pencil;
    setName(*doc, "Body");
    doc->addLayer();
    setName(*doc, "Head");
    doc->addLayer();
    setName(*doc, "Weapon");
    doc->addLayer();
    setName(*doc, "FX");
    for (int f = 0; f < 6; ++f) {
        if (f > 0) {
            doc->duplicateFrame(f - 1);
        }
        const double bob = (f % 2) ? 1 : 0;
        doc->setActiveLayer(0);
        pencil.setColor({60, 90, 200, 255});
        for (int y = 0; y < 14; ++y) {
            stroke(*doc, pencil, {{24, 30 + y + bob}, {39, 30 + y + bob}}, "Body");
        }
        doc->setActiveLayer(1);
        pencil.setColor({240, 200, 160, 255});
        for (int y = 0; y < 10; ++y) {
            stroke(*doc, pencil, {{27, 19 + y + bob}, {36, 19 + y + bob}}, "Head");
        }
        doc->setActiveLayer(2);
        pencil.setColor({200, 200, 210, 255});
        stroke(*doc, pencil, {{42, 40.0 - f * 2}, {54, 28.0 - f * 2}}, "Weapon");
        doc->setActiveLayer(3);
        pencil.setColor({255, 230, 60, 200});
        stroke(*doc, pencil, {{10.0 + f * 7, 56}, {12.0 + f * 7, 52}}, "Spark");
    }
    doc->setFrameDuration(5, 200);
    doc->setTags({{"idle", 0, 1, TagDirection::Forward}, {"run", 2, 5, TagDirection::PingPong}});
    LayerProps body = doc->layer(0).props;
    Effect outline = Effect::make(EffectType::Outline);
    outline.color = {20, 20, 30, 255};
    body.effects.push_back(outline);
    doc->setLayerProps(0, body);
    LayerProps fx = doc->layer(3).props;
    fx.blendMode = BlendMode::Screen;
    doc->setLayerProps(3, fx);
    doc->setActiveFrame(0);
    doc->markSaved();
    return {"pixel_hero_anim", "64x64 sprite, 4 layers, 6 frames, tags, outline effect", std::move(doc)};
}

// 1024x1024 painting: 10 brush layers with blend modes, opacity, a mask,
// effects and a selection-restricted stroke.
SampleProject layeredIllustration() {
    auto doc = std::make_unique<Document>(1024, 1024);
    BrushTool brush;
    static constexpr BlendMode kModes[] = {BlendMode::Normal,  BlendMode::Multiply, BlendMode::Screen,
                                           BlendMode::Overlay, BlendMode::SoftLight, BlendMode::Normal,
                                           BlendMode::Darken,  BlendMode::Lighten,  BlendMode::Color,
                                           BlendMode::Normal};
    for (int l = 0; l < 10; ++l) {
        if (l > 0) {
            doc->addLayer();
        }
        setName(*doc, "Paint " + std::to_string(l + 1));
        brush.settings().diameter = 8.0 + l * 6;
        brush.settings().hardness = 0.3 + 0.07 * l;
        brush.settings().pressureSize = 0.5;
        brush.setSeed(static_cast<std::uint64_t>(l) + 1);
        brush.setColor({static_cast<std::uint8_t>(40 + l * 20), static_cast<std::uint8_t>(200 - l * 15),
                        static_cast<std::uint8_t>(90 + l * 10), 255});
        for (int s = 0; s < 3; ++s) {
            const double y = 120 + l * 80 + s * 20;
            stroke(*doc, brush, wave({60, y}, {960, y + 40}, 30 + 5 * s, 1.5 + s, 120), "Brush");
        }
        LayerProps p = doc->layer(l).props;
        p.blendMode = kModes[l];
        p.opacity = 0.55 + 0.045 * l;
        doc->setLayerProps(l, p);
    }
    // Mask on layer 4: hide the left half.
    doc->setActiveLayer(4);
    doc->addLayerMask(4);
    doc->setEditingMask(true);
    if (TiledImage* mask = doc->beginEdit()) {
        for (int y = 0; y < 1024; y += 1) {
            for (int x = 0; x < 512; ++x) {
                mask->setPixel({x, y}, {0, 0, 0, 255});
            }
        }
        doc->endEdit("Mask");
    }
    doc->setEditingMask(false);
    // Effects.
    LayerProps p2 = doc->layer(2).props;
    Effect shadow = Effect::make(EffectType::DropShadow);
    shadow.offsetX = 6;
    shadow.offsetY = 8;
    p2.effects.push_back(shadow);
    doc->setLayerProps(2, p2);
    LayerProps p6 = doc->layer(6).props;
    p6.effects.push_back(Effect::make(EffectType::OuterGlow));
    Effect adjust = Effect::make(EffectType::ColorAdjust);
    adjust.hue = 30;
    adjust.saturation = 0.2;
    p6.effects.push_back(adjust);
    doc->setLayerProps(6, p6);
    // A stroke limited to an elliptical selection.
    doc->setActiveLayer(9);
    doc->setSelection(Selection::ellipse(1024, 1024, {300, 300, 400, 300}, true));
    brush.settings().diameter = 60;
    brush.setColor({250, 250, 255, 255});
    stroke(*doc, brush, wave({200, 450}, {850, 450}, 120, 3, 200), "Masked Stroke");
    doc->deselect();
    doc->setActiveLayer(0);
    doc->markSaved();
    return {"layered_illustration", "1024x1024, 10 layers, blend modes, mask, effects, selection stroke",
            std::move(doc)};
}

// 512x512 UI kit drawn with vector layers; slices with 9-slice borders.
SampleProject uiKit() {
    auto doc = std::make_unique<Document>(512, 512);
    setName(*doc, "Background");
    VectorStyle panelStyle;
    panelStyle.fillColor = {40, 60, 90, 255};
    panelStyle.stroke = true;
    panelStyle.strokeColor = {230, 200, 120, 255};
    panelStyle.strokeWidth = 3;
    VectorStyle buttonStyle = panelStyle;
    buttonStyle.fillColor = {70, 150, 90, 255};
    buttonStyle.join = LineJoin::Round;
    VectorStyle iconStyle;
    iconStyle.fillColor = {255, 210, 40, 255};
    iconStyle.antialias = false;
    doc->addVectorLayer({{rectanglePath({16, 16, 192, 128}, 16), panelStyle}}, "Panel");
    setName(*doc, "Panel");
    doc->addVectorLayer({{rectanglePath({16, 176, 160, 48}, 24), buttonStyle},
                         {rectanglePath({16, 240, 160, 48}, 8), buttonStyle}},
                        "Buttons");
    setName(*doc, "Buttons");
    doc->addVectorLayer({{polygonPath({296, 80}, 56, 5, 0.45, 0), iconStyle},
                         {ellipsePath({384, 32, 96, 96}), panelStyle}},
                        "Icons");
    setName(*doc, "Icons");
    VectorPath wavePath;
    wavePath.nodes = {{{240, 300}, {240, 300}, {280, 250}}, {{360, 300}, {320, 350}, {400, 250}},
                      {{480, 300}, {440, 350}, {480, 300}}};
    VectorStyle line;
    line.fill = false;
    line.stroke = true;
    line.strokeWidth = 6;
    line.cap = LineCap::Round;
    line.strokeColor = {200, 80, 160, 255};
    doc->addVectorLayer({{wavePath, line}}, "Swoosh");
    setName(*doc, "Swoosh");
    std::vector<Slice> slices = {
        {"panel", {12, 12, 200, 136}, {0.5, 0.5}, {24, 24, 24, 24}},
        {"button_pill", {12, 172, 168, 56}, {0.5, 0.5}, {28, 8, 28, 8}},
        {"button_square", {12, 236, 168, 56}, {0.5, 1.0}, {12, 12, 12, 12}},
        {"star_icon", {236, 20, 120, 120}, {0.5, 0.5}, {}},
        {"ring_icon", {380, 28, 104, 104}, {0.5, 0.5}, {}},
    };
    doc->setSlices(std::move(slices), "Slices");
    doc->markSaved();
    return {"ui_kit", "512x512 vector UI kit, 9-slice panel and buttons, icons", std::move(doc)};
}

// 4096x4096 canvas with a few strokes far apart (sparse tiles).
SampleProject bigSparse() {
    auto doc = std::make_unique<Document>(4096, 4096);
    BrushTool brush;
    brush.settings().diameter = 40;
    setName(*doc, "Sketch");
    brush.setColor({30, 30, 30, 255});
    stroke(*doc, brush, wave({100, 100}, {900, 300}, 40, 2, 150), "Brush");
    stroke(*doc, brush, wave({3000, 3500}, {4000, 3900}, 60, 1, 150), "Brush");
    doc->addLayer();
    setName(*doc, "Color");
    brush.setColor({220, 80, 40, 255});
    brush.settings().diameter = 120;
    stroke(*doc, brush, wave({2000, 2000}, {2400, 2100}, 30, 1, 80), "Brush");
    doc->addLayer();
    setName(*doc, "Empty");
    doc->markSaved();
    return {"big_sparse", "4096x4096, three layers, mostly empty tiles", std::move(doc)};
}

// 256x256 tileset: pencil tiles on a 32 px grid with grid slices.
SampleProject tileset() {
    auto doc = std::make_unique<Document>(256, 256);
    PencilTool pencil;
    setName(*doc, "Tiles");
    std::vector<Slice> slices;
    for (int ty = 0; ty < 4; ++ty) {
        for (int tx = 0; tx < 8; ++tx) {
            pencil.setColor({static_cast<std::uint8_t>(30 * tx), static_cast<std::uint8_t>(60 * ty), 120, 255});
            const double x0 = tx * 32 + 2, y0 = ty * 32 + 2;
            for (int k = 0; k < 28; k += 3) {
                stroke(*doc, pencil, {{x0, y0 + k}, {x0 + 27, y0 + 27 - k}}, "Pencil");
            }
            slices.push_back({"tile_" + std::to_string(ty * 8 + tx), {tx * 32, ty * 32, 32, 32}, {0.5, 1.0}, {}});
        }
    }
    doc->setSlices(std::move(slices), "Grid Slices");
    doc->markSaved();
    return {"tileset_grid", "256x256 pixel tiles with 32 grid slices", std::move(doc)};
}

} // namespace

std::vector<SampleProject> buildSampleProjects() {
    std::vector<SampleProject> out;
    out.push_back(pixelHero());
    out.push_back(layeredIllustration());
    out.push_back(uiKit());
    out.push_back(bigSparse());
    out.push_back(tileset());
    return out;
}

} // namespace gas
