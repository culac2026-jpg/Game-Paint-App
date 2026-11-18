find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets OpenGLWidgets)
qt_standard_project_setup()

qt_add_executable(GameArtStudio WIN32
    main.cpp
    MainWindow.h
    MainWindow.cpp
    CanvasWidget.h
    CanvasWidget.cpp
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
