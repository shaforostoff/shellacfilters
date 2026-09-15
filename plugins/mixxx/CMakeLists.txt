# ---------------------------------------------------------------------------
# Mixxx restoration - Declick and Dehum inside Mixxx
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ctest --test-dir build
#
# THIS PROJECT DOES NOT BUILD MIXXX, and it does not build a plug-in either.
# What lives here is a patch to Mixxx's own source tree - see README.md - and
# what this CMakeLists builds is the verification harness for it: the pipeline
# and the cores, compiled standalone with a stub for the one Mixxx class they
# touch, so that the part of the port that is not shared with any other can be
# tested without a 148 MB checkout and a two-hour build.
#
# scripts/integrate.sh is what puts the sources into a Mixxx tree. scripts/
# build.sh wraps the three lines above.
#
# Everything under src/ is laid out exactly as it lands in Mixxx, so the
# #include "restoration/..." lines here are the same ones that compile there.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.16)

project(mixxx_restoration
        VERSION 1.0.0
        DESCRIPTION "ShellacFilters restoration for Mixxx"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    add_compile_options(/EHsc /Zi /utf-8 /MP)
    add_compile_options($<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/O2>)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        add_compile_options(/arch:SSE2)
    endif()
else()
    # No -ffast-math, ever. Both cores are tuned numerically and the verify
    # suites in ../foobar2000_dsp/tests compare them to the bit; reassociation
    # breaks that and nothing here is fast-math bound anyway.
    add_compile_options(-O2 -g -Wall -Wextra -Wno-unused-parameter -fno-fast-math)
endif()

# The cores are MIRRORS, not sources: copied here from ../foobar2000_dsp/foo_dsp_*
# by that tree's scripts/sync_cores.sh. An edit made here is an edit that will be
# overwritten - change the canonical copy.
set(RESTORATION_SOURCES
    src/restoration/declick_core.cpp
    src/restoration/declick_core.h
    src/restoration/dehum_core.cpp
    src/restoration/dehum_core.h
    src/restoration/dehumscout.h
    src/restoration/restorationpipeline.h
    src/restoration/restorationsettings.h
    src/restoration/restorationsource.h)

add_library(restoration STATIC ${RESTORATION_SOURCES})
# src/ is the include root because that is what it is inside Mixxx, where these
# files are found as "restoration/declick_core.h" like every other Mixxx header.
target_include_directories(restoration PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

enable_testing()

function(add_restoration_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE restoration)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    set_target_properties(${name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY                "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/tests")
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_restoration_test(declick_mixxx_verify)
add_restoration_test(dehum_mixxx_verify)
