# PS2X_NO_AUDIO / PS2X_MUTE / headless: when the runner opens the host audio device
# (ps2_headless.h decideAudioDevice) and the backend's null sink without one.
# The function table object stands in for a recompiled game.
add_executable(ps2_audio_device_tests audio_device_tests.cpp $<TARGET_OBJECTS:ps2_test_function_table>)
target_link_libraries(ps2_audio_device_tests PRIVATE ps2_runtime)
if(MSVC)
    target_link_options(ps2_audio_device_tests PRIVATE "/STACK:8388608")
endif()
if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
    ps2x_stage_ffmpeg_runtime_dlls(ps2_audio_device_tests)
endif()
add_test(NAME audio_device COMMAND ps2_audio_device_tests)
set_tests_properties(audio_device PROPERTIES LABELS "audio" TIMEOUT 60)
