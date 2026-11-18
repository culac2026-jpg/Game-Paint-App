add_library(gas_core STATIC
    src/TiledImage.cpp
    src/Pencil.cpp
    src/Viewport.cpp
    src/Document.cpp
)
add_library(gas::core ALIAS gas_core)

target_include_directories(gas_core PUBLIC include)
target_link_libraries(gas_core PRIVATE gas_warnings)
