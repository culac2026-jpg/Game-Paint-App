# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

**GameArt Studio** is a Windows desktop editor for 2D game assets (raster painting + vector, pixel art, sprite animation, atlas export to Unity/Cocos Creator). The code is at the **v0.2** milestone: canvas, zoom/pan, Pencil, Brush, Eraser, Fill, Eyedropper. The product/technical plan is `docs/GameArt_Studio_Ke_Hoach_Phat_Trien.docx` (Vietnamese, a proposal rather than a commitment) and is the source of truth for scope.

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

- `src/core` → `gas::core` static library, namespace `gas`, headers under `include/gas/core/`. **Must not include Qt or OpenGL.** All tests target this library.
  - `TiledImage`: sparse 256×256 RGBA tiles (`Rgba8`, straight alpha). Writes allocate the tile and mark it dirty. `takeDirtyTiles()` drains the set.
  - `Viewport`: `screen = doc * zoom + offset`, `zoomAt` keeps the anchor point fixed. Use `toPixel()` (floor) for doc→pixel.
  - `StrokeTool`: the begin/moveTo/end interface for drag tools, taking doc-space `PointF`.
  - `PencilTool` / `drawLine`: Bresenham, overwrites pixels. Each `moveTo` connects to the previous pixel so fast strokes have no gaps.
  - `BrushTool` (Paint or Erase mode): stamps round dabs every `spacing × diameter` px, carrying leftover distance across events. Each stroke keeps a per-tile coverage mask (max, not sum) plus a copy of each tile as it was when the stroke first touched it. Pixels are recomputed from that copy, so one stroke never exceeds its opacity. The same tile snapshots are what v0.3 undo should reuse.
  - `floodFill`: scanline fill, **4-connected** on purpose, so it can't leak through the pencil's 8-connected diagonal lines. `pickColor` ignores fully transparent pixels.
  - `Blend`: `blendOver`, `eraseAlpha`, `colorDistance`.
  - `Document`: a single raster layer for now (layers/undo/save in v0.3).
- `src/app`: Qt shell. `CanvasWidget` (a `QOpenGLWidget` painted with `QPainter`) keeps a per-tile `QImage` cache refreshed from `takeDirtyTiles()` on each paint, drawn nearest-neighbour. LMB goes to the active `Tool` (shortcuts N pencil, B brush, E eraser, G fill, I eyedropper; `[`/`]` brush size; C colour). MMB or Space+LMB pans, the wheel zooms at the cursor. `MainWindow` shows per-tool options (size/opacity/hardness, tolerance), and Brush and Eraser keep separate settings. Qt code is built with `QT_NO_KEYWORDS`, so write `Q_SIGNALS`/`Q_EMIT`, not `signals`/`emit`.
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
