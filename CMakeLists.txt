cmake_minimum_required(VERSION 3.21)

project(GameArtStudio
    VERSION 0.1.0
    DESCRIPTION "Desktop editor for 2D game assets"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GAS_BUILD_APP   "Build the Qt desktop application" ON)
option(GAS_BUILD_TESTS "Build unit tests for the core library" ON)

# Shared warning flags, linked privately by every first-party target.
add_library(gas_warnings INTERFACE)
if(MSVC)
    target_compile_options(gas_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(gas_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

# Core must stay free of Qt and OpenGL (see CLAUDE.md).
add_subdirectory(src/core)

if(GAS_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(GAS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
