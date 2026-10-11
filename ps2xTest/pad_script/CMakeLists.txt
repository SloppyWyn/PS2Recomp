# PS2X_PAD_SCRIPT: the script parser and its injection into scePadRead (Stubs/PadScript.h).
# The function table object stands in for a recompiled game (ps2_runtime needs one).
add_executable(ps2_pad_script_tests pad_script_tests.cpp $<TARGET_OBJECTS:ps2_test_function_table>)
target_link_libraries(ps2_pad_script_tests PRIVATE ps2_runtime)
if(MSVC)
    target_link_options(ps2_pad_script_tests PRIVATE "/STACK:8388608")
endif()
if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
    ps2x_stage_ffmpeg_runtime_dlls(ps2_pad_script_tests)
endif()
add_test(NAME pad_script COMMAND ps2_pad_script_tests)
set_tests_properties(pad_script PROPERTIES LABELS "pad" TIMEOUT 60)
