# ---------------------------------------------------------------------------
# foo_dsp_paraeq
# ---------------------------------------------------------------------------

# msimg32 is not needed - everything is drawn with plain GDI so the component
# keeps loading on the oldest Windows the rest of the project supports.
fb2k_component(foo_dsp_paraeq "Parametric EQ"
    SOURCES
        component.cpp
        dsp_paraeq.cpp
        paraeq_core.cpp
        paraeq_preset.cpp

        paraeq_core.h
        paraeq_preset.h
        resource.h
        stdafx.h
        version.h

    # The dialog, the curve editor it hosts and the UI element all draw with
    # GDI.
    WIN32_SOURCES
        config_dialog.cpp
        paraeq_editor.cpp
        ui_element.cpp
        foo_dsp_paraeq.rc

        paraeq_editor.h
)
