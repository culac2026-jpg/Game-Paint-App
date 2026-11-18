# Copies every non-system DLL needed by EXE (and by the Qt plugins that
# windeployqt placed in its subfolders) from SOURCE_DIR into EXE's directory.
#
# Usage: cmake -DEXE=<path/to/app.exe> -DSOURCE_DIR=<toolchain bin> -P CopyRuntimeDlls.cmake

cmake_minimum_required(VERSION 3.21)

if(NOT EXE OR NOT SOURCE_DIR)
    message(FATAL_ERROR "EXE and SOURCE_DIR must be set")
endif()

get_filename_component(_bin_dir "${EXE}" DIRECTORY)
file(TO_CMAKE_PATH "${SOURCE_DIR}" _source_dir)
file(GLOB _plugin_dlls "${_bin_dir}/*/*.dll")

file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${EXE}"
    LIBRARIES ${_plugin_dlls}
    # bin/ first so the exe and the plugins in subfolders resolve the same file.
    DIRECTORIES "${_bin_dir}" "${_source_dir}"
    RESOLVED_DEPENDENCIES_VAR _resolved
    UNRESOLVED_DEPENDENCIES_VAR _unresolved
    PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
    POST_EXCLUDE_REGEXES "[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]"
)

set(_copied 0)
foreach(_dll IN LISTS _resolved)
    # Always copy the toolchain's file of that name, never the path that was
    # resolved: on rebuilds that is the stale copy already in bin/, and it can
    # also be a same-named DLL elsewhere on PATH (e.g. Git for Windows' older
    # mingw64 runtime). DLLs the toolchain doesn't have are left alone.
    get_filename_component(_name "${_dll}" NAME)
    if(EXISTS "${_source_dir}/${_name}")
        file(COPY_FILE "${_source_dir}/${_name}" "${_bin_dir}/${_name}" ONLY_IF_DIFFERENT)
        math(EXPR _copied "${_copied} + 1")
    endif()
endforeach()

message(STATUS "CopyRuntimeDlls: ${_copied} toolchain DLLs in ${_bin_dir}")
