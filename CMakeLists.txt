cmake_minimum_required(VERSION 3.20)

project(ClickShowCpp LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/res/resource.h"
    "${CMAKE_CURRENT_BINARY_DIR}/resource.h"
    COPYONLY)

set(CLICKSHOW_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/clickshow.ico")
set(CLICKSHOW_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/res/app.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
    @ONLY)

add_executable(ClickShow WIN32
    src/main.cpp
    src/util.cpp
    src/json.cpp
    src/settings.cpp
    src/counts.cpp
    src/input.cpp
    src/overlay.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/app.rc")

target_include_directories(ClickShow PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/res")

target_compile_definitions(ClickShow PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00)

target_compile_options(ClickShow PRIVATE /W4 /permissive- /utf-8 /EHsc)

# The manifest is embedded by app.rc; do not let the linker generate another one.
target_link_options(ClickShow PRIVATE /MANIFEST:NO)

target_link_libraries(ClickShow PRIVATE
    user32
    gdi32
    gdiplus
    shell32
    advapi32
    winhttp
    comctl32
    comdlg32
    ole32)

set_target_properties(ClickShow PROPERTIES OUTPUT_NAME "ClickShow")

option(CLICKSHOW_BUILD_TESTS "Build the configuration compatibility test" OFF)
if(CLICKSHOW_BUILD_TESTS)
    add_executable(ConfigTest
        tests/config_test.cpp
        src/util.cpp
        src/json.cpp
        src/settings.cpp
        src/counts.cpp)
    target_include_directories(ConfigTest PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/res")
    target_compile_definitions(ConfigTest PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00)
    target_compile_options(ConfigTest PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_libraries(ConfigTest PRIVATE
        user32
        gdi32
        shell32
        advapi32
        ole32)
endif()
