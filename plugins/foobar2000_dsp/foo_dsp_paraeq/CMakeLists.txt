# ---------------------------------------------------------------------------
# foo_dsp_paraeq
# ---------------------------------------------------------------------------

add_library(foo_dsp_paraeq SHARED
    component.cpp
    config_dialog.cpp
    dsp_paraeq.cpp
    paraeq_core.cpp
    paraeq_editor.cpp
    paraeq_preset.cpp
    ui_element.cpp
    foo_dsp_paraeq.rc

    paraeq_core.h
    paraeq_editor.h
    paraeq_preset.h
    resource.h
    stdafx.h
    version.h
)

# msimg32 is not needed - everything is drawn with plain GDI so the component
# keeps loading on the oldest Windows the rest of the project supports.
target_link_libraries(foo_dsp_paraeq PRIVATE fb2k::sdk comctl32)
target_include_directories(foo_dsp_paraeq PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

set_target_properties(foo_dsp_paraeq PROPERTIES
    OUTPUT_NAME "foo_dsp_paraeq"
    PREFIX ""
    SUFFIX ".dll")

if(MSVC)
    target_compile_options(foo_dsp_paraeq PRIVATE /W4)
endif()

if(FOO_DSP_TARGET_ARCH STREQUAL "x64")
    set(_component_subdir "x64")
elseif(FOO_DSP_TARGET_ARCH STREQUAL "ARM64EC")
    set(_component_subdir "arm64ec")
else()
    set(_component_subdir ".")
endif()

install(TARGETS foo_dsp_paraeq RUNTIME DESTINATION "${_component_subdir}")
install(FILES "$<TARGET_PDB_FILE:foo_dsp_paraeq>"
        DESTINATION "${_component_subdir}" OPTIONAL)
