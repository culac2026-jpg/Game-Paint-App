add_library(gas_core STATIC
    src/TiledImage.cpp
    src/Pencil.cpp
    src/Viewport.cpp
    src/Document.cpp
    src/Effects.cpp
    src/Symmetry.cpp
    src/StrokeSmoother.cpp
    src/Slicing.cpp
    src/Atlas.cpp
    src/Blend.cpp
    src/Brush.cpp
    src/BrushPresets.cpp
    src/Fill.cpp
    src/ProjectIO.cpp
    src/Selection.cpp
    src/Transform.cpp
)
add_library(gas::core ALIAS gas_core)

find_package(ZLIB REQUIRED)

target_include_directories(gas_core PUBLIC include)
target_link_libraries(gas_core PRIVATE gas_warnings ZLIB::ZLIB)
