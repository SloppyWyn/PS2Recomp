// Typed stub ABI layer: a runtime handler declares its C signature once and a generated
// wrapper decodes the arguments from the guest register file and stack the way the EE
// calling convention lays them out, then writes the return value back. Game-agnostic.
//
// The EE ABI (ee-gcc "EABI" with 64-bit GPRs), as the games' own SDK copies show it
// (docs/STUB-AUDIT.md, Walls 7 and 8):
//   - integer-class arguments 1-8 (int, pointer, long / long long, double: soft-float) go in
//     a0-a3, t0-t3 (regs 4-11), one 64-bit register each, whatever the type's width;
//   - float arguments go in f12-f19, counted separately from the integer-class ones;
//   - once a class has used up its registers, its further arguments go on the caller's stack
//     in 8-byte slots from sp+0, in argument order (slot k at sp + 8k; a 32-bit value is the
//     low word of its slot). Nothing is reserved for a0-a3 "home slots": sp+0x10.. holds the
//     caller's locals. The one float case seen so far (sceVu0ViewScreenMatrix's ninth float at
//     sp+0) matches; mixed overflow of both classes has not been seen in a game yet.
//     The slot of every declared argument is fixed at compile time (argLayout); a 32-bit or
//     narrower stack argument reads only the low word of its slot. Stack words are read through
//     the direct RAM/scratchpad pointer, else the runtime's memory map (0 when unmapped).
//   - varargs continue the integer-class sequence: slot i < 8 is register 4+i, slot i >= 8 is
//     sp + 8(i-8), each 8 bytes (Ps2VarArgCursor / Ps2VaListCursor semantics).
//   - return values: int / pointer in v0 (sign-extended to 64 bits), 64-bit integers in v0
//     (with v1 = high word as well, setReturnU64's convention), float in f0.
//
// Declaring a handler (inside namespace ps2_stubs or ps2_syscalls, PS2_STUB_KIND set by the
// library's Common.h):
//
//     // sceSifSendCmd(cid, pkt, psize, src, dest, size)
//     PS2_STUB(int32_t, sceSifSendCmd, uint32_t cid, ps2_abi::gptr pkt, uint32_t psize,
//              ps2_abi::gptr src, ps2_abi::gptr dest, uint32_t size)
//     {
//         ...            // rdram, ctx, runtime are in scope, as in a plain handler
//         return 1;      // written to v0
//     }
//
// Argument types: int8_t..uint64_t, float, ps2_abi::gptr (a guest address: converts to
// uint32_t), and ps2_abi::VarArgs as the last parameter for printf-style handlers. Return
// types: void (v0 untouched), int32_t/uint32_t/gptr (v0), int64_t/uint64_t (v0+v1), float (f0),
// or ps2_abi::SelfReturn<T> when a shared body sets the registers itself (T is recorded for the
// tooling only).
//
// Transition forms, for a handler that keeps its old decoding until the typed one is proven:
//
//     PS2_STUB_TYPED(int32_t, name, <params>) { ... }            // defines name_typed
//     void name_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { ... old ... }
//     PS2_STUB_DISPATCH(name)                                    // name() picks one at run time
//
// The dispatcher calls the typed wrapper when PS2X_TYPED_STUBS=1 is set (or the build sets
// PS2X_TYPED_STUBS_DEFAULT=1), the legacy function otherwise. PS2_STUB_ALIAS(name, target)
// defines a handler that forwards to another one (registered as an alias, so the tooling
// knows its signature). PS2_STUB0 / PS2_STUB0_TYPED are the no-argument forms.
//
// Every declaration registers its signature (ps2_abi::registry()); ps2x_stubsigs dumps the
// table as JSON for tools/stubaudit.py and tools/bindcheck.py, and ps2x_tests runs each
// typed/legacy pair on random register and stack states (ps2_stub_abi_tests.cpp).
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"

namespace ps2_abi
{
    using Handler = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    // A guest address. Converts to and from uint32_t so handler bodies read as before.
    struct gptr
    {
        uint32_t addr = 0;
        constexpr gptr() = default;
        constexpr gptr(uint32_t a) : addr(a) {}
        constexpr operator uint32_t() const { return addr; }
    };

    // Return type of a typed handler whose body writes v0/v1/f0 itself (a shared body between the
    // typed and the legacy form). T is the declared C return type, kept for the tooling.
    template <typename T>
    struct SelfReturn
    {
    };

