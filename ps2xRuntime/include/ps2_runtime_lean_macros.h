// Lean generated-code helpers: included by code generated with `[general] lean_codegen = true`
// (ps2xRecomp), after ps2_runtime_macros.h. Nothing else includes it, so with the switch off
// neither the generated code nor the runtime changes. Design: ps2xRecomp docs/LEAN-CODEGEN.md.
//
// 1. Memory access as inline functions instead of lambdas, with the guest pc published only on
//    the slow path (a special address: MMIO, scratchpad, kseg2/3, anything the runtime handles,
//    where Load*/Store* may log, raise an exception or run DMA). The fast path (a plain RAM
//    access, masked by Ps2FastRead/Write, cannot fault) stores nothing. In a delay slot the slow
//    path also publishes in_delay_slot/branch_pc the way the emitted branch code used to, so
//    raiseCop0Exception sees the same state as before.
// 2. GPR accessors as scalar 32/64-bit loads and stores of the low half of the 128-bit
//    register. Semantics are those of the SSE forms they replace: SET_GPR_* writes the low
//    64 bits (sign- or zero-extended) and leaves the upper 64 bits as they were; GPR_* reads the
//    low 32/64 bits. GPR_VEC/SET_GPR_VEC (128-bit) are unchanged.
#ifndef PS2_RUNTIME_LEAN_MACROS_H
#define PS2_RUNTIME_LEAN_MACROS_H

#include "ps2_runtime_macros.h"

#define PS2X_LEAN_DEFINE_READ(width, type)                                                                    \
    static inline type ps2xLeanRead##width(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,            \
                                           uint32_t addr, uint32_t pc, bool inDelaySlot, uint32_t branchPc) \
    {                                                                                                         \
        if (PS2Runtime::isSpecialAddress(addr)) [[unlikely]]                                                  \
        {                                                                                                     \
            ctx->pc = pc;                                                                                     \
            if (inDelaySlot)                                                                                  \
            {                                                                                                 \
                ctx->in_delay_slot = true;                                                                    \
                ctx->branch_pc = branchPc;                                                                    \
            }                                                                                                 \
            const type value = runtime->Load##width(rdram, ctx, addr);                                        \
            if (inDelaySlot)                                                                                  \
            {                                                                                                 \
                ctx->in_delay_slot = false;                                                                   \
            }                                                                                                 \
            return value;                                                                                     \
        }                                                                                                     \
        return Ps2FastRead##width(rdram, addr);                                                               \
    }

#define PS2X_LEAN_DEFINE_WRITE(width, type)                                                                   \
    static inline void ps2xLeanWrite##width(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,           \
                                            uint32_t addr, type value, uint32_t pc, bool inDelaySlot,        \
                                            uint32_t branchPc)                                               \
    {                                                                                                         \
        if (PS2Runtime::isSpecialAddress(addr)) [[unlikely]]                                                  \
        {                                                                                                     \
            ctx->pc = pc;                                                                                     \
            if (inDelaySlot)                                                                                  \
            {                                                                                                 \
                ctx->in_delay_slot = true;                                                                    \
                ctx->branch_pc = branchPc;                                                                    \
            }                                                                                                 \
            runtime->Store##width(rdram, ctx, addr, value);                                                   \
            if (inDelaySlot)                                                                                  \
            {                                                                                                 \
                ctx->in_delay_slot = false;                                                                   \
            }                                                                                                 \
            return;                                                                                           \
        }                                                                                                     \
        Ps2FastWrite##width(rdram, addr, value);                                                              \
    }

PS2X_LEAN_DEFINE_READ(8, uint8_t)
PS2X_LEAN_DEFINE_READ(16, uint16_t)
PS2X_LEAN_DEFINE_READ(32, uint32_t)
PS2X_LEAN_DEFINE_READ(64, uint64_t)
PS2X_LEAN_DEFINE_READ(128, __m128i)
PS2X_LEAN_DEFINE_WRITE(8, uint8_t)
PS2X_LEAN_DEFINE_WRITE(16, uint16_t)
PS2X_LEAN_DEFINE_WRITE(32, uint32_t)
PS2X_LEAN_DEFINE_WRITE(64, uint64_t)
PS2X_LEAN_DEFINE_WRITE(128, __m128i)

#undef PS2X_LEAN_DEFINE_READ
#undef PS2X_LEAN_DEFINE_WRITE

// PS2X_READn(addr, pc) / PS2X_WRITEn(addr, val, pc): a memory access outside a delay slot;
// the _DS forms take the branch's pc too (the access is that branch's delay slot).
#define PS2X_READ8(addr, pc) ps2xLeanRead8(rdram, ctx, runtime, (uint32_t)(addr), (pc), false, 0u)
#define PS2X_READ16(addr, pc) ps2xLeanRead16(rdram, ctx, runtime, (uint32_t)(addr), (pc), false, 0u)
#define PS2X_READ32(addr, pc) ps2xLeanRead32(rdram, ctx, runtime, (uint32_t)(addr), (pc), false, 0u)
#define PS2X_READ64(addr, pc) ps2xLeanRead64(rdram, ctx, runtime, (uint32_t)(addr), (pc), false, 0u)
#define PS2X_READ128(addr, pc) ps2xLeanRead128(rdram, ctx, runtime, (uint32_t)(addr), (pc), false, 0u)
#define PS2X_READ8_DS(addr, pc, bpc) ps2xLeanRead8(rdram, ctx, runtime, (uint32_t)(addr), (pc), true, (bpc))
#define PS2X_READ16_DS(addr, pc, bpc) ps2xLeanRead16(rdram, ctx, runtime, (uint32_t)(addr), (pc), true, (bpc))
#define PS2X_READ32_DS(addr, pc, bpc) ps2xLeanRead32(rdram, ctx, runtime, (uint32_t)(addr), (pc), true, (bpc))
#define PS2X_READ64_DS(addr, pc, bpc) ps2xLeanRead64(rdram, ctx, runtime, (uint32_t)(addr), (pc), true, (bpc))
#define PS2X_READ128_DS(addr, pc, bpc) ps2xLeanRead128(rdram, ctx, runtime, (uint32_t)(addr), (pc), true, (bpc))

