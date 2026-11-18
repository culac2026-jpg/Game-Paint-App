find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets OpenGLWidgets)
qt_standard_project_setup()

qt_add_executable(GameArtStudio WIN32
    main.cpp
    MainWindow.h
    MainWindow.cpp
    CanvasWidget.h
    CanvasWidget.cpp
    LayersPanel.h
    LayersPanel.cpp
    SlicesPanel.cpp
    TimelinePanel.cpp
    VectorTools.cpp
    BrushPanel.h
    BrushPanel.cpp
    EffectsPanel.h
    EffectsPanel.cpp
)

target_compile_definitions(GameArtStudio PRIVATE
    GAS_VERSION="${PROJECT_VERSION}"
    QT_NO_KEYWORDS
)

target_link_libraries(GameArtStudio PRIVATE
    gas::core
    gas_warnings
    Qt6::Widgets
    Qt6::OpenGLWidgets
)

# Make bin/ self-contained so the exe runs by double-click, independent of PATH:
# windeployqt copies Qt DLLs + plugins, then CopyRuntimeDlls.cmake copies every
# other non-system DLL (GCC runtime, ICU, zlib, ...) from the toolchain's bin dir.
if(WIN32 AND GAS_DEPLOY_RUNTIME)
    get_target_property(_gas_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_gas_qt_bin "${_gas_qmake}" DIRECTORY)
    get_filename_component(_gas_toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(GAS_WINDEPLOYQT NAMES windeployqt6 windeployqt
        HINTS "${_gas_qt_bin}" "${_gas_toolchain_bin}" REQUIRED)

    add_custom_command(TARGET GameArtStudio POST_BUILD
        COMMAND "${GAS_WINDEPLOYQT}" --no-translations --no-system-d3d-compiler
                --no-opengl-sw --no-compiler-runtime --verbose 0
                "$<TARGET_FILE:GameArtStudio>"
        COMMAND "${CMAKE_COMMAND}"
                "-DEXE=$<TARGET_FILE:GameArtStudio>"
                "-DSOURCE_DIR=${_gas_toolchain_bin}"
                -P "${PROJECT_SOURCE_DIR}/cmake/CopyRuntimeDlls.cmake"
        COMMENT "Deploying runtime DLLs next to GameArtStudio.exe"
        VERBATIM)
endif()
