# Guest names, overlay naming and the PS2X_TRACE_CALLS filter/output (ps2_guest_names.h,
# ps2_call_trace.h). The function table object stands in for a recompiled game.
add_executable(ps2_debug_names_tests debug_names_tests.cpp $<TARGET_OBJECTS:ps2_test_function_table>)
target_link_libraries(ps2_debug_names_tests PRIVATE ps2_runtime)
if(MSVC)
    target_link_options(ps2_debug_names_tests PRIVATE "/STACK:8388608")
endif()
if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
    ps2x_stage_ffmpeg_runtime_dlls(ps2_debug_names_tests)
endif()
add_test(NAME debug_names COMMAND ps2_debug_names_tests)
set_tests_properties(debug_names PROPERTIES LABELS "debug" TIMEOUT 60)