#define PS2X_WRITE8(addr, val, pc) ps2xLeanWrite8(rdram, ctx, runtime, (uint32_t)(addr), (uint8_t)(val), (pc), false, 0u)
#define PS2X_WRITE16(addr, val, pc) ps2xLeanWrite16(rdram, ctx, runtime, (uint32_t)(addr), (uint16_t)(val), (pc), false, 0u)
#define PS2X_WRITE32(addr, val, pc) ps2xLeanWrite32(rdram, ctx, runtime, (uint32_t)(addr), (uint32_t)(val), (pc), false, 0u)
#define PS2X_WRITE64(addr, val, pc) ps2xLeanWrite64(rdram, ctx, runtime, (uint32_t)(addr), (uint64_t)(val), (pc), false, 0u)
#define PS2X_WRITE128(addr, val, pc) ps2xLeanWrite128(rdram, ctx, runtime, (uint32_t)(addr), (val), (pc), false, 0u)
#define PS2X_WRITE8_DS(addr, val, pc, bpc) ps2xLeanWrite8(rdram, ctx, runtime, (uint32_t)(addr), (uint8_t)(val), (pc), true, (bpc))
#define PS2X_WRITE16_DS(addr, val, pc, bpc) ps2xLeanWrite16(rdram, ctx, runtime, (uint32_t)(addr), (uint16_t)(val), (pc), true, (bpc))
#define PS2X_WRITE32_DS(addr, val, pc, bpc) ps2xLeanWrite32(rdram, ctx, runtime, (uint32_t)(addr), (uint32_t)(val), (pc), true, (bpc))
#define PS2X_WRITE64_DS(addr, val, pc, bpc) ps2xLeanWrite64(rdram, ctx, runtime, (uint32_t)(addr), (uint64_t)(val), (pc), true, (bpc))
#define PS2X_WRITE128_DS(addr, val, pc, bpc) ps2xLeanWrite128(rdram, ctx, runtime, (uint32_t)(addr), (val), (pc), true, (bpc))

#if defined(PS2X_LEAN_GPR_SCALAR) && PS2X_LEAN_GPR_SCALAR
// Scalar GPR accessors (low half of the 128-bit register; little-endian, so the low 64 bits
// are the first 8 bytes).
static inline uint32_t ps2xLeanGprU32(const R5900Context *ctx, int reg)
{
    uint32_t value;
    std::memcpy(&value, &ctx->r[reg], sizeof(value));
    return value;
}

static inline uint64_t ps2xLeanGprU64(const R5900Context *ctx, int reg)
{
    uint64_t value;
    std::memcpy(&value, &ctx->r[reg], sizeof(value));
    return value;
}

static inline void ps2xLeanSetGprLo64(R5900Context *ctx, int reg, uint64_t value)
{
    std::memcpy(&ctx->r[reg], &value, sizeof(value));
}

#undef GPR_U32
#undef GPR_S32
#undef GPR_U64
#undef GPR_S64
#undef SET_GPR_U32
#undef SET_GPR_ZE32
#undef SET_GPR_S32
#undef SET_GPR_U64
#undef SET_GPR_S64

#define GPR_U32(ctx_ptr, reg_idx) (((reg_idx) == 0) ? 0U : ps2xLeanGprU32((ctx_ptr), (reg_idx)))
#define GPR_S32(ctx_ptr, reg_idx) (((reg_idx) == 0) ? 0 : static_cast<int32_t>(ps2xLeanGprU32((ctx_ptr), (reg_idx))))
#define GPR_U64(ctx_ptr, reg_idx) (((reg_idx) == 0) ? 0ULL : ps2xLeanGprU64((ctx_ptr), (reg_idx)))
#define GPR_S64(ctx_ptr, reg_idx) (((reg_idx) == 0) ? 0LL : static_cast<int64_t>(ps2xLeanGprU64((ctx_ptr), (reg_idx))))

#define SET_GPR_U32(ctx_ptr, reg_idx, val)                                                               \
    do                                                                                                   \
    {                                                                                                    \
        if ((reg_idx) != 0)                                                                              \
            ps2xLeanSetGprLo64((ctx_ptr), (reg_idx), static_cast<uint64_t>((int64_t)(int32_t)(val)));    \
    } while (0)
#define SET_GPR_ZE32(ctx_ptr, reg_idx, val)                                                              \
    do                                                                                                   \
    {                                                                                                    \
        if ((reg_idx) != 0)                                                                              \
            ps2xLeanSetGprLo64((ctx_ptr), (reg_idx), static_cast<uint64_t>((uint32_t)(val)));            \
    } while (0)
#define SET_GPR_S32(ctx_ptr, reg_idx, val) SET_GPR_U32(ctx_ptr, reg_idx, val)
#define SET_GPR_U64(ctx_ptr, reg_idx, val)                                                               \
    do                                                                                                   \
    {                                                                                                    \
        if ((reg_idx) != 0)                                                                              \
            ps2xLeanSetGprLo64((ctx_ptr), (reg_idx), static_cast<uint64_t>((int64_t)(val)));             \
    } while (0)
#define SET_GPR_S64(ctx_ptr, reg_idx, val) SET_GPR_U64(ctx_ptr, reg_idx, val)
#endif // PS2X_LEAN_GPR_SCALAR

#endif // PS2_RUNTIME_LEAN_MACROS_H
