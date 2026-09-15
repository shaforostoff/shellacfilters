# ---------------------------------------------------------------------------
# VirtualDJ plug-ins - Declick and Dehum
#
#   Windows:  cmake -S . -B build/x64 -A x64
#             cmake --build build/x64 --config Release
#
#   macOS:    cmake -S . -B build/mac -DCMAKE_BUILD_TYPE=Release
#             cmake --build build/mac
#
# The VirtualDJ SDK is fetched automatically the first time you configure; see
# cmake/vdj_download_sdk.cmake. scripts/build.ps1 and scripts/build.sh wrap
# the two lines above and package the result into ../dist/vdj.
#
#   Declick.dll   detect-and-interpolate declicker, no delay
#   Dehum.dll     hum and drone canceller, no delay, scouted acquisition
#
# Both are IVdjPluginBufferDsp8 rather than IVdjPluginDsp8, and that is the one
# design decision worth knowing before reading anything here. A buffer plug-in
# is asked for the song and may read any part of it, so Declick's 880 samples of
# lookahead cost an extra read instead of 20 ms of delay - and a delay on a deck
# is not a detail, it moves the deck off everything it is being mixed against.
# See README.md.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.16)

# Pick the MSVC runtime through CMAKE_MSVC_RUNTIME_LIBRARY rather than by
# rewriting CMAKE_CXX_FLAGS.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(shellacfilters_virtualdj
        VERSION 1.0.1
        DESCRIPTION "ShellacFilters DSP plug-ins for VirtualDJ"
        LANGUAGES CXX)

if(NOT WIN32 AND NOT APPLE)
    message(FATAL_ERROR
        "vdjPlugin8.h only defines its platform macros for Windows, macOS and "
        "Android, and VirtualDJ ships desktop builds for the first two. "
        "Nothing here will compile on ${CMAKE_SYSTEM_NAME}.")
endif()

# --- options ---------------------------------------------------------------
set(VDJ_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/virtualdj_sdk"
    CACHE PATH "Location of the unpacked VirtualDJ 8 plugin SDK")
option(VDJ_SDK_AUTO_DOWNLOAD "Fetch the VirtualDJ SDK if it is not present" ON)
option(VDJ_STATIC_CRT        "Link the C runtime statically (no VC redist needed)" ON)
option(VDJ_LTO               "Whole program optimisation in Release builds" ON)
# Diagnostic only, and off for anything shipped. See common/vdj_trace.h: it is
# how "the host never opened the file" is told apart from "the host opened it
# and wanted a different interface", which nothing outside the plug-in can
# distinguish.
option(VDJ_TRACE             "Log every load and lifecycle call to a file" OFF)
set(VDJ_ARCH "SSE2" CACHE STRING "x86/x64 instruction set baseline: SSE2, AVX or AVX2")
set_property(CACHE VDJ_ARCH PROPERTY STRINGS SSE2 AVX AVX2)

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    # One bundle for both. VirtualDJ keeps Intel plug-ins in Plugins64 and
    # Apple Silicon ones in PluginsArm, but a universal bundle is loadable
    # from either, so the install script can drop the same artefact in both
    # and there is only one thing to build and to test.
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "" FORCE)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "" FORCE)
endif()

# --- SDK -------------------------------------------------------------------
if(NOT EXISTS "${VDJ_SDK_DIR}/vdjDsp8.h")
    if(NOT VDJ_SDK_AUTO_DOWNLOAD)
        message(FATAL_ERROR
            "The VirtualDJ SDK was not found in ${VDJ_SDK_DIR}.\n"
            "Run scripts/get_sdk.ps1 (or scripts/get_sdk.sh), or configure with "
            "-DVDJ_SDK_AUTO_DOWNLOAD=ON.")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DVDJ_SDK_DEST=${VDJ_SDK_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vdj_download_sdk.cmake"
        RESULT_VARIABLE _sdk_result)
    if(NOT _sdk_result EQUAL 0)
        message(FATAL_ERROR "Could not obtain the VirtualDJ SDK (exit ${_sdk_result}).")
    endif()
endif()

# --- global build settings -------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# DllGetClassObject has to be visible, and on macOS vdjPlugin8.h asks for that
# with __attribute__((visibility("default"))) - which only means anything if the
# default is not already default. Left alone deliberately: do not add
# CXX_VISIBILITY_PRESET hidden here without checking the export survives it.

if(VDJ_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(WIN32)
    # vdjPlugin8.h includes windows.h, which defines min and max as macros and
    # so breaks every std::min in this project. It sets WIN32_LEAN_AND_MEAN
    # itself but not this.
    add_compile_definitions(NOMINMAX)
endif()

if(MSVC)
    add_compile_options($<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/O2>
                        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/Oi>
                        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/Ot>)
    # Exceptions stay on: Channel::configure() allocates, so bad_alloc is
    # reachable, and both wrappers catch it rather than letting it cross the
    # ABI back into VirtualDJ.
    add_compile_options(/EHsc /Zi /utf-8 /MP)
    if(MSVC_VERSION GREATER_EQUAL 1929)
        add_compile_options(/external:W0)
    endif()
    add_link_options($<$<NOT:$<CONFIG:Debug>>:/DEBUG>
                     $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
                     $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>)

    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        # x64 is SSE2 by definition; on x86 it has to be asked for, and it also
        # moves double maths off the x87 stack, which is what keeps this port
        # numerically comparable to the others.
        if(VDJ_ARCH STREQUAL "AVX2")
            add_compile_options(/arch:AVX2)
        elseif(VDJ_ARCH STREQUAL "AVX")
            add_compile_options(/arch:AVX)
        else()
            add_compile_options(/arch:SSE2)
        endif()
    else()
        if(VDJ_ARCH STREQUAL "AVX2")
            add_compile_options(/arch:AVX2)
        elseif(VDJ_ARCH STREQUAL "AVX")
            add_compile_options(/arch:AVX)
        endif()
    endif()
else()
    # No -ffast-math, ever. Both cores are tuned numerically and the verify
    # suites in ../foobar2000_dsp/tests compare them to the bit; reassociation
    # breaks that and nothing here is fast-math bound anyway.
    add_compile_options(-O2 -g -Wall -Wextra -Wno-unused-parameter -fno-fast-math)
endif()

if(VDJ_TRACE)
    add_compile_definitions(VDJ_TRACE=1)
    message(STATUS "Load tracing is ON - these are diagnostic builds, not release ones")
endif()

if(VDJ_LTO)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/vdj_plugin.cmake")

add_subdirectory(vdj_declick)
add_subdirectory(vdj_dehum)

option(VDJ_BUILD_TESTS "Build the wrapper verification harness" ON)
if(VDJ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