    enum class Type : uint8_t
    {
        Void,
        S8,
        U8,
        S16,
        U16,
        S32,
        U32,
        S64,
        U64,
        F32,
        GPtr,
        VarArgs
    };

    enum class Kind : uint8_t
    {
        Stub,
        Syscall
    };

    const char *typeName(Type t);

    // Reads one 32-bit word of guest memory for an argument slot: RAM and scratchpad through
    // the direct pointer, anything else through the runtime's memory map (an unmapped address
    // reads as 0). The same path Ps2VarArgCursor took for stack slots.
    bool readGuestWord(uint8_t *rdram, PS2Runtime *runtime, uint32_t addr, uint32_t &out);
    uint64_t readStackSlot(uint8_t *rdram, PS2Runtime *runtime, uint32_t sp, uint32_t slot);

    // The variable arguments of a printf-style handler: one 8-byte slot per argument,
    // continuing the integer-class sequence after the fixed arguments (slots 0-7 are a0-t3,
    // slot i >= 8 is the caller's sp + 8(i-8)).
    class VarArgs
    {
    public:
        VarArgs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t firstSlot)
            : m_rdram(rdram), m_ctx(ctx), m_runtime(runtime), m_sp(getRegU32(ctx, 29)), m_slot(firstSlot),
              m_first(firstSlot)
        {
        }

        uint32_t nextU32() { return static_cast<uint32_t>(nextU64()); }

        uint64_t nextU64()
        {
            const uint64_t value = peekSlot(m_slot);
            ++m_slot;
            return value;
        }

        uint64_t peekSlot(uint32_t slot) const
        {
            if (slot < 8u)
            {
                uint64_t value = 0;
                std::memcpy(&value, &m_ctx->r[4 + slot], sizeof(value));
                return value;
            }
            return readStackSlot(m_rdram, m_runtime, m_sp, slot - 8u);
        }

        // The i-th variable argument (0 = the first), low word, without moving the cursor.
        uint32_t peekU32(uint32_t i) const { return static_cast<uint32_t>(peekSlot(m_first + i)); }

        uint32_t slot() const { return m_slot; }
        uint32_t firstSlot() const { return m_first; }

