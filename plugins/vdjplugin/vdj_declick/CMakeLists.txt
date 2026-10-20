# One plug-in. See ../CMakeLists.txt for why it is the buffer interface and not
# the live one.
#
# declick_core.{h,cpp} is a mirror, not a source: it is copied here from
# ../../foobar2000_dsp/foo_dsp_declick by scripts/sync_cores.ps1 (or .sh) in
# that tree, and an edit made here is an edit that will be overwritten. Change
# the canonical copy.

add_vdj_plugin(Declick
    SOURCES declick_plugin.cpp
            declick_core.cpp
            declick_core.h
            declick_engine.h
            declick_vdj_warm.h)
