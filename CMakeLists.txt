cmake_minimum_required(VERSION 3.24)
project(Browser LANGUAGES C CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# Lexbor HTML5 parser (static library).
set(LEXBOR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LEXBOR_INSTALL_HEADERS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    lexbor
    URL https://github.com/lexbor/lexbor/archive/refs/tags/v3.0.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(lexbor)

# WebView2 SDK (Chromium) — optional hosted web window for the console UI.
set(WEBVIEW2_DIR "${CMAKE_SOURCE_DIR}/deps/webview2")
set(WEBVIEW2_NUPKG "${CMAKE_BINARY_DIR}/webview2.nupkg")
if(NOT EXISTS "${WEBVIEW2_DIR}/build/native/include/WebView2.h")
    file(DOWNLOAD
        "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4258.31/microsoft.web.webview2.1.0.4258.31.nupkg"
        "${WEBVIEW2_NUPKG}" SHOW_PROGRESS)
    file(REMOVE_RECURSE "${WEBVIEW2_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${WEBVIEW2_NUPKG}"
         DESTINATION "${WEBVIEW2_DIR}")
endif()
set(WEBVIEW2_NATIVE "${WEBVIEW2_DIR}/build/native")

# Core library shared by the browser executable and the test suite.
add_library(browser_core STATIC
    src/util.cpp
    src/network.cpp
    src/html.cpp
    src/storage.cpp
    src/feeds.cpp
    src/downloads.cpp
    src/sync.cpp
    src/app.cpp
    src/console_ui.cpp
    src/wvhost.cpp
)

target_compile_definitions(browser_core PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
)

target_link_libraries(browser_core PUBLIC
    winhttp
    shlwapi
    lexbor_static
    windowscodecs
    ole32
)

target_include_directories(browser_core PRIVATE "${WEBVIEW2_NATIVE}/include")

if(MSVC)
    target_compile_options(browser_core PRIVATE
        /W4 /permissive- /EHsc /utf-8
        $<$<CONFIG:Release>:/O2;/Oi;/Ot;/Gy;/Gw>
    )
else()
    target_compile_options(browser_core PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()

add_executable(Browser src/main.cpp)
target_link_libraries(Browser PRIVATE browser_core
    "${WEBVIEW2_NATIVE}/x64/WebView2LoaderStatic.lib")

if(MSVC)
    target_sources(Browser PRIVATE assets/browser.rc)
endif()

if(MSVC)
    target_compile_options(Browser PRIVATE /W4 /permissive- /EHsc /utf-8
        $<$<CONFIG:Release>:/O2;/Oi;/Ot;/Gy;/Gw>)
    target_link_options(Browser PRIVATE $<$<CONFIG:Release>:/OPT:REF;/OPT:ICF>)
endif()

set_target_properties(Browser PROPERTIES OUTPUT_NAME "Browser")

# ---------------------------------------------------------------------------
# Unit tests (pure functions: encoding, URL, charset, HTML parsing, cache)
# ---------------------------------------------------------------------------
option(BROWSER_BUILD_TESTS "Build browser unit tests" ON)

if(BROWSER_BUILD_TESTS)
    enable_testing()
    add_executable(browser_tests
        tests/test_main.cpp
    )
    target_link_libraries(browser_tests PRIVATE browser_core
        "${WEBVIEW2_NATIVE}/x64/WebView2LoaderStatic.lib")
    target_compile_definitions(browser_tests PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    if(MSVC)
        target_compile_options(browser_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(browser_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME browser_tests COMMAND browser_tests)
endif()
