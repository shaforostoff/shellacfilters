# ---------------------------------------------------------------------------
# fb2k_sdk.cmake
#
# Builds the parts of the foobar2000 SDK a DSP component needs, as static
# libraries, and exposes them through the interface target `fb2k::sdk`.
#
# helpers/, libPPUI/ and anything else that pulls in ATL or WTL are left out on
# purpose - see README.md.
#
# What else is built differs by platform:
#
#   Windows   pfc and the SDK proper, with the component client in the SDK
#             library. shared.dll ships with foobar2000 itself, so only its
#             import library is linked.
#
#   macOS     pfc and the SDK proper including their Objective-C++ halves, and
#             shared/ built from source - there is no shared.dylib to import
#             against, and the SDK's own Xcode projects compile it into the
#             component. The component client is compiled into each component
#             rather than into a library; see FB2K_COMPONENT_CLIENT_SOURCE.
# ---------------------------------------------------------------------------

if(NOT EXISTS "${FB2K_SDK_DIR}/foobar2000/SDK/foobar2000.h")
    message(FATAL_ERROR "FB2K_SDK_DIR does not look like a foobar2000 SDK: ${FB2K_SDK_DIR}")
endif()
if(APPLE AND NOT EXISTS "${FB2K_SDK_DIR}/foobar2000/helpers-mac/fb2k-platform.h")
    message(FATAL_ERROR
        "This foobar2000 SDK has no macOS support (foobar2000/helpers-mac is missing): ${FB2K_SDK_DIR}")
endif()

# --- pfc -------------------------------------------------------------------
if(WIN32)
    file(GLOB FB2K_PFC_SOURCES CONFIGURE_DEPENDS "${FB2K_SDK_DIR}/pfc/*.cpp")
    # POSIX-only translation unit; win-objects.cpp is its counterpart.
    list(FILTER FB2K_PFC_SOURCES EXCLUDE REGEX "synchro_nix\\.cpp$")
else()
    # Every .cpp and the one .mm. Nothing is excluded: win-objects.cpp is
    # #ifdef'd out from the inside and compiles to nothing off Windows, which
    # is how the SDK's own Xcode project builds it too.
    file(GLOB FB2K_PFC_SOURCES CONFIGURE_DEPENDS
         "${FB2K_SDK_DIR}/pfc/*.cpp" "${FB2K_SDK_DIR}/pfc/*.mm")
endif()

add_library(fb2k_pfc STATIC ${FB2K_PFC_SOURCES})
# Both roots, matching the stock .vcxproj files: the SDK sources use
# <pfc/...> and <libPPUI/...> from the top, and <SDK/...> from foobar2000/.
# SYSTEM (plus /external:W0 in the top level CMakeLists) so third party
# warnings do not bury our own.
target_include_directories(fb2k_pfc SYSTEM PUBLIC
    "${FB2K_SDK_DIR}"
    "${FB2K_SDK_DIR}/foobar2000")
if(WIN32)
    # WIN32_LEAN_AND_MEAN is deliberately NOT set: pfc/timers.h calls
    # timeGetTime, which only appears once windows.h has pulled in mmsystem.h.
    target_compile_definitions(fb2k_pfc PUBLIC
        UNICODE _UNICODE
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        _WIN32_WINNT=${FOO_DSP_WIN32_WINNT}
        WINVER=${FOO_DSP_WIN32_WINNT})
    target_link_libraries(fb2k_pfc PUBLIC winmm)
endif()

# --- SDK + component client ------------------------------------------------
file(GLOB FB2K_SDK_SOURCES CONFIGURE_DEPENDS "${FB2K_SDK_DIR}/foobar2000/SDK/*.cpp")
if(APPLE)
    file(GLOB _fb2k_sdk_mm CONFIGURE_DEPENDS "${FB2K_SDK_DIR}/foobar2000/SDK/*.mm")
    list(APPEND FB2K_SDK_SOURCES ${_fb2k_sdk_mm})
endif()

