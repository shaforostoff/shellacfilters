# ---------------------------------------------------------------------------
# Tests
#
#   decrackle_verify   - DSP core vs. a literal transcription of the Airwindows
#                        VST source, plus robustness and throughput. No SDK.
#   declick_verify     - the declicker's AR linear algebra vs. dense brute
#                        force, plus streaming properties. No SDK.
#   declick_rt_verify  - counts heap allocations on the processing path and
#                        requires zero. No SDK.
#   declick_vst_verify - the WinVST port of the declicker vs. the core it
#                        shares with this one. Built against
#                        plugins/WinVST/vst2_shim, not Steinberg's SDK. No SDK.
#   dehum_verify       - the dehummer's FFT, notch, frequency tracker and
#                        detector against independent references. No SDK.
#   dehum_rt_verify    - same allocation count for the dehummer, whose detector
#                        runs on the audio thread. No SDK.
#   dehum_vst_verify   - the WinVST port of the dehummer vs. the core it shares.
#                        No SDK.
#                        These two are Windows targets only because this project
#                        is; scripts/build_linuxvst.sh builds and runs the same
#                        two sources against the LinuxVST copies, which are
#                        required to be byte-identical. So are the MacVST ones,
#                        which is why MacVST gets no target of its own.
#   declick_au_verify  - the MacAU port of the declicker vs. the core it shares.
#                        MacAU is a wrapper in its own right rather than a
#                        mirror of the VST one, so unlike MacVST it does need a
#                        target. Built against plugins/MacAU/au_shim, not
#                        Apple's CoreAudio sources. No SDK.
#   dehum_au_verify    - the MacAU port of the dehummer, same arrangement.
#                        No SDK.
#   vst_host_verify    - NOT a target here. It loads a finished VST2 plug-in and
#                        drives it over the C ABI, so it needs a built one to
#                        point at; scripts\build_winvst.ps1 and
#                        scripts/build_linuxvst.sh each compile and run it
#                        against every plug-in they produce.
#   preset_roundtrip   - every parameter survives save/load. Links the SDK, so
#                        it needs foobar2000's shared.dll; skips if absent.
#   component_smoke    - loads the built DLL through foobar2000's own service
#                        plumbing. Needs foobar2000 installed; skips if absent.
# ---------------------------------------------------------------------------

add_executable(decrackle_verify
    decrackle_verify.cpp
    decrackle_reference.h
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_decrackle/decrackle_core.cpp"
)

