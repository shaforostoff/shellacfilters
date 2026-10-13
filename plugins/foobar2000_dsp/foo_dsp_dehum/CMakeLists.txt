# ---------------------------------------------------------------------------
# foo_dsp_dehum
# ---------------------------------------------------------------------------

fb2k_component(foo_dsp_dehum "Dehum"
    SOURCES
        component.cpp
        dehum_core.cpp
        dehum_preset.cpp
        dehum_scout.cpp
        dsp_dehum.cpp

        dehum_core.h
        dehum_preset.h
        dehum_scout.h
        resource.h
        stdafx.h
        version.h

    WIN32_SOURCES
        config_dialog.cpp
        foo_dsp_dehum.rc

    MAC_SOURCES
        config_view_mac.mm
        ../mac/slider_panel.mm
        ../mac/slider_panel.h
)