# component_client.cpp holds foobar2000_get_interface, the one symbol the host
# looks up in a component.
#
# On macOS each component compiles it itself rather than linking it out of this
# static library. In an archive it is a member that is only pulled in because
# the same file defines core_api functions the component happens to call; a
# component that stopped calling them would link cleanly to a bundle exporting
# nothing, and foobar2000 would refuse to load it without being able to say
# why. Windows keeps the arrangement it has always had.
set(FB2K_COMPONENT_CLIENT_SOURCE
    "${FB2K_SDK_DIR}/foobar2000/foobar2000_component_client/component_client.cpp")
if(NOT APPLE)
    list(APPEND FB2K_SDK_SOURCES "${FB2K_COMPONENT_CLIENT_SOURCE}")
endif()

add_library(fb2k_sdk_core STATIC ${FB2K_SDK_SOURCES})
target_link_libraries(fb2k_sdk_core PUBLIC fb2k_pfc)

set(_fb2k_targets fb2k_pfc fb2k_sdk_core)

# --- shared ----------------------------------------------------------------
if(WIN32)
    # shared.dll ships with foobar2000 itself; the SDK only carries import libs.
    if(FOO_DSP_TARGET_ARCH STREQUAL "x64")
        set(_fb2k_shared_lib "${FB2K_SDK_DIR}/foobar2000/shared/shared-x64.lib")
    elseif(FOO_DSP_TARGET_ARCH STREQUAL "ARM64EC")
        set(_fb2k_shared_lib "${FB2K_SDK_DIR}/foobar2000/shared/shared-ARM64EC.lib")
    else()
        set(_fb2k_shared_lib "${FB2K_SDK_DIR}/foobar2000/shared/shared-Win32.lib")
    endif()
    if(NOT EXISTS "${_fb2k_shared_lib}")
        message(FATAL_ERROR "Missing foobar2000 import library: ${_fb2k_shared_lib}")
    endif()
    target_link_libraries(fb2k_sdk_core PUBLIC "${_fb2k_shared_lib}")
else()
    # There is no shared library to import against on macOS: the handful of
    # entry points it holds are compiled into the component, as the SDK's own
    # shared.xcodeproj does. The exclusions are its Win32 half - file dialogs,
    # the system tray, the minidump writer - and are the difference between
    # what is in the directory and what that project builds.
    file(GLOB FB2K_SHARED_SOURCES CONFIGURE_DEPENDS
         "${FB2K_SDK_DIR}/foobar2000/shared/*.cpp" "${FB2K_SDK_DIR}/foobar2000/shared/*.mm")
    list(FILTER FB2K_SHARED_SOURCES EXCLUDE REGEX
        "/(Utility|crash_info|filedialogs|filedialogs_vista|font_description|minidump|modal_dialog|systray|text_drawing|utf8api)\\.cpp$")
    add_library(fb2k_shared STATIC ${FB2K_SHARED_SOURCES})
    target_link_libraries(fb2k_shared PUBLIC fb2k_pfc)
    target_link_libraries(fb2k_sdk_core PUBLIC fb2k_shared)
    list(APPEND _fb2k_targets fb2k_shared)
endif()

# --- umbrella --------------------------------------------------------------
add_library(fb2k_sdk INTERFACE)
target_link_libraries(fb2k_sdk INTERFACE fb2k_sdk_core)
if(APPLE)
    target_link_libraries(fb2k_sdk INTERFACE "-framework Cocoa")
endif()
add_library(fb2k::sdk ALIAS fb2k_sdk)

set_target_properties(${_fb2k_targets} PROPERTIES FOLDER "foobar2000 SDK")

# The SDK is third party code; do not let its warnings drown out ours.
# /wd4996 covers the SDK deliberately calling its own deprecated APIs.
foreach(_t ${_fb2k_targets})
    if(MSVC)
        target_compile_options(${_t} PRIVATE /W3 /wd4996)
    else()
        target_compile_options(${_t} PRIVATE -w)
    endif()
endforeach()