target_include_directories(decrackle_verify PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_decrackle")

if(MSVC)
    target_compile_options(decrackle_verify PRIVATE /W4)
    # The oracle is a verbatim copy of Airwindows code; its warnings are not
    # ours to fix.
    set_source_files_properties(decrackle_verify.cpp PROPERTIES
        COMPILE_OPTIONS "/wd4244;/wd4305;/wd4101")
endif()

add_test(NAME decrackle_verify COMMAND decrackle_verify)

# ---------------------------------------------------------------------------
# The declicker's linear algebra, against dense brute force. No SDK.

add_executable(declick_verify
    declick_verify.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_declick/declick_core.cpp"
)

target_include_directories(declick_verify PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_declick")

if(MSVC)
    target_compile_options(declick_verify PRIVATE /W4)
endif()

add_test(NAME declick_verify COMMAND declick_verify)
set_target_properties(declick_verify PROPERTIES FOLDER "tests")

# ---------------------------------------------------------------------------
# The audio thread must not touch the heap. Replaces global operator new and
# requires the count to be zero across the processing path and every parameter
# change. No SDK.

add_executable(declick_rt_verify
    declick_rt_verify.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_declick/declick_core.cpp"
)

target_include_directories(declick_rt_verify PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_declick")

if(MSVC)
    target_compile_options(declick_rt_verify PRIVATE /W4)
endif()

add_test(NAME declick_rt_verify COMMAND declick_rt_verify)
set_target_properties(declick_rt_verify PROPERTIES FOLDER "tests")

# ---------------------------------------------------------------------------
# The dehummer: the hand-rolled FFT, the notch depth against its analytic floor,
# the frequency tracker against tones of known frequency, and the detector
# against material with and without a line in it. No SDK.

add_executable(dehum_verify
    dehum_verify.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_dehum/dehum_core.cpp"
)

target_include_directories(dehum_verify PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_dehum")

if(MSVC)
    target_compile_options(dehum_verify PRIVATE /W4)
endif()

add_test(NAME dehum_verify COMMAND dehum_verify)
set_target_properties(dehum_verify PROPERTIES FOLDER "tests")

# ---------------------------------------------------------------------------
# Dehum's audio thread must not touch the heap either - and it has more to get
# wrong, since a whole FFT detector runs on it. No SDK.

add_executable(dehum_rt_verify
    dehum_rt_verify.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_dehum/dehum_core.cpp"
)

target_include_directories(dehum_rt_verify PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_dehum")

if(MSVC)
    target_compile_options(dehum_rt_verify PRIVATE /W4)
endif()

add_test(NAME dehum_rt_verify COMMAND dehum_rt_verify)
set_target_properties(dehum_rt_verify PROPERTIES FOLDER "tests")

# ---------------------------------------------------------------------------
# The parametric equaliser: the cookbook sections against the formulas, the
# glide against the stability triangle it is safe by, and the fast drawing path
# against the slow one it replaces. No SDK.

add_executable(paraeq_verify
    paraeq_verify.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_paraeq/paraeq_core.cpp"
)

target_include_directories(paraeq_verify PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_paraeq")

if(MSVC)
    target_compile_options(paraeq_verify PRIVATE /W4)
endif()

add_test(NAME paraeq_verify COMMAND paraeq_verify)
set_target_properties(paraeq_verify PROPERTIES FOLDER "tests")

# ---------------------------------------------------------------------------
# The WinVST port of the declicker, against the core it shares with this tree.
#
# It compiles the WinVST folder's OWN copy of declick_core.cpp, deliberately:
# that is the file the VST project builds, so this is testing the artefact that
# ships rather than a stand-in for it. Whether that copy is still byte-identical
# to the canonical one is a different question, and the answer is
# scripts/sync_cores.ps1 -Check, which build_release.ps1 runs first.
#
# vst2_shim must come first on the include path: that is where the
# "audioeffectx.h" the plug-in asks for is found. It is a clean-room
# reimplementation of the VST2 ABI - Steinberg's is not in this repository - and
# it is the same one scripts/build_winvst.ps1 links into the shipped DLL, so what
# is compiled here is what ships. Two things it cannot establish: that the
# plug-in compiles against the real SDK, and anything about the ABI itself, since
# this test links the plug-in rather than loading it. The second gap is what
# vst_host_verify.cpp closes; see plugins/WinVST/vst2_shim/README.md.

set(WINVST_SHIM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../WinVST/vst2_shim")
set(WINVST_DECLICK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../WinVST/Declick")

if(EXISTS "${WINVST_DECLICK_DIR}/Declick.cpp")
    add_executable(declick_vst_verify
        declick_vst_verify.cpp
        "${WINVST_SHIM_DIR}/vst2_abi.h"
        "${WINVST_SHIM_DIR}/audioeffectx.h"
        "${WINVST_SHIM_DIR}/audioeffectx.cpp"
        "${WINVST_DECLICK_DIR}/Declick.cpp"
        "${WINVST_DECLICK_DIR}/DeclickProc.cpp"
        "${WINVST_DECLICK_DIR}/declick_core.cpp"
    )

    target_include_directories(declick_vst_verify PRIVATE
        "${WINVST_SHIM_DIR}"
        "${WINVST_DECLICK_DIR}")

    if(MSVC)
        target_compile_options(declick_vst_verify PRIVATE /W4)
        # The wrapper follows the Airwindows house pattern - `A = 0.5;` on a
        # float, `*out1 = inputSampleL;` from a double, and setChunk() ignoring
        # byteSize on purpose - so it warns exactly where the other 519 plug-ins
        # do. Those are not ours to fix either.
        set_source_files_properties(
            "${WINVST_DECLICK_DIR}/Declick.cpp"
            "${WINVST_DECLICK_DIR}/DeclickProc.cpp"
            PROPERTIES COMPILE_OPTIONS "/wd4244;/wd4305;/wd4100")
    endif()

    add_test(NAME declick_vst_verify COMMAND declick_vst_verify)
    set_target_properties(declick_vst_verify PROPERTIES FOLDER "tests")
else()
    message(STATUS "plugins/WinVST/Declick not present - skipping declick_vst_verify")
endif()

# ---------------------------------------------------------------------------
# The WinVST port of the dehummer, against the core it shares with this tree.
# Same arrangement as declick_vst_verify above: it compiles the WinVST folder's
# own copy of dehum_core.cpp, because that is the file the VST project builds.
# Whether that copy is still byte-identical is scripts/sync_cores.ps1 -Check.

set(WINVST_DEHUM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../WinVST/Dehum")

if(EXISTS "${WINVST_DEHUM_DIR}/Dehum.cpp")
    add_executable(dehum_vst_verify
        dehum_vst_verify.cpp
        "${WINVST_SHIM_DIR}/audioeffectx.cpp"
        "${WINVST_DEHUM_DIR}/Dehum.cpp"
        "${WINVST_DEHUM_DIR}/DehumProc.cpp"
        "${WINVST_DEHUM_DIR}/dehum_core.cpp"
    )

    target_include_directories(dehum_vst_verify PRIVATE
        "${WINVST_SHIM_DIR}"
        "${WINVST_DEHUM_DIR}")

    if(MSVC)
        target_compile_options(dehum_vst_verify PRIVATE /W4)
        # The wrapper follows the Airwindows house pattern - `A = 0.5;` on a
        # float, `out1[i] = inputSampleL;` from a double, and setChunk()
        # ignoring byteSize on purpose - so it warns exactly where the other 519
        # plug-ins do. Those are not ours to fix.
        set_source_files_properties(
            "${WINVST_DEHUM_DIR}/Dehum.cpp"
            "${WINVST_DEHUM_DIR}/DehumProc.cpp"
            PROPERTIES COMPILE_OPTIONS "/wd4244;/wd4305;/wd4100")
    endif()

    add_test(NAME dehum_vst_verify COMMAND dehum_vst_verify)
    set_target_properties(dehum_vst_verify PROPERTIES FOLDER "tests")
else()
    message(STATUS "plugins/WinVST/Dehum not present - skipping dehum_vst_verify")
endif()

# ---------------------------------------------------------------------------
# The MacAU ports, against the cores they share with this tree.
#
# Same arrangement again: each compiles the MacAU folder's OWN copy of the core,
# because that is the file the Audio Unit project builds. Mirror identity is
# scripts/sync_cores.{ps1,sh}.
#
# au_shim must come first on the include path: that is where the "AUEffectBase.h"
# the plug-in asks for is found. Apple's CoreAudio AUPublic and PublicUtility
# sources are not in this repository - the .xcodeproj reaches for them under
# $(SYSTEM_DEVELOPER_DIR), where no Xcode has put them for over a decade - so it
# is a small stand-in that lets the wrapper compile and be driven. Unlike
# vst2_shim it is not an ABI and nothing loads it, so there is no AU counterpart
# of vst_host_verify - building a .component needs the Apple sources the shim
# exists because of. See plugins/MacAU/au_shim/README.md.
#
# There is no MacVST equivalent of these, for the same reason there is no
# LinuxVST one: MacVST/Declick/source and MacVST/Dehum/source are required to be
# byte-identical to the WinVST wrapper by sync_cores, so declick_vst_verify and
# dehum_vst_verify are already compiling that code or the mirror check fails
# first.

set(MACAU_SHIM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../MacAU/au_shim")

foreach(_au Declick Dehum)
    string(TOLOWER "${_au}" _core)
    set(_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../MacAU/${_au}")

    if(EXISTS "${_dir}/${_au}.cpp")
        add_executable(${_core}_au_verify
            ${_core}_au_verify.cpp
            "${MACAU_SHIM_DIR}/AUEffectBase.h"
            "${_dir}/${_au}.cpp"
            "${_dir}/${_core}_core.cpp"
        )

        target_include_directories(${_core}_au_verify PRIVATE
            "${MACAU_SHIM_DIR}"
            "${_dir}")

        if(MSVC)
            target_compile_options(${_core}_au_verify PRIVATE /W4)
            # The wrapper follows the Airwindows AU house pattern - the template's
            # unused parameters, `fpdL = 1.0;` on a uint32_t, and a float store
            # from a double - so it warns exactly where the other 540 do. Those
            # are not ours to fix.
            set_source_files_properties("${_dir}/${_au}.cpp"
                PROPERTIES COMPILE_OPTIONS "/wd4244;/wd4305;/wd4100")
        endif()

        add_test(NAME ${_core}_au_verify COMMAND ${_core}_au_verify)
        set_target_properties(${_core}_au_verify PROPERTIES FOLDER "tests")
    else()
        message(STATUS "plugins/MacAU/${_au} not present - skipping ${_core}_au_verify")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Preset serialisation. Always built - a compile break here is worth catching
# on every architecture - but only registered as a test where shared.dll can
# be placed beside it (see the smoke-test block below). The logic under test
# is architecture-independent, so running it on one architecture is enough.

include("${CMAKE_SOURCE_DIR}/cmake/fb2k_find_runtime.cmake")
fb2k_find_runtime("${FOO_DSP_TARGET_ARCH}" FB2K_RUNTIME_DIR)

add_executable(preset_roundtrip
    preset_roundtrip.cpp
    stdafx_host.h
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_declick/declick_preset.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_declick/declick_core.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_decrackle/decrackle_preset.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_decrackle/decrackle_core.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_dehum/dehum_preset.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_dehum/dehum_core.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_paraeq/paraeq_preset.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_paraeq/paraeq_core.cpp"
)

target_include_directories(preset_roundtrip PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_declick"
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_decrackle"
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_dehum"
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_paraeq")

target_link_libraries(preset_roundtrip PRIVATE fb2k::sdk)

if(MSVC)
    target_compile_options(preset_roundtrip PRIVATE /W3)
endif()

if(FB2K_RUNTIME_DIR)
    add_custom_command(TARGET preset_roundtrip POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${FB2K_RUNTIME_DIR}/shared.dll" "$<TARGET_FILE_DIR:preset_roundtrip>/"
        VERBATIM)
    add_test(NAME preset_roundtrip COMMAND preset_roundtrip)
endif()

set_target_properties(preset_roundtrip PROPERTIES FOLDER "tests")

# ---------------------------------------------------------------------------

if(NOT FB2K_RUNTIME_DIR)
    message(STATUS "No ${FOO_DSP_TARGET_ARCH} foobar2000 install found "
                   "- skipping the smoke and preset tests "
                   "(set FOOBAR2000_DIR to point at one)")
else()
    message(STATUS "Component smoke test will use ${FB2K_RUNTIME_DIR}")

    add_executable(component_smoke component_smoke.cpp stdafx_host.h)
    target_link_libraries(component_smoke PRIVATE fb2k::sdk)
    if(MSVC)
        target_compile_options(component_smoke PRIVATE /W3)
    endif()

    # The host links against shared.dll just like foobar2000's own binaries do,
    # and the SDK touches it during static initialisation - so it has to sit
    # next to the executable rather than being found later at run time.
    add_custom_command(TARGET component_smoke POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${FB2K_RUNTIME_DIR}/shared.dll" "$<TARGET_FILE_DIR:component_smoke>/"
        VERBATIM)

    add_dependencies(component_smoke foo_dsp_decrackle foo_dsp_declick foo_dsp_dehum
                     foo_dsp_paraeq)

    add_test(NAME smoke_decrackle
             COMMAND component_smoke "$<TARGET_FILE:foo_dsp_decrackle>" "${FB2K_RUNTIME_DIR}"
                     --dialog 101 --slider 1001 --label 1011)
    add_test(NAME smoke_declick
             COMMAND component_smoke "$<TARGET_FILE:foo_dsp_declick>" "${FB2K_RUNTIME_DIR}"
                     --dialog 201 --slider 2001 --label 2011)
    add_test(NAME smoke_dehum
             COMMAND component_smoke "$<TARGET_FILE:foo_dsp_dehum>" "${FB2K_RUNTIME_DIR}"
                     --dialog 301 --slider 3001 --label 3011)
    # --editor instead of --slider: the equaliser's dialog is owner-drawn, so
    # there is no trackbar to move and the probe types at the editor instead.
    add_test(NAME smoke_paraeq
             COMMAND component_smoke "$<TARGET_FILE:foo_dsp_paraeq>" "${FB2K_RUNTIME_DIR}"
                     --dialog 401 --editor 4002)
    # These open a real modal window; do not let them wedge CI.
    set_tests_properties(smoke_decrackle smoke_declick smoke_dehum smoke_paraeq
                         PROPERTIES TIMEOUT 120)
    set_target_properties(component_smoke PROPERTIES FOLDER "tests")
endif()

set_target_properties(decrackle_verify PROPERTIES FOLDER "tests")
