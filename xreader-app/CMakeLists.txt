cmake_minimum_required(VERSION 3.16)
project(xreader VERSION 0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick Network Gui)
qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(xreader
    main.cpp
    stylus.h stylus.cpp
    touchguard.h touchguard.cpp
    powerkey.h powerkey.cpp
    inkitem.h inkitem.cpp
    pagestore.h pagestore.cpp
    xclient.h xclient.cpp
    renderer.h renderer.cpp
    telegram.h telegram.cpp)

set_target_properties(xreader PROPERTIES OUTPUT_NAME "xr")

qt_add_qml_module(xreader
    URI xreader
    VERSION 1.0
    QML_FILES Main.qml Menu.qml)

target_link_libraries(xreader PRIVATE Qt6::Quick Qt6::Network Qt6::Gui Qt6::GuiPrivate)
# -rdynamic：让 dladdr/backtrace 在崩溃日志里解析主程序符号
target_link_options(xreader PRIVATE -rdynamic)

# AddressSanitizer 构建（默认关：ASan 运行时开销大，调试崩溃时开
# -DREMARKX_ASAN=ON；日常靠 main.cpp 崩溃处理器 + crash.log 定位）
option(REMARKX_ASAN "Build with AddressSanitizer" OFF)
if(REMARKX_ASAN)
    target_compile_options(xreader PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(xreader PRIVATE -fsanitize=address)
endif()
