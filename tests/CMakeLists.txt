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
)
target_link_libraries(gas_core_tests PRIVATE gas::core gas_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(gas_core_tests)
