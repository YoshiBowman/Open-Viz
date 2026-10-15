cmake_minimum_required(VERSION 3.20)
project(vizcore LANGUAGES C CXX)

# Standalone core of the lighting visualizer: GDTF/MVR parsing and DMX evaluation.
# It must build without Unreal, and the same sources are compiled into the Unreal plugin,
# so it follows Unreal's defaults: no exceptions, no RTTI.

# Built for speed unless told otherwise: Openviz runs the inspector every time a show's patch is
# listed, and without optimisation a rig of five hundred fixtures takes a second and a half.
# (A build folder made before this was a plain, unoptimised one: configure it again.)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Debug, Release, RelWithDebInfo or MinSizeRel" FORCE)
endif()

# C++17 is the floor; Unreal 5.8 compiles the same sources as C++20.
if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VIZCORE_BUILD_TESTS "Build the unit tests" ON)
option(VIZCORE_BUILD_TOOLS "Build the command-line inspector" ON)

# Third-party sources, compiled as part of the library. The definitions keep both libraries
# from throwing and keep miniz from exporting zlib's symbol names, which Unreal already has.
set(VIZCORE_THIRD_PARTY_DEFINITIONS
    PUGIXML_NO_EXCEPTIONS
    PUGIXML_NO_XPATH
    MINIZ_NO_STDIO
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

add_library(vizcore STATIC
    src/archive.cpp
    src/dmx_recording.cpp
    src/evaluate.cpp
    src/fixture_state.cpp
    src/gdtf.cpp
    src/image.cpp
    src/json.cpp
    src/mesh.cpp
    src/movie.cpp
    src/mvr.cpp
    src/mvr_export.cpp
    src/pieces.cpp
    src/rig.cpp
    src/screens.cpp
    src/show_edits.cpp
    src/text.cpp
    src/types.cpp
    third_party/pugixml/pugixml.cpp
    third_party/miniz/miniz.c)
target_include_directories(vizcore PUBLIC include PRIVATE src third_party/pugixml third_party/miniz)
target_compile_definitions(vizcore PRIVATE ${VIZCORE_THIRD_PARTY_DEFINITIONS})

if(MSVC)
    # Visual Studio calls the standard C file functions unsafe and, with warnings as errors, would stop there.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    set(VIZCORE_WARNINGS /W4 /WX)
    set(VIZCORE_NO_EXCEPTIONS /EHs-c- /GR- /D_HAS_EXCEPTIONS=0)
else()
    set(VIZCORE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
    set(VIZCORE_NO_EXCEPTIONS -fno-exceptions -fno-rtti)
endif()
target_compile_options(vizcore PRIVATE $<$<COMPILE_LANGUAGE:CXX>:${VIZCORE_NO_EXCEPTIONS}>)
# Warnings as errors for our own sources only.
set_source_files_properties(
    src/archive.cpp src/dmx_recording.cpp src/evaluate.cpp src/fixture_state.cpp src/gdtf.cpp src/image.cpp src/json.cpp src/mesh.cpp src/movie.cpp src/mvr.cpp src/mvr_export.cpp src/pieces.cpp src/rig.cpp src/screens.cpp src/show_edits.cpp src/text.cpp src/types.cpp
    PROPERTIES COMPILE_OPTIONS "${VIZCORE_WARNINGS}")

if(VIZCORE_BUILD_TOOLS)
    add_executable(vizcore_inspect tools/inspect.cpp)
    target_link_libraries(vizcore_inspect PRIVATE vizcore)
    target_compile_options(vizcore_inspect PRIVATE ${VIZCORE_WARNINGS} ${VIZCORE_NO_EXCEPTIONS})
    if(WIN32)
        # Paths on the command line as UTF-8, as everywhere else (see the manifest).
        target_sources(vizcore_inspect PRIVATE tools/utf8.manifest)
    endif()
endif()

# The tests are part of the development copy only; the published one leaves them out.
if(VIZCORE_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/main.cpp")
    enable_testing()
    add_executable(vizcore_tests
        tests/main.cpp
        tests/test_archive.cpp
        tests/test_dmx_value.cpp
        tests/test_evaluate.cpp
        tests/test_files.cpp
        tests/test_fixture_state.cpp
        tests/test_gdtf.cpp
        tests/test_image.cpp
        tests/test_mesh.cpp
        tests/test_movie.cpp
        tests/test_mvr.cpp
        tests/test_mvr_export.cpp
        tests/test_pieces.cpp
        tests/test_reference.cpp
        tests/test_rig.cpp
        tests/test_screens.cpp
        tests/test_show_edits.cpp)
    target_link_libraries(vizcore_tests PRIVATE vizcore)
    # The tests build .gdtf and .mvr archives in memory with miniz's writer.
    target_include_directories(vizcore_tests PRIVATE src third_party/miniz)
    target_compile_definitions(vizcore_tests PRIVATE
        ${VIZCORE_THIRD_PARTY_DEFINITIONS}
        VIZCORE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
        VIZCORE_REFERENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../reference")
    target_compile_options(vizcore_tests PRIVATE ${VIZCORE_WARNINGS} ${VIZCORE_NO_EXCEPTIONS})
    if(WIN32)
        target_sources(vizcore_tests PRIVATE tools/utf8.manifest)
    endif()
    add_test(NAME vizcore_tests COMMAND vizcore_tests)
endif()
