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
)

target_compile_definitions(browser_core PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

target_link_libraries(browser_core PUBLIC
    winhttp
    shlwapi
    lexbor_static
    windowscodecs
    ole32
)

if(MSVC)
    target_compile_options(browser_core PRIVATE
        /W4 /permissive- /EHsc /utf-8
        $<$<CONFIG:Release>:/O2;/Oi;/Ot;/Gy;/Gw>
    )
else()
    target_compile_options(browser_core PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()

add_executable(Browser src/main.cpp)
target_link_libraries(Browser PRIVATE browser_core)

if(MSVC)
    target_sources(Browser PRIVATE assets/browser.rc)
endif()

if(MSVC)
    target_compile_options(Browser PRIVATE /W4 /permissive- /EHsc /utf-8
        $<$<CONFIG:Release>:/O2;/Oi;/Ot;/Gy;/Gw>)
    target_link_options(Browser PRIVATE $<$<CONFIG:Release>:/OPT:REF;/OPT:ICF>)
endif()

set_target_properties(Browser PROPERTIES OUTPUT_NAME "Browser")

# Lightweight terminal-style GUI (Win32 window, same browser_core engine).
add_executable(BrowserGui WIN32
    src/gui_main.cpp
    src/gui_render.cpp
    src/gui_input.cpp
)
target_link_libraries(BrowserGui PRIVATE browser_core)
if(MSVC)
    target_sources(BrowserGui PRIVATE assets/browser.rc)
    target_compile_options(BrowserGui PRIVATE /W4 /permissive- /EHsc /utf-8
        $<$<CONFIG:Release>:/O2;/Oi;/Ot;/Gy;/Gw>)
    target_link_options(BrowserGui PRIVATE $<$<CONFIG:Release>:/OPT:REF;/OPT:ICF>)
endif()
set_target_properties(BrowserGui PROPERTIES OUTPUT_NAME "BrowserGui")

# ---------------------------------------------------------------------------
# Unit tests (pure functions: encoding, URL, charset, HTML parsing, cache)
# ---------------------------------------------------------------------------
option(BROWSER_BUILD_TESTS "Build browser unit tests" ON)

if(BROWSER_BUILD_TESTS)
    enable_testing()
    add_executable(browser_tests
        tests/test_main.cpp
    )
    target_link_libraries(browser_tests PRIVATE browser_core)
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
