#include "ps2recomp/code_generator.h"
#include "ps2recomp/codegen_helpers.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/types.h"
#include <fmt/format.h>
#include <sstream>
#include <cmath>

// COP2 macro-mode (VU0) instructions. The VU semantics (VF0/VI0 hardwired, no Inf/NaN/denormal
// floats, MAC/status/clip flags, DIV by zero, saturating FTOI, the R register) live in the
// runtime's ps2_vu0_macro.h (namespace ps2x_vu0); these emitters pick registers, fields and the
// helper, and drop writes to VF0/VI0.
namespace ps2recomp
{
    namespace
    {
        // VF destination pointer: VF0 is read only, so its writes are dropped (flags still set).
        std::string vfDst(uint8_t reg)
        {
            return reg == 0 ? std::string("nullptr") : fmt::format("&ctx->vu0_vf[{}]", reg);
        }

        std::string vf(uint8_t reg)
        {
            return fmt::format("ctx->vu0_vf[{}]", reg);
        }

        std::string bc(uint8_t reg, uint8_t field)
        {
            return fmt::format("ps2x_vu0::broadcast(ctx->vu0_vf[{}], {}u)", reg, field & 3u);
        }

        constexpr const char *kQ = "_mm_set1_ps(ctx->vu0_q)";
        constexpr const char *kI = "_mm_set1_ps(ctx->vu0_i)";
        constexpr const char *kAccDst = "&ctx->vu0_acc";
        constexpr const char *kAcc = "ctx->vu0_acc";

        // ps2x_vu0::<op>(ctx, dst, a, b, dest) for add/sub/mul.
        std::string fmac2(const char *op, const std::string &dst, const std::string &a, const std::string &b, uint8_t dest)
        {
            return fmt::format("ps2x_vu0::{}(ctx, {}, {}, {}, 0x{:X}u);", op, dst, a, b, dest & 0xFu);
        }

        // ps2x_vu0::madd/msub(ctx, dst, ACC, a, b, dest).
        std::string fmac3(const char *op, const std::string &dst, const std::string &a, const std::string &b, uint8_t dest)
        {
            return fmt::format("ps2x_vu0::{}(ctx, {}, {}, {}, {}, 0x{:X}u);", op, dst, kAcc, a, b, dest & 0xFu);
        }

        std::string vi(uint8_t reg)
        {
            return fmt::format("ctx->vi[{}]", reg & 0xFu);
        }

        // VU0 data memory as the EE sees it (4 KiB at 0x11004000); VI addresses are qwords.
        std::string vu0DataAddress(uint8_t viReg)
        {
            return fmt::format("(0x11004000u + ((uint32_t)(ctx->vi[{}] & 0xFFu) << 4))", viReg & 0xFu);
        }

        std::string dropped(const char *name)
        {
            return fmt::format("// {} to VI0 dropped (VI0 is hardwired to 0)", name);
        }
    }

