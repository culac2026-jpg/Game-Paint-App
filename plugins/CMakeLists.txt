# Plugins are shared libraries loaded at run time from bin/plugins.
enable_language(C)

function(gas_add_plugin name)
    add_library(${name} MODULE ${ARGN})
    # Only the C header is needed, not the core library.
    target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src/core/include")
    target_link_libraries(${name} PRIVATE gas_warnings)
    set_target_properties(${name} PROPERTIES
        PREFIX ""
        C_VISIBILITY_PRESET hidden
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/plugins")
    if(MINGW)
        target_link_options(${name} PRIVATE -static-libgcc) # self-contained DLL
    endif()
    if(NOT WIN32)
        target_link_libraries(${name} PRIVATE m)
    endif()
endfunction()

gas_add_plugin(gas_sample_plugin sample/sample_plugin.c)
