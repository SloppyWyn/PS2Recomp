# sceGsSetDefDBuff / sceGsSwapDBuff / sceGsPutDrawEnv checked against SDK libgraph
# (expectations from SLPS-25887's own libgraph copy, see the source).
# The function table object stands in for a recompiled game (ps2_runtime needs one).
add_executable(ps2_gs_dbuff_tests gs_dbuff_tests.cpp $<TARGET_OBJECTS:ps2_test_function_table>)
target_link_libraries(ps2_gs_dbuff_tests PRIVATE ps2_runtime)
if(MSVC)
    target_link_options(ps2_gs_dbuff_tests PRIVATE "/STACK:8388608")
endif()
if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
    ps2x_stage_ffmpeg_runtime_dlls(ps2_gs_dbuff_tests)
endif()
add_test(NAME gs_dbuff COMMAND ps2_gs_dbuff_tests)
set_tests_properties(gs_dbuff PROPERTIES LABELS "gs" TIMEOUT 120)

# libgraph's graphics-mode state (sceGsGParam), sceGszbufaddr and sceGsSetDefDrawEnv(2)
# against SLPS-25887's libgraph (gen_gparam.py).
add_executable(ps2_gs_gparam_tests gs_gparam_tests.cpp $<TARGET_OBJECTS:ps2_test_function_table>)
target_link_libraries(ps2_gs_gparam_tests PRIVATE ps2_runtime)
if(MSVC)
    target_link_options(ps2_gs_gparam_tests PRIVATE "/STACK:8388608")
endif()
if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
    ps2x_stage_ffmpeg_runtime_dlls(ps2_gs_gparam_tests)
endif()
add_test(NAME gs_gparam COMMAND ps2_gs_gparam_tests)
set_tests_properties(gs_gparam PROPERTIES LABELS "gs" TIMEOUT 120)
