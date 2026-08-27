cmake_minimum_required(VERSION 3.16)
project(xreader VERSION 0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick Network Gui)
qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(xreader
    main.cpp
    stylus.h stylus.cpp
    powerkey.h powerkey.cpp
    inkitem.h inkitem.cpp
    pagestore.h pagestore.cpp)

set_target_properties(xreader PROPERTIES OUTPUT_NAME "xr")

qt_add_qml_module(xreader
    URI xreader
    VERSION 1.0
    QML_FILES Main.qml Menu.qml)

target_link_libraries(xreader PRIVATE Qt6::Quick Qt6::Network Qt6::GuiPrivate)
