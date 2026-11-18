# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

**GameArt Studio** is a Windows desktop editor for 2D game assets (raster painting + vector, pixel art, sprite animation, atlas export to Unity/Cocos Creator). The code is at the **v0.6** milestone: canvas, zoom/pan, Pencil, Brush, Eraser, Fill, Eyedropper, layers, undo/redo, project Save/Open, PNG export, selections (rect/ellipse/lasso, add/subtract/intersect/invert), move/free transform/flip/rotate/crop, layer masks, 18 layer/brush blend modes, brush dynamics (pen pressure, jitter, scatter, flat tips, stabilizer) brush presets, and non-destructive layer effects (color adjust, blur, pixelate, outline, drop/inner shadow, outer glow, color overlay). The product/technical plan is `docs/GameArt_Studio_Ke_Hoach_Phat_Trien.docx` (Vietnamese, a proposal rather than a commitment) and is the source of truth for scope.

## Toolchain and commands

C++20, CMake ≥ 3.21 + Ninja, Qt 6.5+ (Widgets, OpenGLWidgets). On this machine the toolchain is **MSYS2 UCRT64** (GCC and Qt from `pacman`, in `C:\msys64\ucrt64`). There is no MSVC C++ toolset installed. Keep MSYS2 fully upgraded (`pacman -Syu`): MSYS2's Qt is built against the latest GCC runtime, and a partial upgrade makes Qt DLLs fail with `0xc0000139`.

Git for Windows puts its own `/mingw64/bin` (with older `libstdc++-6.dll`, `zlib1.dll`, etc.) ahead of `C:\msys64\ucrt64\bin` in the Bash tool's PATH. Prepend ucrt64 when building or running tools: `PATH="/c/msys64/ucrt64/bin:$PATH"`.

All executables go to `build/<preset>/bin`. After each app build (`GAS_DEPLOY_RUNTIME`, on by default on Windows), `windeployqt` copies the Qt DLLs and plugins there. Then `cmake/CopyRuntimeDlls.cmake` copies every other non-system DLL by name from the toolchain's bin directory, never from wherever PATH resolved it. So `bin/` is self-contained and the exe runs by double-click without MSYS2 on PATH.

```bash
cmake --preset debug                 # configure (build/debug); also: release, core-only (no Qt)
cmake --build --preset debug         # build app + tests
ctest --preset debug                 # run all tests
ctest --preset debug -R Viewport     # tests whose name matches a regex
./build/debug/bin/gas_core_tests --gtest_filter='PencilTool.*'   # run tests directly
./build/debug/bin/GameArtStudio.exe
```

GoogleTest 1.15.2 is fetched by `FetchContent` at configure time (or found via `find_package(GTest)` if installed).

## Code layout

