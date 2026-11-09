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

# src/version.h is the single source of truth for the version. Both the
# version resource and the manifest are generated from it so a version bump
# cannot leave the update check (kAppVersion) out of sync.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h" _clicshow_version_line
     REGEX "^#define CLICKSHOW_VERSION_WSTRING")
string(REGEX MATCH "L\"([0-9]+(\\.[0-9]+)*)\"" _clicshow_version_match
       "${_clicshow_version_line}")
if(NOT _clicshow_version_match)
    message(FATAL_ERROR "Cannot parse CLICKSHOW_VERSION_WSTRING from src/version.h")
endif()
set(CLICKSHOW_VERSION "${CMAKE_MATCH_1}")

# VERSIONINFO and assemblyIdentity both require four numeric components, while
# the display version may have any number of them (e.g. "1.51"). Pad or trim.
string(REPLACE "." ";" _clicshow_version_parts "${CLICKSHOW_VERSION}")
list(LENGTH _clicshow_version_parts _clicshow_version_count)
if(_clicshow_version_count GREATER 4)
    list(SUBLIST _clicshow_version_parts 0 4 _clicshow_version_parts)
endif()
while(TRUE)
    list(LENGTH _clicshow_version_parts _clicshow_version_count)
    if(_clicshow_version_count GREATER_EQUAL 4)
        break()
    endif()
    list(APPEND _clicshow_version_parts "0")
endwhile()
list(JOIN _clicshow_version_parts "." CLICKSHOW_VERSION_FULL)
list(JOIN _clicshow_version_parts "," CLICKSHOW_VERSION_COMMA)

set(CLICKSHOW_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/clickshow.ico")
set(CLICKSHOW_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/app.manifest")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest"
    "${CLICKSHOW_MANIFEST}"
    @ONLY)
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
    enable_testing()
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
    add_test(NAME ConfigTest COMMAND ConfigTest)
    set_tests_properties(ConfigTest PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
