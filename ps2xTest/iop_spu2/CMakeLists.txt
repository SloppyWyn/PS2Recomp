# SPU2 timing model (ps2xIOP/src/emulator/core/iop_spu2.*): ADPCM decode, loop/end flags,
# NAX advance at the pitch rate, ADSR/ENVX, KON/KOFF/ENDX, IRQA, DMA/FIFO transfers, and the
# IOP memory map / DMA channel plumbing in front of it.
add_executable(ps2_iop_spu2_tests iop_spu2_tests.cpp)
target_link_libraries(ps2_iop_spu2_tests PRIVATE ps2_iop)
target_include_directories(ps2_iop_spu2_tests PRIVATE ${CMAKE_SOURCE_DIR}/ps2xIOP/src)
add_test(NAME iop_spu2 COMMAND ps2_iop_spu2_tests)
set_tests_properties(iop_spu2 PROPERTIES LABELS "iop" TIMEOUT 60)