    std::string CodeGenerator::translateVU_VADD_Field(const Instruction &inst)
    {
        return fmac2("add", vfDst(inst.sa), vf(inst.rd), bc(inst.rt, inst.function), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VSUB_Field(const Instruction &inst)
    {
        return fmac2("sub", vfDst(inst.sa), vf(inst.rd), bc(inst.rt, inst.function), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMUL_Field(const Instruction &inst)
    {
        return fmac2("mul", vfDst(inst.sa), vf(inst.rd), bc(inst.rt, inst.function), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VADD(const Instruction &inst)
    {
        return fmac2("add", vfDst(inst.sa), vf(inst.rd), vf(inst.rt), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VSUB(const Instruction &inst)
    {
        return fmac2("sub", vfDst(inst.sa), vf(inst.rd), vf(inst.rt), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMUL(const Instruction &inst)
    {
        return fmac2("mul", vfDst(inst.sa), vf(inst.rd), vf(inst.rt), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VDIV(const Instruction &inst)
    {
        return fmt::format("ps2x_vu0::vdiv(ctx, ps2x_vu0::lane({}, {}u), ps2x_vu0::lane({}, {}u));",
                           vf(inst.rd), inst.vectorInfo.fsf & 3u, vf(inst.rt), inst.vectorInfo.ftf & 3u);
    }

    std::string CodeGenerator::translateVU_VSQRT(const Instruction &inst)
    {
        return fmt::format("ps2x_vu0::vsqrt(ctx, ps2x_vu0::lane({}, {}u));", vf(inst.rt), inst.vectorInfo.ftf & 3u);
    }

    std::string CodeGenerator::translateVU_VRSQRT(const Instruction &inst)
    {
        return fmt::format("ps2x_vu0::vrsqrt(ctx, ps2x_vu0::lane({}, {}u), ps2x_vu0::lane({}, {}u));",
                           vf(inst.rd), inst.vectorInfo.fsf & 3u, vf(inst.rt), inst.vectorInfo.ftf & 3u);
    }

    std::string CodeGenerator::translateVU_VMTIR(const Instruction &inst)
    {
        if ((inst.rt & 0xFu) == 0)
            return dropped("VMTIR");
        return fmt::format("{{ uint32_t bits; float src = ps2x_vu0::lane({}, {}u); std::memcpy(&bits, &src, sizeof(bits)); {} = (uint16_t)(bits & 0xFFFFu); }}",
                           vf(inst.rd), inst.vectorInfo.fsf & 3u, vi(inst.rt));
    }

    std::string CodeGenerator::translateVU_VMFIR(const Instruction &inst)
    {
        return fmt::format("ps2x_vu0::store({}, _mm_castsi128_ps(_mm_set1_epi32((int32_t)(int16_t){})), 0x{:X}u);",
                           vfDst(inst.rt), vi(inst.rd), inst.vectorInfo.vectorField & 0xFu);
    }

    std::string CodeGenerator::translateVU_VILWR(const Instruction &inst)
    {
        // ILWR.f vit, (vis): the low 16 bits of field f of qword vis. The dest names one field.
        if ((inst.rt & 0xFu) == 0)
            return dropped("VILWR");
        const uint8_t dest = inst.vectorInfo.vectorField;
        const uint32_t field = (dest & 8u) ? 0u : (dest & 4u) ? 1u : (dest & 2u) ? 2u : 3u;
        return fmt::format("{} = (uint16_t)(READ32({} + {}u) & 0xFFFFu);", vi(inst.rt), vu0DataAddress(inst.rd), field * 4u);
    }

    std::string CodeGenerator::translateVU_VISWR(const Instruction &inst)
    {
        // ISWR.dest vit, (vis): vit (zero-extended) into every dest field of qword vis.
        const uint8_t dest = inst.vectorInfo.vectorField;
        std::string out = fmt::format("{{ const uint32_t addr = {}; const uint32_t value = (uint32_t){};", vu0DataAddress(inst.rd), vi(inst.rt));
        for (uint32_t field = 0; field < 4u; ++field)
        {
            if (dest & (8u >> field))
                out += fmt::format(" WRITE32(addr + {}u, value);", field * 4u);
        }
        return out + " }";
    }

    std::string CodeGenerator::translateVU_VIADD(const Instruction &inst)
    {
        if ((inst.sa & 0xFu) == 0)
            return dropped("VIADD");
        return fmt::format("{} = {} + {};", vi(inst.sa), vi(inst.rd), vi(inst.rt)); // vid, vis, vit
    }

    std::string CodeGenerator::translateVU_VISUB(const Instruction &inst)
    {
        if ((inst.sa & 0xFu) == 0)
            return dropped("VISUB");
        return fmt::format("{} = {} - {};", vi(inst.sa), vi(inst.rd), vi(inst.rt)); // vid, vis, vit
    }

    std::string CodeGenerator::translateVU_VIADDI(const Instruction &inst)
    {
        if ((inst.rt & 0xFu) == 0)
            return dropped("VIADDI");
        int32_t imm5 = (inst.sa & 0x10) ? static_cast<int32_t>(inst.sa | ~0x1F) : static_cast<int32_t>(inst.sa);
        return fmt::format("{} = {} + {};", vi(inst.rt), vi(inst.rd), imm5); // vit, vis, imm5
    }

    std::string CodeGenerator::translateVU_VIAND(const Instruction &inst)
    {
        if ((inst.sa & 0xFu) == 0)
            return dropped("VIAND");
        return fmt::format("{} = {} & {};", vi(inst.sa), vi(inst.rd), vi(inst.rt)); // vid, vis, vit
    }

    std::string CodeGenerator::translateVU_VIOR(const Instruction &inst)
    {
        if ((inst.sa & 0xFu) == 0)
            return dropped("VIOR");
        return fmt::format("{} = {} | {};", vi(inst.sa), vi(inst.rd), vi(inst.rt)); // vid, vis, vit
    }

    std::string CodeGenerator::translateVU_VCALLMS(const Instruction &inst)
    {
        // VCALLMS calls a VU0 microprogram at the specified immediate address.
        // VU0 micro memory is 4KB = 512 instructions (8 bytes each). Index is 0-511.
        uint16_t instr_index = static_cast<uint16_t>((inst.raw >> 6) & 0x1FF); // imm15[8:0]
        uint32_t target_byte_addr = static_cast<uint32_t>(instr_index) << 3;   // Convert instruction index to byte address

        return fmt::format(
            "{{ "
            "    ctx->vu0_tpc = 0x{:X}; " // Set target program counter
            "    runtime->executeVU0Microprogram(rdram, ctx, 0x{:X}); "
            "}}",
            target_byte_addr, target_byte_addr);
    }

    std::string CodeGenerator::translateVU_VCALLMSR(const Instruction &inst)
    {
        // VCALLMSR calls a VU0 microprogram at address stored in integer register
        uint8_t vis_reg_idx = inst.rd; // Source integer register (vis)

        return fmt::format(
            "{{ "
            "    uint16_t instr_index = ctx->vi[{}] & 0x1FF; "             // Get instruction index from VI[IS], mask to 9 bits
            "    uint32_t target_byte_addr = (uint32_t)instr_index << 3; " // Convert to byte address
            "    ctx->vu0_pc = target_byte_addr; "
            "    runtime->vu0StartMicroProgram(rdram, ctx, target_byte_addr); "
            "}}",
            vis_reg_idx);
    }

    std::string CodeGenerator::translateVU_VRNEXT(const Instruction &inst)
    {
        return fmt::format("ps2x_vu0::rnext(ctx, {}, 0x{:X}u);", vfDst(inst.rt), inst.vectorInfo.vectorField & 0xFu);
    }

    std::string CodeGenerator::translateVU_VMADD_Field(const Instruction &inst)
    {
        return fmac3("madd", vfDst(inst.sa), vf(inst.rd), bc(inst.rt, inst.function), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMSUB_Field(const Instruction &inst)
    {
        return fmac3("msub", vfDst(inst.sa), vf(inst.rd), bc(inst.rt, inst.function), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMINI_Field(const Instruction &inst)
    {
        return fmt::format("ps2x_vu0::vmini({}, {}, {}, 0x{:X}u);", vfDst(inst.sa), vf(inst.rd), bc(inst.rt, inst.function), inst.vectorInfo.vectorField & 0xFu);
    }

    std::string CodeGenerator::translateVU_VMAX_Field(const Instruction &inst)
    {
        return fmt::format("ps2x_vu0::vmax({}, {}, {}, 0x{:X}u);", vfDst(inst.sa), vf(inst.rd), bc(inst.rt, inst.function), inst.vectorInfo.vectorField & 0xFu);
    }

    std::string CodeGenerator::translateVU_VMADD(const Instruction &inst)
    {
        return fmac3("madd", vfDst(inst.sa), vf(inst.rd), vf(inst.rt), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMADDq(const Instruction &inst)
    {
        return fmac3("madd", vfDst(inst.sa), vf(inst.rd), kQ, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMADDi(const Instruction &inst)
    {
        return fmac3("madd", vfDst(inst.sa), vf(inst.rd), kI, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMAX(const Instruction &inst)
    {
        return fmt::format("ps2x_vu0::vmax({}, {}, {}, 0x{:X}u);", vfDst(inst.sa), vf(inst.rd), vf(inst.rt), inst.vectorInfo.vectorField & 0xFu);
    }

    std::string CodeGenerator::translateVU_VMAXi(const Instruction &inst)
    {
        return fmt::format("ps2x_vu0::vmax({}, {}, {}, 0x{:X}u);", vfDst(inst.sa), vf(inst.rd), kI, inst.vectorInfo.vectorField & 0xFu);
    }

    std::string CodeGenerator::translateVU_VMINIi(const Instruction &inst)
    {
        return fmt::format("ps2x_vu0::vmini({}, {}, {}, 0x{:X}u);", vfDst(inst.sa), vf(inst.rd), kI, inst.vectorInfo.vectorField & 0xFu);
    }

    std::string CodeGenerator::translateVU_VMULi(const Instruction &inst)
    {
        return fmac2("mul", vfDst(inst.sa), vf(inst.rd), kI, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMULq(const Instruction &inst)
    {
        return fmac2("mul", vfDst(inst.sa), vf(inst.rd), kQ, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VOPMSUB(const Instruction &inst)
    {
        // fd = ACC - fs.yzx * ft.zxy (the second half of a cross product after OPMULA).
        return fmac3("msub", vfDst(inst.sa), fmt::format("ps2x_vu0::yzx({})", vf(inst.rd)),
                     fmt::format("ps2x_vu0::zxy({})", vf(inst.rt)), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VADDq(const Instruction &inst)
    {
        return fmac2("add", vfDst(inst.sa), vf(inst.rd), kQ, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VADDi(const Instruction &inst)
    {
        return fmac2("add", vfDst(inst.sa), vf(inst.rd), kI, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMSUB(const Instruction &inst)
    {
        return fmac3("msub", vfDst(inst.sa), vf(inst.rd), vf(inst.rt), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMINI(const Instruction &inst)
    {
        return fmt::format("ps2x_vu0::vmini({}, {}, {}, 0x{:X}u);", vfDst(inst.sa), vf(inst.rd), vf(inst.rt), inst.vectorInfo.vectorField & 0xFu);
    }

    std::string CodeGenerator::translateVU_VSUBi(const Instruction &inst)
    {
        return fmac2("sub", vfDst(inst.sa), vf(inst.rd), kI, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VSUBq(const Instruction &inst)
    {
        return fmac2("sub", vfDst(inst.sa), vf(inst.rd), kQ, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMSUBq(const Instruction &inst)
    {
        return fmac3("msub", vfDst(inst.sa), vf(inst.rd), kQ, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMSUBi(const Instruction &inst)
    {
        return fmac3("msub", vfDst(inst.sa), vf(inst.rd), kI, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VADDA_Field(const Instruction &inst)
    {
        return fmac2("add", kAccDst, vf(inst.rd), bc(inst.rt, inst.function), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VSUBA_Field(const Instruction &inst)
    {
        return fmac2("sub", kAccDst, vf(inst.rd), bc(inst.rt, inst.function), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMADDA_Field(const Instruction &inst)
    {
        return fmac3("madd", kAccDst, vf(inst.rd), bc(inst.rt, inst.function), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMSUBA_Field(const Instruction &inst)
    {
        return fmac3("msub", kAccDst, vf(inst.rd), bc(inst.rt, inst.function), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMULA_Field(const Instruction &inst)
    {
        return fmac2("mul", kAccDst, vf(inst.rd), bc(inst.rt, inst.function), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VADDA(const Instruction &inst)
    {
        return fmac2("add", kAccDst, vf(inst.rd), vf(inst.rt), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VADDAq(const Instruction &inst)
    {
        return fmac2("add", kAccDst, vf(inst.rd), kQ, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VADDAi(const Instruction &inst)
    {
        return fmac2("add", kAccDst, vf(inst.rd), kI, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VSUBA(const Instruction &inst)
    {
        return fmac2("sub", kAccDst, vf(inst.rd), vf(inst.rt), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VSUBAq(const Instruction &inst)
    {
        return fmac2("sub", kAccDst, vf(inst.rd), kQ, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VSUBAi(const Instruction &inst)
    {
        return fmac2("sub", kAccDst, vf(inst.rd), kI, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMADDA(const Instruction &inst)
    {
        return fmac3("madd", kAccDst, vf(inst.rd), vf(inst.rt), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMADDAq(const Instruction &inst)
    {
        return fmac3("madd", kAccDst, vf(inst.rd), kQ, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMADDAi(const Instruction &inst)
    {
        return fmac3("madd", kAccDst, vf(inst.rd), kI, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMSUBA(const Instruction &inst)
    {
        return fmac3("msub", kAccDst, vf(inst.rd), vf(inst.rt), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMSUBAq(const Instruction &inst)
    {
        return fmac3("msub", kAccDst, vf(inst.rd), kQ, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMSUBAi(const Instruction &inst)
    {
        return fmac3("msub", kAccDst, vf(inst.rd), kI, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMULA(const Instruction &inst)
    {
        return fmac2("mul", kAccDst, vf(inst.rd), vf(inst.rt), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMULAq(const Instruction &inst)
    {
        return fmac2("mul", kAccDst, vf(inst.rd), kQ, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VMULAi(const Instruction &inst)
    {
        return fmac2("mul", kAccDst, vf(inst.rd), kI, inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VOPMULA(const Instruction &inst)
    {
        // ACC = fs.yzx * ft.zxy
        return fmac2("mul", kAccDst, fmt::format("ps2x_vu0::yzx({})", vf(inst.rd)),
                     fmt::format("ps2x_vu0::zxy({})", vf(inst.rt)), inst.vectorInfo.vectorField);
    }

    std::string CodeGenerator::translateVU_VITOF(const Instruction &inst, int shift)
    {
        return fmt::format("ps2x_vu0::itof({}, {}, {}u, 0x{:X}u);", vfDst(inst.rt), vf(inst.rd), shift, inst.vectorInfo.vectorField & 0xFu);
    }

    std::string CodeGenerator::translateVU_VFTOI(const Instruction &inst, int shift)
    {
        return fmt::format("ps2x_vu0::ftoi({}, {}, {}u, 0x{:X}u);", vfDst(inst.rt), vf(inst.rd), shift, inst.vectorInfo.vectorField & 0xFu);
    }

    // LQI/SQI/LQD/SQD: VU0 data memory through VI (post-increment / pre-decrement; VI0 stays 0).
    std::string CodeGenerator::translateVU_VLQI(const Instruction &inst)
    {
        const uint8_t vis = inst.rd & 0xFu;
        std::string out = fmt::format("{{ ps2x_vu0::store({}, _mm_castsi128_ps(READ128({})), 0x{:X}u);",
                                      vfDst(inst.rt), vu0DataAddress(vis), inst.vectorInfo.vectorField & 0xFu);
        if (vis != 0)
            out += fmt::format(" {} = (uint16_t)({} + 1);", vi(vis), vi(vis));
        return out + " }";
    }

    std::string CodeGenerator::translateVU_VSQI(const Instruction &inst)
    {
        const uint8_t vis = inst.rd & 0xFu;
        std::string out = fmt::format("{{ const uint32_t addr = {}; __m128 mem = _mm_castsi128_ps(READ128(addr)); "
                                      "WRITE128(addr, _mm_castps_si128(ps2x_vu0::select(mem, {}, 0x{:X}u)));",
                                      vu0DataAddress(vis), vf(inst.rt), inst.vectorInfo.vectorField & 0xFu);
        if (vis != 0)
            out += fmt::format(" {} = (uint16_t)({} + 1);", vi(vis), vi(vis));
        return out + " }";
    }

    std::string CodeGenerator::translateVU_VLQD(const Instruction &inst)
    {
        const uint8_t vis = inst.rd & 0xFu;
        std::string out = "{";
        if (vis != 0)
            out += fmt::format(" {} = (uint16_t)({} - 1);", vi(vis), vi(vis));
        out += fmt::format(" ps2x_vu0::store({}, _mm_castsi128_ps(READ128({})), 0x{:X}u);",
                           vfDst(inst.rt), vu0DataAddress(vis), inst.vectorInfo.vectorField & 0xFu);
        return out + " }";
    }

    std::string CodeGenerator::translateVU_VSQD(const Instruction &inst)
    {
        const uint8_t vis = inst.rd & 0xFu;
        std::string out = "{";
        if (vis != 0)
            out += fmt::format(" {} = (uint16_t)({} - 1);", vi(vis), vi(vis));
        out += fmt::format(" const uint32_t addr = {}; __m128 mem = _mm_castsi128_ps(READ128(addr)); "
                           "WRITE128(addr, _mm_castps_si128(ps2x_vu0::select(mem, {}, 0x{:X}u)));",
                           vu0DataAddress(vis), vf(inst.rt), inst.vectorInfo.vectorField & 0xFu);
        return out + " }";
    }

    std::string CodeGenerator::translateVU_VRGET(const Instruction &inst)
    {
        return fmt::format("ps2x_vu0::rget(ctx, {}, 0x{:X}u);", vfDst(inst.rt), inst.vectorInfo.vectorField & 0xFu);
    }

    std::string CodeGenerator::translateVU_VRINIT(const Instruction &inst)
    {
        return fmt::format("ps2x_vu0::rinit(ctx, ps2x_vu0::lane({}, {}u));", vf(inst.rd), inst.vectorInfo.fsf & 3u);
    }

    std::string CodeGenerator::translateVU_VRXOR(const Instruction &inst)
    {
        return fmt::format("ps2x_vu0::rxor(ctx, ps2x_vu0::lane({}, {}u));", vf(inst.rd), inst.vectorInfo.fsf & 3u);
    }

}