    private:
        uint8_t *m_rdram;
        R5900Context *m_ctx;
        PS2Runtime *m_runtime;
        uint32_t m_sp;
        uint32_t m_slot;
        uint32_t m_first;
    };

    // ---- type codes --------------------------------------------------------------------
    template <typename T>
    struct TypeCode;
    template <> struct TypeCode<void> { static constexpr Type value = Type::Void; };
    template <> struct TypeCode<int8_t> { static constexpr Type value = Type::S8; };
    template <> struct TypeCode<uint8_t> { static constexpr Type value = Type::U8; };
    template <> struct TypeCode<int16_t> { static constexpr Type value = Type::S16; };
    template <> struct TypeCode<uint16_t> { static constexpr Type value = Type::U16; };
    template <> struct TypeCode<int32_t> { static constexpr Type value = Type::S32; };
    template <> struct TypeCode<uint32_t> { static constexpr Type value = Type::U32; };
    template <> struct TypeCode<int64_t> { static constexpr Type value = Type::S64; };
    template <> struct TypeCode<uint64_t> { static constexpr Type value = Type::U64; };
    template <> struct TypeCode<float> { static constexpr Type value = Type::F32; };
    template <> struct TypeCode<gptr> { static constexpr Type value = Type::GPtr; };
    template <> struct TypeCode<VarArgs> { static constexpr Type value = Type::VarArgs; };
    template <typename T> struct TypeCode<SelfReturn<T>> { static constexpr Type value = TypeCode<T>::value; };

    template <typename T>
    struct IsSelfReturn : std::false_type {};
    template <typename T>
    struct IsSelfReturn<SelfReturn<T>> : std::true_type {};

    // ---- argument layout ----------------------------------------------------------------
    // Where each declared argument lives, worked out at compile time from the parameter types
    // (so decoding never depends on the order in which the decoders are evaluated).
    enum class SlotKind : uint8_t
    {
        IntReg,   // a0-a3, t0-t3: index 0-7 = GPR 4+index (low 64 bits)
        FloatReg, // f12-f19: index 0-7 = f[12+index]
        Stack,    // the 8-byte slot `index` from the caller's sp
        VarArgs   // the variable arguments: index = the first integer-class slot they take
    };

    struct ArgSlot
    {
        SlotKind kind = SlotKind::IntReg;
        uint32_t index = 0;
    };

    template <typename... Args>
    constexpr std::array<ArgSlot, sizeof...(Args)> argLayout()
    {
        constexpr Type types[] = {TypeCode<std::decay_t<Args>>::value..., Type::Void};
        std::array<ArgSlot, sizeof...(Args)> out{};
        uint32_t ints = 0, floats = 0, stack = 0;
        for (size_t i = 0; i < sizeof...(Args); ++i)
        {
            const Type t = types[i];
            if (t == Type::VarArgs)
            {
                // Varargs continue the integer-class sequence: slot i < 8 is GPR 4+i, slot i >= 8
                // the stack slot i-8. The named stack arguments come first, so this holds as long
                // as no float argument overflowed to the stack (never seen in a printf-style call).
                out[i] = {SlotKind::VarArgs, ints};
            }
            else if (t == Type::F32)
            {
                out[i] = floats < 8u ? ArgSlot{SlotKind::FloatReg, floats} : ArgSlot{SlotKind::Stack, stack++};
                ++floats;
            }
            else
            {
                out[i] = ints < 8u ? ArgSlot{SlotKind::IntReg, ints} : ArgSlot{SlotKind::Stack, stack++};
                ++ints;
            }
        }
        return out;
    }

    // ---- decoding ----------------------------------------------------------------------
    inline uint64_t registerLow64(const R5900Context *ctx, uint32_t gpr)
    {
        uint64_t value = 0;
        std::memcpy(&value, &ctx->r[gpr], sizeof(value)); // low 64 bits
        return value;
    }

    template <typename T>
    inline T decodeAt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, ArgSlot slot)
    {
        if constexpr (std::is_same_v<T, VarArgs>)
        {
            return VarArgs(rdram, ctx, runtime, slot.index);
        }
        else if constexpr (std::is_same_v<T, float>)
        {
            if (slot.kind == SlotKind::FloatReg)
            {
                return ctx->f[12 + slot.index];
            }
            uint32_t bits = 0;
            (void)readGuestWord(rdram, runtime, getRegU32(ctx, 29) + slot.index * 8u, bits);
            float f = 0.0f;
            std::memcpy(&f, &bits, sizeof(f));
            return f;
        }
        else if constexpr (std::is_same_v<T, gptr> || std::is_integral_v<T>)
        {
            uint64_t raw = 0;
            if (slot.kind == SlotKind::IntReg)
            {
                raw = registerLow64(ctx, 4u + slot.index);
            }
            else if constexpr (sizeof(T) > 4u)
            {
                raw = readStackSlot(rdram, runtime, getRegU32(ctx, 29), slot.index);
            }
            else
            {
                // A 32-bit or narrower value is the low word of its slot: the high word is not read.
                uint32_t word = 0;
                (void)readGuestWord(rdram, runtime, getRegU32(ctx, 29) + slot.index * 8u, word);
                raw = word;
            }
            if constexpr (std::is_same_v<T, gptr>)
            {
                return gptr{static_cast<uint32_t>(raw)};
            }
            else
            {
                // A narrower type takes the low bits of its 64-bit slot (the EE extends ints to
                // 64 bits in the register; shorts and chars are promoted the same way).
                return static_cast<T>(raw);
            }
        }
        else
        {
            static_assert(sizeof(T) == 0, "unsupported typed-stub argument type");
        }
    }

    template <typename T>
    inline void writeReturn(R5900Context *ctx, T value)
    {
        if constexpr (std::is_same_v<T, float>)
        {
            ctx->f[0] = value;
        }
        else if constexpr (std::is_same_v<T, gptr>)
        {
            setReturnU32(ctx, value.addr);
        }
        else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t>)
        {
            setReturnU64(ctx, static_cast<uint64_t>(value));
        }
        else if constexpr (std::is_integral_v<T>)
        {
            // int, unsigned, short, char: the low 32 bits, sign-extended to 64 (R5900 convention).
            setReturnS32(ctx, static_cast<int32_t>(value));
        }
        else
        {
            static_assert(sizeof(T) == 0, "unsupported typed-stub return type");
        }
    }

    template <typename Ret, typename... Args, size_t... I>
    inline Ret callDecoded(Ret (*impl)(uint8_t *, R5900Context *, PS2Runtime *, Args...),
                           uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, std::index_sequence<I...>)
    {
        constexpr std::array<ArgSlot, sizeof...(Args)> layout = argLayout<Args...>();
        (void)layout; // unused by a no-argument handler
        return impl(rdram, ctx, runtime, decodeAt<std::decay_t<Args>>(rdram, ctx, runtime, layout[I])...);
    }

    template <typename Ret, typename... Args>
    inline void invoke(Ret (*impl)(uint8_t *, R5900Context *, PS2Runtime *, Args...),
                       uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if constexpr (std::is_void_v<Ret> || IsSelfReturn<Ret>::value)
        {
            (void)callDecoded(impl, rdram, ctx, runtime, std::index_sequence_for<Args...>{});
        }
        else
        {
            writeReturn<Ret>(ctx, callDecoded(impl, rdram, ctx, runtime, std::index_sequence_for<Args...>{}));
        }
    }

    // ---- signatures and the registry -----------------------------------------------------
    struct Signature
    {
        Type ret = Type::Void;
        bool selfReturn = false;          // the body writes the return registers itself
        std::vector<Type> params;         // in declaration order (VarArgs last, if any)
        bool varargs = false;
        uint32_t intArgs = 0;             // integer-class fixed arguments (registers and stack)
        uint32_t floatArgs = 0;           // float arguments (registers and stack)
        uint32_t stackSlots = 0;          // 8-byte slots from sp+0 holding fixed arguments
    };

    template <typename Ret, typename... Args>
    inline Signature signatureOf(Ret (*)(uint8_t *, R5900Context *, PS2Runtime *, Args...))
    {
        Signature s;
        s.ret = TypeCode<std::decay_t<Ret>>::value;
        s.selfReturn = IsSelfReturn<std::decay_t<Ret>>::value;
        s.params = {TypeCode<std::decay_t<Args>>::value...};
        constexpr std::array<ArgSlot, sizeof...(Args)> layout = argLayout<Args...>();
        for (size_t i = 0; i < layout.size(); ++i)
        {
            const Type t = s.params[i];
            if (t == Type::VarArgs)
                s.varargs = true;
            else if (t == Type::F32)
                ++s.floatArgs;
            else
                ++s.intArgs;
            if (layout[i].kind == SlotKind::Stack)
                ++s.stackSlots;
        }
        return s;
    }

    struct Entry
    {
        const char *name = nullptr;
        Kind kind = Kind::Stub;
        Signature sig;
        Handler typed = nullptr;    // the typed wrapper (also `dispatch` for a final handler)
        Handler legacy = nullptr;   // the old decoding, kept during the transition (or null)
        Handler dispatch = nullptr; // the public handler the call tables name
        const char *aliasOf = nullptr; // the handler this one forwards to (signature taken from it)
        Kind aliasKind = Kind::Stub;
    };

    const std::vector<Entry> &registry();
    const Entry *find(Kind kind, const char *name);
    // The entry an alias resolves to (itself for a plain handler; null when the target is missing).
    const Entry *resolveAlias(const Entry &entry);

    struct Registrar
    {
        Registrar(const char *name, Kind kind, Signature sig, Handler typed, Handler legacy, Handler dispatch);
        // An alias: the signature is the target's, looked up when the table is read (the target
        // may live in another translation unit, so it cannot be copied at static-init time).
        Registrar(const char *name, Kind kind, Handler dispatch, const char *aliasOf, Kind aliasKind);
    };

    // PS2X_TYPED_STUBS=1 selects the typed wrappers in every dispatcher, =0 the legacy code;
    // unset: PS2X_TYPED_STUBS_DEFAULT (0 unless the build says otherwise).
    bool typedEnabled();
    // Tests only: force the choice (-1 = back to the environment).
    void setTypedEnabledForTests(int value);

    // A deliberate deviation of a handler's declaration from the SDK prototype (kept to preserve
    // the old decoding, e.g. a u32 where the SDK says short, or a void SDK function whose handler
    // writes v0): recorded with PS2_STUB_DEVIATION and dumped as "deviation" in the JSON, so the
    // tooling (stubaudit / bindcheck) reports it as known instead of as a mismatch.
    struct DeviationNote
    {
        DeviationNote(Kind kind, const char *name, const char *text);
    };
    // The deviation text for a handler, or null.
    const char *deviation(Kind kind, const char *name);

    // The whole table as JSON (one object per handler), for the tooling.
    std::string registryJson();
}

