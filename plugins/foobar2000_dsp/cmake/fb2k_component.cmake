# ---------------------------------------------------------------------------
# fb2k_component.cmake
#
# fb2k_component(<name> <display name> SOURCES ... WIN32_SOURCES ...)
#
# Declares one foobar2000 component, in the shape each platform loads:
#
#   Windows   <name>.dll. component.cpp asserts that filename with
#             VALIDATE_COMPONENT_FILENAME, so it is the same on every
#             architecture; which one it is shows in where the packaging
#             puts it (the archive root, or x64/).
#
#   macOS     <name>.component, a loadable bundle:
#             <name>.component/Contents/MacOS/<name> and an Info.plist beside
#             it. VALIDATE_COMPONENT_FILENAME compiles to nothing there, but
#             the name still matters: both platforms' payloads share one
#             .fb2k-component archive and are told apart by where they sit in
#             it, and mac/<name>.component is where foobar2000 for Mac looks.
#
# SOURCES are compiled everywhere. WIN32_SOURCES - the dialogs, the resource
# script, anything that draws with GDI - only on Windows.
#
# scripts\build_release.ps1 and scripts/build_release_macos.sh assemble the
# results into archives.
# ---------------------------------------------------------------------------

function(fb2k_component name display_name)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "SOURCES;WIN32_SOURCES")

    set(_sources ${arg_SOURCES})
    if(WIN32)
        list(APPEND _sources ${arg_WIN32_SOURCES})
    else()
        # foobar2000_get_interface; see FB2K_COMPONENT_CLIENT_SOURCE in
        # fb2k_sdk.cmake for why it is compiled here rather than linked.
        list(APPEND _sources "${FB2K_COMPONENT_CLIENT_SOURCE}")
    endif()

    # SHARED on Windows, as it has always been; MODULE on macOS, because a
    # bundle is something one dlopens rather than links against.
    if(WIN32)
        add_library(${name} SHARED ${_sources})
    else()
        add_library(${name} MODULE ${_sources})
    endif()
    target_link_libraries(${name} PRIVATE fb2k::sdk)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

    if(WIN32)
        target_link_libraries(${name} PRIVATE comctl32)
        set_target_properties(${name} PROPERTIES
            OUTPUT_NAME "${name}"
            PREFIX ""
            SUFFIX ".dll")
    else()
        # The version is read out of version.h, which is its one home on both
        # platforms - the .rc file and the about box read it from there too.
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/version.h" _version_line
             REGEX "_VERSION_STRING[ \t]+\"")
        if(NOT _version_line MATCHES "\"([^\"]+)\"")
            message(FATAL_ERROR "Could not read the version out of ${name}/version.h")
        endif()
        set(_version "${CMAKE_MATCH_1}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${CMAKE_CURRENT_SOURCE_DIR}/version.h")

        string(REPLACE "_" "-" _bundle_id "${name}")
        # BUNDLE with a MODULE library is what makes CMake lay out the bundle
        # directory and put the Info.plist in it.
        set_target_properties(${name} PROPERTIES
            OUTPUT_NAME "${name}"
            BUNDLE TRUE
            BUNDLE_EXTENSION "component"
            MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/cmake/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${display_name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "org.foobar2000.component.${_bundle_id}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${_version}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${_version}")

        if(FOO_DSP_LTO)
            # With LTO the linker compiles the whole program into temporary
            # object files and throws them away, and every one of them has the
            # same name and timestamp. dsymutil finds debug info by following
            # the link map back to those objects, so it keeps the first and
            # skips the rest - with a warning, not an error, leaving a .dSYM
            # that looks like one and holds almost nothing. A directory to keep
            # them in gives each a name of its own.
            set(_lto_objects "${CMAKE_CURRENT_BINARY_DIR}/lto")
            file(MAKE_DIRECTORY "${_lto_objects}")
            target_link_options(${name} PRIVATE
                "$<$<NOT:$<CONFIG:Debug>>:LINKER:-object_path_lto,${_lto_objects}>")
        endif()
    endif()

    if(MSVC)
        target_compile_options(${name} PRIVATE /W4)
    else()
        # The SDK's interfaces fix the parameter list of every override, so an
        # unused parameter is the norm rather than a mistake.
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()

    if(WIN32)
        if(FOO_DSP_TARGET_ARCH STREQUAL "x64")
            set(_component_subdir "x64")
        elseif(FOO_DSP_TARGET_ARCH STREQUAL "ARM64EC")
            set(_component_subdir "arm64ec")
        else()
            set(_component_subdir ".")
        endif()

        install(TARGETS ${name} RUNTIME DESTINATION "${_component_subdir}")
        install(FILES "$<TARGET_PDB_FILE:${name}>"
                DESTINATION "${_component_subdir}" OPTIONAL)
    endif()
endfunction()
