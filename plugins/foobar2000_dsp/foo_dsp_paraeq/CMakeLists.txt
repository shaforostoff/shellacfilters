# ---------------------------------------------------------------------------
# foo_dsp_paraeq
# ---------------------------------------------------------------------------

# msimg32 is not needed - everything is drawn with plain GDI so the component
# keeps loading on the oldest Windows the rest of the project supports.
fb2k_component(foo_dsp_paraeq "Parametric EQ"
    SOURCES
        component.cpp
        dsp_paraeq.cpp
        paraeq_calibrate.cpp
        paraeq_core.cpp
        paraeq_preset.cpp

        paraeq_calibrate.h
        paraeq_core.h
        paraeq_preset.h
        resource.h
        stdafx.h
        version.h

    # The dialog, the curve editor it hosts and the UI element all draw with
    # GDI.
    WIN32_SOURCES
        calibrate_dialog.cpp
        config_dialog.cpp
        paraeq_editor.cpp
        ui_element.cpp
        wasapi_capture.cpp
        foo_dsp_paraeq.rc

        paraeq_editor.h
        wasapi_capture.h

    # The same editor in Cocoa, for the DSP's configuration window and the
    # layout element.
    MAC_SOURCES
        paraeq_editor_mac.mm
        config_view_mac.mm
        ui_element_mac.mm

        paraeq_editor_mac.h
)

# The calibration window's microphone input is WASAPI, which is COM.
if(WIN32)
    target_link_libraries(foo_dsp_paraeq PRIVATE ole32)
endif()