- `src/core` → `gas::core` static library, namespace `gas`, headers under `include/gas/core/`. **Must not include Qt or OpenGL.** Its only dependency is zlib. All tests target this library.
  - `TiledImage`: sparse 256×256 RGBA tiles (`Rgba8`, straight alpha) held as **copy-on-write `shared_ptr`s**. Copying an image or holding `snapshot()`/`sharedTile()` is pointer sharing, and `editTile()` clones a shared tile before writing. Writes mark tiles dirty, and `takeDirtyTiles()` drains the set.
  - `Document`: layers bottom (index 0) to top, each with `LayerProps` (name, visible, locked, opacity) and a `TiledImage`. **Every change goes through the document** so it's undoable:
    - Layer methods (`addLayer`, `removeLayer`, `moveLayer`, `setLayerProps`, …) record steps.
    - Tools draw between `beginEdit()` (returns the active layer's image, or null if it's locked or hidden) and `endEdit(label)`. `endEdit` diffs tile pointers against the snapshot taken at `beginEdit`, so a step stores only the changed tiles.
    - History is capped by estimated memory (1 GiB by default), dropping the oldest steps first. Consecutive `setLayerProps` on the same layer merge into one step.
    - `isModified()` compares against `markSaved()`.
    - `revertEdit()` restores the edited image to its `beginEdit` state while keeping the edit open (StreamLine redraws the stroke this way, still one undo step).
    - **Selection** (`selection()`, `setSelection()` and friends are undo steps; `selectionGeneration()` bumps on every change). While a selection exists, `beginEdit()` hands tools a COW *scratch* copy; `syncEdit()` (call it after each tool step so the canvas shows progress) and `endEdit()` write `mix(original, scratch, coverage)` into the layer, so nothing outside the selection can change.
    - **Transform session**: `beginTransform()` lifts the selected pixels (or the layer's content bounds) into a `FloatingImage` and leaves a hole; `setTransform(Affine)` previews with Nearest; `commitTransform(interp)` makes one undo step (pixels + transformed selection); identity commit = cancel. Undo and other edits are refused while transforming (`busy()`). `flipContent`/`rotateContent90` are exact one-shot transforms.
    - `crop(IRect)` re-tiles every layer and mask (shares tiles when the offset is tile-aligned), deselects, changes the document size.
    - **Layer masks**: `Layer::mask` is an optional `TiledImage` painted like any layer; `maskValue()` maps it (unpainted = shown, black = hidden), multiplied into opacity at composite time when `props.maskEnabled`. `setEditingMask(true)` makes `beginEdit()` target the mask. Adding a mask while a selection exists reveals only the selection.
    - Rendering uses `compositeTile`/`compositePixel`/`flatten`, plus `takeDirtyTiles()`, which covers every tile after a structure change.
  - `Selection`: 8-bit coverage mask in COW 256×256 tiles (null = unselected, one shared constant tile = fully selected). Builders `all/rect/ellipse/polygon` (even-odd, analytic anti-aliasing), `combined(op)`, `inverted`, `transformed`, `cropped`, `feathered(radius)` (3-pass box blur of the coverage), `outline()` (merged boundary segments for marching ants; skips uniform tiles, so select-all on 16k² is cheap).
  - `Transform`: `FloatingImage` + `transformInto(dst, src, Affine, Interpolation)` (inverse-maps pixel centres; Nearest or premultiplied Bilinear). `Geometry.h` holds `IRect`, `RectF`, `Affine`, `mappedBounds`.
  - `Effects`: `Effect` (one struct, fields used per `EffectType`) lives in `LayerProps::effects`, so effect edits are ordinary undoable, mergeable prop edits. `renderEffects()` renders per destination tile from a region grown by `effectMargin()` (premultiplied float planes; 3-pass box blur; Felzenszwalb distance transform for outline/glow; pixelate cells aligned to the canvas origin). Order: filters (ColorAdjust/Blur/Pixelate) in stack order, then DropShadow < OuterGlow < Outline < content (+ColorOverlay) < InnerShadow.
  - Effect cache: `Document::effectImage()` keeps a per-layer `EffectCache` (in `Layer::effectCache`, shared by copies but self-validating). It compares `TiledImage::revision()` (a globally unique counter bumped on every write) and then COW tile pointers to find changed tiles, and re-renders only tiles within the margin. `takeDirtyTiles()` expands dirty tiles by the same margin. Layer pixels are never modified; `rasterizeLayerEffects()` bakes them on request.
  - `ProjectIO`: `.gas` save/load (chunked little-endian binary (v2 adds `LMSK` layer-mask chunks, v3 adds `LPRP` layer-property chunks holding the blend mode, v4 adds `LEFX` effect stacks; older versions still load), zlib-compressed non-empty tiles, CRC-32 trailer, format version, unknown chunks skipped; the layout is documented in `ProjectIO.h`). `exportPng` (a self-contained PNG encoder) and all saves go through `writeFileAtomic` (temp sibling + rename). Bump `kProjectFormatVersion` for incompatible changes.
  - `Viewport`: `screen = doc * zoom + offset`, `zoomAt` keeps the anchor point fixed. Use `toPixel()` (floor) for doc→pixel.
  - `StrokeTool`: the begin/moveTo/end interface for drag tools, taking doc-space `PointF`. It also carries a `Symmetry` (read at `begin`).
  - `Symmetry`: mirror painting modes (Vertical/Horizontal/Quad/Radial/Kaleidoscope, append-only enum) expanded by `copies()` into `SymmetryCopy` (flip across the vertical line through the centre, then rotate; multiples of 90° use exact cos/sin). `BrushTool::stampSymmetric` stamps every copy of each dab into the *same* stroke mask (copies never stack), with mirrored centre/angle and `Dab::flipped` flipping the tip's local y so asymmetric tips mirror; wet-mix colour and smudge buffers are kept per copy (smudge cells indexed with symmetric `lround`). The pencil mirrors pixel by pixel when all copies are grid-aligned (exact pixel-art mirrors) and draws one line per copy for other rotations.
  - `PencilTool` / `drawLine`: Bresenham, overwrites pixels. Each `moveTo` connects to the previous pixel so fast strokes have no gaps.
  - `BrushTool` (Paint or Erase mode): stamps dabs every `spacing × dab diameter` px, carrying leftover distance across events. Each stroke keeps a per-tile coverage mask (max of coverage × dab opacity, not sum) plus a COW copy of each tile as it was when the stroke first touched it. Pixels are recomputed from that copy (with `blendPixel` and the brush's blend mode), so one stroke never exceeds its opacity. `BrushSettings` also holds the dynamics: tip shape (`BrushShape`: Round/Square/Diamond/Triangle/Hexagon/Star as distance or polygon-gauge functions with a numeric-gradient 1 px AA edge, or Custom = a `BrushTip` coverage image made by `brushTipFromImage`, e.g. Edit > Define Brush Tip; tips compare by content and are base64 in preset files), pressure → size/opacity (pressure is interpolated along segments), size/opacity/angle jitter, scatter, roundness/angle/follow-direction (flat tips anti-alias via the ellipse's distance gradient), smoothing. Randomness is a SplitMix64 seeded per stroke (`setSeed`), never `<random>` distributions, so output is identical on every platform; every dab draws the same number of random values.
  - Media (`BrushSettings`): `grain` (canvas-anchored value-noise paper texture, so one stroke never fills its pits), `particles` (airbrush dots from a per-pixel hash of the dab index), `mix` (wet paint: picks up the pre-stroke colour under the dab, per tip diameter travelled, ignoring transparent pixels), `BrushShape::Bristle` (fixed streak profile across the tip). `BrushMode::Smudge`/`Blur` reuse the tip coverage but work progressively on the current pixels (premultiplied; smudge carries a buffer centred on the dab; `opacity` = strength). Built-in presets start with the paint-program media set (Brush, Calligraphy Brush/Pen, Airbrush, Oil Brush, Crayon, Marker, Natural Pencil, Watercolor); the panel's preset menu shows a rendered stroke per preset.
  - `StrokeSmoother`: the stabilizer, applied by the input layer (canvas) before calling the tool; `finish()` returns the release point so strokes end where the pointer did. `streamlinePath()` (StreamLine) smooths a whole recorded stroke after the fact: 1 px arc-length resampling, then two Gaussian passes whose window shrinks near the ends so both endpoints stay put.
  - `BrushPresets`: built-in presets plus a locale-independent `key = value` text format (unknown keys ignored, values clamped).
  - `floodFill`: scanline fill, **4-connected** on purpose, so it can't leak through the pencil's 8-connected diagonal lines. `pickColor` ignores fully transparent pixels.
  - `Blend`: `blendOver`, `eraseAlpha`, `mix`, `colorDistance`, `maskValue`, and `blendPixel(dst, src, BlendMode, coverage)` implementing the W3C Compositing spec formulas (separable + Hue/Saturation/Color/Luminosity). `BlendMode` values are stored in files: append only. Layers carry `props.blendMode`; golden values live in `BlendModeTest`.
- `src/app`: Qt shell.
  - `CanvasWidget` (a `QOpenGLWidget` painted with `QPainter`) keeps a per-tile cache of *composited* `QImage`s, refreshed from `Document::takeDirtyTiles()` on each paint and drawn nearest-neighbour; the cache is dropped when the document size changes (crop/undo). It wraps each stroke in `beginEdit`/`syncEdit`/`endEdit` and emits `documentEdited`. It owns the symmetry settings (not saved in the document; centre recentred when the canvas size changes, draggable handle snapping to half pixels, dashed guides), the selection-tool drags, the marching-ants outline (rebuilt when `selectionGeneration()` changes) and the free-transform box (state = offset/angle/scale about the source centre → `Document::setTransform`).
  - `LayersPanel`: dock showing the top layer first, with blend mode, opacity, lock and mask controls.
  - `EffectsPanel`: dock (tabbed with Brush) editing the active layer's effect stack; the list shows the last-applied effect on top and the form shows only the fields the selected type uses.
  - `BrushPanel`: dock with presets (user presets saved to `brush-presets.txt` in the app data folder via `QSaveFile`) and dynamics of the active Brush/Eraser; size/opacity/hardness stay on the options bar, and `settingsChanged` re-syncs it.
  - Pen pressure: `CanvasWidget::tabletEvent` only records `pressure()` and ignores the event, so Qt synthesizes mouse events that drive the tools; `pressureOf()` uses the recorded pressure for stylus-sourced mouse events and 1 for the mouse.
  - `MainWindow`: owns the `Document` and the file path. `onDocumentChanged()` re-syncs the title (`[*]` modified marker), Undo/Redo labels, the layers panel and the canvas. Connect anything that edits the document to it. The panel's signal is queued so the list isn't rebuilt inside its own item signal.
  - Convert paths with `toStdWString()` so Vietnamese file names work. LMB goes to the active `Tool` (shortcuts N pencil, B brush, E eraser, G fill, I eyedropper, M rect select, O ellipse select, L lasso, V move, S smudge, R blur; Q toggles StreamLine (options-bar strength; the canvas records raw samples, smooths live with the stabilizer, and on release reverts the edit and redraws along `streamlinePath`), U toggles straight-line mode (a drag previews a line and paints it on release; Ctrl at press does the same once; Shift snaps to 15° via `snapAngle`; Shift+click joins the last stroke's end to the click); `[`/`]` brush size; C colour). Shift/Alt/Shift+Alt at drag start = add/subtract/intersect. Ctrl+T free transform (Enter applies, Esc cancels), Delete clears, Alt+Backspace fills, Ctrl+A/Ctrl+D/Ctrl+Shift+I select all/deselect/inverse, Shift+F6 feather. `MainWindow::settleCanvas()` commits an open transform before save/export/close. The layer list has no keyboard focus so single-key shortcuts aren't swallowed by item search. MMB or Space+LMB pans, the wheel zooms at the cursor. `MainWindow` shows per-tool options (size/opacity/hardness, tolerance), and Brush and Eraser keep separate settings. Qt code is built with `QT_NO_KEYWORDS`, so write `Q_SIGNALS`/`Q_EMIT`, not `signals`/`emit`.
- `tests`: GoogleTest, one file per core class.

All first-party targets link the `gas_warnings` interface target (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow`).

## Planned stack beyond v0.1

- Stroke and pixel work on the CPU first; move to the GPU gradually
- Outputs: PNG, sprite sheet + JSON metadata; SVG and other formats come in later phases

## Planned module architecture

| Module | Responsibility |
|---|---|
| Application Shell | Qt panels, docking, shortcuts, command palette |
| Document Core | Canvas, layer tree, property graph, undo/redo, autosave |
| Raster Engine | Tiled RGBA buffers, brushes, selections, fills, image filters |
| Vector Engine | Paths, geometry, tessellation, vector layers |
| Renderer | Compositing, viewport zoom/pan, texture cache, OpenGL backend |
| Animation | Timeline, per-frame cels, onion skin, preview |
| Asset Pipeline | Slicing, atlas packing (MaxRects), 9-slice, engine export, validation |
| Plugin API | Tool/effect/export extension points; AI features stay optional plugins |

**Key constraint:** keep the core (Document Core, Raster/Vector engines) independent of Qt and OpenGL so it can be unit-tested and the rendering backend can be swapped later.

## Technical decisions to honor

- **Tile-based storage:** prototype 256×256 px tiles; mark only the tiles a stroke or filter touches as dirty.
- **Undo/redo:** a command log for light operations and tile snapshots for large strokes; cap history by RAM usage.
- **Color/alpha (decided in v0.2):** pixels are *stored* as straight-alpha RGBA8, matching PNG and the `QImage::Format_RGBA8888` upload. Blend math premultiplies internally (`Blend.cpp`), and any pixel with alpha 0 is normalised to all-zero. Use golden-image tests for color/alpha correctness.
- **Brush:** resample the pointer path so strokes never break; add pen pressure/tilt only after mouse input is stable.
- **Project files:** a versioned schema storing layers, transforms, masks, effects and metadata. Saves must be atomic, with autosave and backups.

## Build order

The MVP comes first: custom canvas, Pencil/Brush/Eraser, Eyedropper, Fill Bucket, RGBA palette, zoom/pan, raster layers, undo/redo, project save/open, PNG with transparency. AI, bone animation, PSD fidelity and vector booleans are explicitly **out of MVP scope**.

Roadmap: v0.1 canvas/zoom/pan/checkerboard/Pencil → v0.2 Brush/Eraser/Fill/Eyedropper → v0.3 layers/undo/save-open → v0.4 selection/transform/mask → v0.5 brush dynamics/blend modes → v0.6 non-destructive filters/effects → v0.7 slicing/atlas/batch export → v0.8 animation timeline → v0.9 vector/Pen/9-slice → v1.0 QA/performance/plugin API/crash recovery. Lock scope per version; each milestone has its own acceptance test in the plan doc.

## Acceptance criteria (from the plan)

- Fast strokes on a 2048×2048 canvas with no unexpected gaps
- 10 layers composite correctly (opacity, visibility, order)
- At least 20 consecutive undo/redo steps, and state survives Save/Open
- PNG export keeps exact size, colors and transparent pixels
- Selection/transform never touches pixels outside the selection
- Atlas export has correct metadata, padding and pivots per preset
- Test across Windows DPI/scaling levels; track stroke latency and peak RAM

## Reading the plan doc

The `.docx` can be dumped as text with Python (the console needs UTF-8 for Vietnamese):

```bash
PYTHONIOENCODING=utf-8 python -c "import zipfile,re;x=zipfile.ZipFile('docs/GameArt_Studio_Ke_Hoach_Phat_Trien.docx').read('word/document.xml').decode();[print(t) for p in re.findall(r'<w:p[ >].*?</w:p>',x,re.S) if (t:=''.join(re.findall(r'<w:t[^>]*>(.*?)</w:t>',p))).strip()]"
```
