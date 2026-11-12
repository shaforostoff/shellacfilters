# ---------------------------------------------------------------------------
# Development utilities. Not part of the shipped component.
# ---------------------------------------------------------------------------

add_executable(decrackle_cli
    decrackle_cli.cpp
    wav_io.h
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_decrackle/decrackle_core.cpp"
)

target_include_directories(decrackle_cli PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_decrackle")

if(MSVC)
    target_compile_options(decrackle_cli PRIVATE /W4)
endif()

set_target_properties(decrackle_cli PROPERTIES FOLDER "tools")

add_executable(declick_cli
    declick_cli.cpp
    wav_io.h
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_declick/declick_core.cpp"
)

target_include_directories(declick_cli PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_declick")

if(MSVC)
    target_compile_options(declick_cli PRIVATE /W4)
endif()

set_target_properties(declick_cli PROPERTIES FOLDER "tools")

add_executable(dehum_cli
    dehum_cli.cpp
    wav_io.h
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_dehum/dehum_core.cpp"
)

target_include_directories(dehum_cli PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_dehum")

if(MSVC)
    target_compile_options(dehum_cli PRIVATE /W4)
endif()

set_target_properties(dehum_cli PROPERTIES FOLDER "tools")

# The ParaEQ room calibration over real hardware: plays through one device,
# records another, and runs the same analyser and WASAPI capture as the
# foobar2000 window. Windows only.
if(WIN32)
    add_executable(paraeq_calib_loopback
        paraeq_calib_loopback.cpp
        wav_io.h
        "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_paraeq/paraeq_calibrate.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_paraeq/paraeq_core.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_paraeq/wasapi_capture.cpp"
    )

    target_include_directories(paraeq_calib_loopback PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../foo_dsp_paraeq")

    target_link_libraries(paraeq_calib_loopback PRIVATE ole32)

    if(MSVC)
        target_compile_options(paraeq_calib_loopback PRIVATE /W4)
    endif()

    set_target_properties(paraeq_calib_loopback PROPERTIES FOLDER "tools")
endif()
