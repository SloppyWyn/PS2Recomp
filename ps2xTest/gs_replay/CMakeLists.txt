cmake_minimum_required(VERSION 3.21)

# ps2x_gs_replay: replays a PCSX2 GS dump through the runtime's GS front end and CPU rasteriser
# (usage in gs_replay.cpp). Built from the GS sources alone, like the gs_cache suites.
get_filename_component(PS2_GS_REPLAY_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}/../../ps2xRuntime" ABSOLUTE)

find_package(Threads REQUIRED)
add_executable(ps2x_gs_replay
    gs_replay.cpp
    "${PS2_GS_REPLAY_RUNTIME_DIR}/src/lib/gs/gs_cpu_backend.cpp"
    "${PS2_GS_REPLAY_RUNTIME_DIR}/src/lib/gs/gs_frontend.cpp"
    "${PS2_GS_REPLAY_RUNTIME_DIR}/src/lib/gs/ps2_gs_memory.cpp"
)
target_include_directories(ps2x_gs_replay PRIVATE "${PS2_GS_REPLAY_RUNTIME_DIR}/include")
target_compile_features(ps2x_gs_replay PRIVATE cxx_std_20)
target_compile_definitions(ps2x_gs_replay PRIVATE PS2_RUNTIME_LOGS=0 AGRESSIVE_LOGS=0)
target_link_libraries(ps2x_gs_replay PRIVATE Threads::Threads)
# The same Release flags as ps2_runtime (/O2 /GL /fp:fast /arch:AVX2 ...), so the replay measures
# what the game runs; the rasteriser itself is /fp:precise, as in ps2_runtime.
include("${PS2_GS_REPLAY_RUNTIME_DIR}/cmake/ReleaseMode.cmake")
EnableFastReleaseMode(ps2x_gs_replay)
set_source_files_properties("${PS2_GS_REPLAY_RUNTIME_DIR}/src/lib/gs/gs_cpu_backend.cpp"
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/fp:precise>")
if(MSVC)
    # Symbols for --profile (a PDB; optimisation is unchanged).
    target_compile_options(ps2x_gs_replay PRIVATE /Zi)
    target_link_options(ps2x_gs_replay PRIVATE /DEBUG)
    target_link_libraries(ps2x_gs_replay PRIVATE dbghelp)
endif()
set_target_properties(ps2x_gs_replay PROPERTIES CXX_EXTENSIONS OFF)