#ifndef PS2_STUB_KIND
#define PS2_STUB_KIND ::ps2_abi::Kind::Stub
#endif

#define PS2_ABI_HANDLER_PARAMS uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime

// Final typed handler: `name` is the typed wrapper.
#define PS2_STUB(ret, name, ...)                                                                            \
    static ret name##_impl(PS2_ABI_HANDLER_PARAMS, __VA_ARGS__);                                            \
    void name(PS2_ABI_HANDLER_PARAMS) { ::ps2_abi::invoke(&name##_impl, rdram, ctx, runtime); }             \
    static const ::ps2_abi::Registrar name##_abi_registrar(#name, PS2_STUB_KIND,                            \
                                                           ::ps2_abi::signatureOf(&name##_impl), &name,     \
                                                           nullptr, &name);                                 \
    static ret name##_impl(PS2_ABI_HANDLER_PARAMS, __VA_ARGS__)

#define PS2_STUB0(ret, name)                                                                                \
    static ret name##_impl(PS2_ABI_HANDLER_PARAMS);                                                         \
    void name(PS2_ABI_HANDLER_PARAMS) { ::ps2_abi::invoke(&name##_impl, rdram, ctx, runtime); }             \
    static const ::ps2_abi::Registrar name##_abi_registrar(#name, PS2_STUB_KIND,                            \
                                                           ::ps2_abi::signatureOf(&name##_impl), &name,     \
                                                           nullptr, &name);                                 \
    static ret name##_impl(PS2_ABI_HANDLER_PARAMS)

// Transition: typed wrapper `name_typed`, legacy `name_legacy` written by hand, dispatcher `name`.
#define PS2_STUB_TYPED(ret, name, ...)                                                                      \
    static ret name##_impl(PS2_ABI_HANDLER_PARAMS, __VA_ARGS__);                                            \
    void name##_typed(PS2_ABI_HANDLER_PARAMS) { ::ps2_abi::invoke(&name##_impl, rdram, ctx, runtime); }     \
    static ret name##_impl(PS2_ABI_HANDLER_PARAMS, __VA_ARGS__)

#define PS2_STUB0_TYPED(ret, name)                                                                          \
    static ret name##_impl(PS2_ABI_HANDLER_PARAMS);                                                         \
    void name##_typed(PS2_ABI_HANDLER_PARAMS) { ::ps2_abi::invoke(&name##_impl, rdram, ctx, runtime); }     \
    static ret name##_impl(PS2_ABI_HANDLER_PARAMS)

#define PS2_STUB_DISPATCH(name)                                                                             \
    void name(PS2_ABI_HANDLER_PARAMS)                                                                       \
    {                                                                                                       \
        if (::ps2_abi::typedEnabled())                                                                      \
            name##_typed(rdram, ctx, runtime);                                                              \
        else                                                                                                \
            name##_legacy(rdram, ctx, runtime);                                                             \
    }                                                                                                       \
    static const ::ps2_abi::Registrar name##_abi_registrar(#name, PS2_STUB_KIND,                            \
                                                           ::ps2_abi::signatureOf(&name##_impl),            \
                                                           &name##_typed, &name##_legacy, &name);

// A handler that forwards to another typed handler: `target` is how it is called from here
// (qualified if it lives in the other namespace), `targetKind` the registry it is in.
// Marks a handler's declaration as a deliberate deviation from the SDK prototype (text: what and
// why), e.g. PS2_STUB_DEVIATION(scePrintf, "SDK returns int; v0 never written (kept)").
#define PS2_STUB_DEVIATION(name, text)                                                                      \
    static const ::ps2_abi::DeviationNote name##_abi_deviation(PS2_STUB_KIND, #name, text);

#define PS2_STUB_ALIAS(name, target, targetKind)                                                            \
    void name(PS2_ABI_HANDLER_PARAMS) { target(rdram, ctx, runtime); }                                      \
    static const ::ps2_abi::Registrar name##_abi_registrar(#name, PS2_STUB_KIND, &name, #target, targetKind);
