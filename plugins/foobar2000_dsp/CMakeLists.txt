# ---------------------------------------------------------------------------
# foobar2000 DSP components - Airwindows ports
#
#   cmake -S . -B build/x86 -A Win32
#   cmake --build build/x86 --config Release
#
# The foobar2000 SDK is fetched automatically the first time you configure.
# See README.md.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.16)

# Pick the MSVC runtime through CMAKE_MSVC_RUNTIME_LIBRARY rather than by
# rewriting CMAKE_CXX_FLAGS.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(airwindows_foobar2000_dsp
        VERSION 1.0.0
        DESCRIPTION "Airwindows DSP components for foobar2000"
        LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "These components target the Windows build of foobar2000.")
endif()

# --- options ---------------------------------------------------------------
set(FB2K_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/foobar2000_sdk"
    CACHE PATH "Location of the unpacked foobar2000 SDK")
option(FB2K_SDK_AUTO_DOWNLOAD "Fetch the foobar2000 SDK if it is not present" ON)
option(FOO_DSP_STATIC_CRT     "Link the C runtime statically (no VC redist needed)" ON)
option(FOO_DSP_LTO            "Whole program optimisation in Release builds" ON)
set(FOO_DSP_ARCH "SSE2" CACHE STRING "Instruction set baseline: SSE2, AVX or AVX2")
set_property(CACHE FOO_DSP_ARCH PROPERTY STRINGS SSE2 AVX AVX2)
set(FOO_DSP_WIN32_WINNT "0x0601" CACHE STRING "Minimum Windows version (0x0601 = Windows 7)")

# --- target architecture ---------------------------------------------------
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]64")
        set(FOO_DSP_TARGET_ARCH "ARM64EC")
    else()
        set(FOO_DSP_TARGET_ARCH "x64")
    endif()
else()
    set(FOO_DSP_TARGET_ARCH "x86")
endif()
message(STATUS "Target architecture: ${FOO_DSP_TARGET_ARCH}")

# --- SDK -------------------------------------------------------------------
if(NOT EXISTS "${FB2K_SDK_DIR}/foobar2000/SDK/foobar2000.h")
    if(NOT FB2K_SDK_AUTO_DOWNLOAD)
        message(FATAL_ERROR
            "The foobar2000 SDK was not found in ${FB2K_SDK_DIR}.\n"
            "Run scripts/get_sdk.ps1, or configure with -DFB2K_SDK_AUTO_DOWNLOAD=ON.")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFB2K_SDK_DEST=${FB2K_SDK_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fb2k_download_sdk.cmake"
        RESULT_VARIABLE _sdk_result)
    if(NOT _sdk_result EQUAL 0)
        message(FATAL_ERROR "Could not obtain the foobar2000 SDK (exit ${_sdk_result}).")
    endif()
endif()

# --- global build settings -------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

if(FOO_DSP_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    # /O2 favours speed; /Oi lets the compiler use intrinsics for fabs, sin and
    # friends; /Ot is speed over size. Exceptions stay on - the SDK throws.
    add_compile_options($<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/O2>
                        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/Oi>
                        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:/Ot>)
    add_compile_options(/EHsc /Zi /utf-8 /MP)

    # /Gw gives every global and static its own COMDAT, and /Zc:inline drops
    # unreferenced inline functions along with the data only they touched, so
    # the /OPT:REF below can throw away what nothing reaches. Neither of them
    # changes the code that is kept.
    #
    # /GR- because nothing here needs RTTI: there is not one dynamic_cast or
    # typeid in these components or anywhere in the SDK, and exception handling
    # does not use it - catch matching carries its own type descriptors.
    #
    # Together these take about 7.6% off every component in x64 Release, and
    # nothing off the DSP path: what they remove is unreachable, not slower.
    add_compile_options(/Gw /Zc:inline /GR-)
    # Keep foobar2000 SDK header warnings out of our build log (VS 2019 16.10+).
    if(MSVC_VERSION GREATER_EQUAL 1929)
        add_compile_options(/external:W0)
    endif()
    # Produce a PDB for Release too: foobar2000's crash reporter is only useful
    # with symbols, and /DEBUG does not slow the generated code down.
    add_link_options($<$<NOT:$<CONFIG:Debug>>:/DEBUG>
                     $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
                     $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>)

    if(FOO_DSP_TARGET_ARCH STREQUAL "x86")
        # x64 is SSE2 by definition; on x86 it has to be asked for, and it also
        # moves double maths off the x87 stack, which matters for consistency.
        if(FOO_DSP_ARCH STREQUAL "AVX2")
            add_compile_options(/arch:AVX2)
        elseif(FOO_DSP_ARCH STREQUAL "AVX")
            add_compile_options(/arch:AVX)
        else()
            add_compile_options(/arch:SSE2)
        endif()
    elseif(FOO_DSP_TARGET_ARCH STREQUAL "x64")
        if(FOO_DSP_ARCH STREQUAL "AVX2")
            add_compile_options(/arch:AVX2)
        elseif(FOO_DSP_ARCH STREQUAL "AVX")
            add_compile_options(/arch:AVX)
        endif()
    endif()
endif()

if(FOO_DSP_LTO)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/fb2k_sdk.cmake")

add_subdirectory(foo_dsp_decrackle)
add_subdirectory(foo_dsp_declick)
add_subdirectory(foo_dsp_dehum)
add_subdirectory(foo_dsp_paraeq)

option(FOO_DSP_BUILD_TESTS "Build the DSP verification harness" ON)
if(FOO_DSP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(FOO_DSP_BUILD_TOOLS "Build the offline development utilities" ON)
if(FOO_DSP_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
