# ---------------------------------------------------------------------------
# foo_dsp_declick
# ---------------------------------------------------------------------------

fb2k_component(foo_dsp_declick "Declick"
    SOURCES
        component.cpp
        declick_core.cpp
        declick_preset.cpp
        dsp_declick.cpp

        declick_core.h
        declick_preset.h
        resource.h
        stdafx.h
        version.h

    WIN32_SOURCES
        config_dialog.cpp
        foo_dsp_declick.rc

    MAC_SOURCES
        config_view_mac.mm
        ../mac/slider_panel.mm
        ../mac/slider_panel.h
)
