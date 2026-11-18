# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

**GameArt Studio** is a Windows desktop editor for 2D game assets (raster painting + vector, pixel art, sprite animation, atlas export to Unity/Cocos Creator). The code is at the **v0.1** milestone: canvas, zoom/pan, checkerboard, Pencil. The product/technical plan is `docs/GameArt_Studio_Ke_Hoach_Phat_Trien.docx` (Vietnamese, a proposal rather than a commitment) and is the source of truth for scope.

## Toolchain and commands

C++20, CMake ≥ 3.21 + Ninja, Qt 6.5+ (Widgets, OpenGLWidgets). On this machine the toolchain is **MSYS2 UCRT64** (GCC and Qt from `pacman`, in `C:\msys64\ucrt64`). There is no MSVC C++ toolset installed. Keep MSYS2 fully upgraded (`pacman -Syu`): MSYS2's Qt is built against the latest GCC runtime, and a partial upgrade makes Qt DLLs fail with `0xc0000139`.

Git for Windows puts its own `/mingw64/bin` (with older `libstdc++-6.dll`, `zlib1.dll`, etc.) ahead of `C:\msys64\ucrt64\bin` in the Bash tool's PATH. Prepend ucrt64 when running Qt binaries: `PATH="/c/msys64/ucrt64/bin:$PATH"`.

```bash
cmake --preset debug                 # configure (build/debug); also: release, core-only (no Qt)
cmake --build --preset debug         # build app + tests
ctest --preset debug                 # run all tests
ctest --preset debug -R Viewport     # tests whose name matches a regex
./build/debug/tests/gas_core_tests --gtest_filter='PencilTool.*'   # run tests directly
./build/debug/src/app/GameArtStudio.exe
```

GoogleTest 1.15.2 is fetched by `FetchContent` at configure time (or found via `find_package(GTest)` if installed).

## Code layout

- `src/core` → `gas::core` static library, namespace `gas`, headers under `include/gas/core/`. **Must not include Qt or OpenGL.** All tests target this library.
  - `TiledImage`: sparse 256×256 RGBA tiles (`Rgba8`, straight alpha). Writes allocate the tile and mark it dirty. `takeDirtyTiles()` drains the set.
  - `Viewport`: `screen = doc * zoom + offset`, `zoomAt` keeps the anchor point fixed. Use `toPixel()` (floor) for doc→pixel.
  - `PencilTool` / `drawLine`: Bresenham. Each `moveTo` connects to the previous point so fast strokes have no gaps.
  - `Document`: a single raster layer for now (layers/undo/save in v0.3).
- `src/app`: Qt shell. `CanvasWidget` (a `QOpenGLWidget` painted with `QPainter`) keeps a per-tile `QImage` cache refreshed from `takeDirtyTiles()` on each paint, drawn nearest-neighbour. Input: LMB = pencil, MMB or Space+LMB = pan, wheel = zoom at cursor. Qt code is built with `QT_NO_KEYWORDS`, so write `Q_SIGNALS`/`Q_EMIT`, not `signals`/`emit`.
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
- **Color/alpha:** RGBA internally with explicit alpha handling; settle the straight vs. premultiplied alpha policy for both renderer and export. Use golden-image tests for color/alpha correctness.
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
