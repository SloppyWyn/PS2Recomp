# IOP scheduling around EE RPC requests (ps2xIOP/src/emulator/iop_emulator.cpp,
# runThreadsBeforeServer): a server function runs after the ready threads that go before its
# thread, and the IOP time that takes is repaid from the next EE slices.
add_executable(ps2_iop_sched_tests iop_sched_tests.cpp)
target_link_libraries(ps2_iop_sched_tests PRIVATE ps2_iop)
target_include_directories(ps2_iop_sched_tests PRIVATE ${CMAKE_SOURCE_DIR}/ps2xIOP/src)
add_test(NAME iop_sched COMMAND ps2_iop_sched_tests)
set_tests_properties(iop_sched PROPERTIES LABELS "iop" TIMEOUT 60)
