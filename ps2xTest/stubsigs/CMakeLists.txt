# ps2x_stubsigs: dumps the typed stub ABI registry (ps2_stub_abi.h) as JSON for
# tools/stubaudit.py --signatures and tools/bindcheck.py. Usage: ps2x_stubsigs <out.json>
# (no argument: stdout). The function table object stands in for a recompiled game.
add_executable(ps2x_stubsigs stubsigs.cpp $<TARGET_OBJECTS:ps2_test_function_table>)
target_link_libraries(ps2x_stubsigs PRIVATE ps2_runtime ps2_iop)
if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
    ps2x_stage_ffmpeg_runtime_dlls(ps2x_stubsigs)
endif()
if(BUILD_TESTING)
    add_test(NAME stubsigs COMMAND ps2x_stubsigs)
    set_tests_properties(stubsigs PROPERTIES LABELS "debug" TIMEOUT 60)
endif()
