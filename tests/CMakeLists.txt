include(FetchContent)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    FIND_PACKAGE_ARGS NAMES GTest
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Core tests must not link Qt.
add_executable(gas_core_tests
    TiledImageTest.cpp
    PencilTest.cpp
    ViewportTest.cpp
    BlendTest.cpp
    BrushTest.cpp
    FillTest.cpp
    DocumentTest.cpp
    ProjectIOTest.cpp
    SelectionTest.cpp
    SelectionEditTest.cpp
    BlendModeTest.cpp
    BrushDynamicsTest.cpp
    BrushMediaTest.cpp
    SymmetryTest.cpp
    StraightLineTest.cpp
    StreamLineTest.cpp
    SlicingTest.cpp
    AtlasTest.cpp
    AnimationTest.cpp
    VectorTest.cpp
    EffectsTest.cpp
    RecoveryTest.cpp
    PluginTest.cpp
    QATest.cpp
)
find_package(ZLIB REQUIRED) # the PNG test decodes with zlib directly
target_link_libraries(gas_core_tests PRIVATE gas::core gas_warnings GTest::gtest_main ZLIB::ZLIB)

# The plugin test loads the real sample plugin DLL.
if(TARGET gas_sample_plugin)
    add_dependencies(gas_core_tests gas_sample_plugin)
    target_compile_definitions(gas_core_tests PRIVATE GAS_SAMPLE_PLUGIN="$<TARGET_FILE:gas_sample_plugin>")
else()
    target_compile_definitions(gas_core_tests PRIVATE GAS_SAMPLE_PLUGIN="")
endif()

include(GoogleTest)
gtest_discover_tests(gas_core_tests)
