// Typed stub ABI layer: the registry, the typed/legacy switch and the JSON dump (ps2_stub_abi.h).
#include "ps2_stub_abi.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string_view>

namespace ps2_abi
{
    const char *typeName(Type t)
    {
        switch (t)
        {
        case Type::Void: return "void";
        case Type::S8: return "s8";
        case Type::U8: return "u8";
        case Type::S16: return "s16";
        case Type::U16: return "u16";
        case Type::S32: return "s32";
        case Type::U32: return "u32";
        case Type::S64: return "s64";
        case Type::U64: return "u64";
        case Type::F32: return "f32";
        case Type::GPtr: return "gptr";
        case Type::VarArgs: return "varargs";
        }
        return "?";
    }

    bool readGuestWord(uint8_t *rdram, PS2Runtime *runtime, uint32_t addr, uint32_t &out)
    {
        if (const uint8_t *ptr = getConstMemPtr(rdram, addr))
        {
            std::memcpy(&out, ptr, sizeof(out));
            return true;
        }
        if (runtime)
        {
            try
            {
                PS2Memory &mem = runtime->memory();
                out = static_cast<uint32_t>(mem.read8(addr + 0u)) |
                      (static_cast<uint32_t>(mem.read8(addr + 1u)) << 8u) |
                      (static_cast<uint32_t>(mem.read8(addr + 2u)) << 16u) |
                      (static_cast<uint32_t>(mem.read8(addr + 3u)) << 24u);
                return true;
            }
            catch (...)
            {
            }
        }
        out = 0;
        return false;
    }

    uint64_t readStackSlot(uint8_t *rdram, PS2Runtime *runtime, uint32_t sp, uint32_t slot)
    {
        const uint32_t addr = sp + slot * 8u;
        uint32_t low = 0;
        uint32_t high = 0;
        (void)readGuestWord(rdram, runtime, addr, low);
        (void)readGuestWord(rdram, runtime, addr + 4u, high);
        return static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32);
    }

    namespace
    {
        std::vector<Entry> &mutableRegistry()
        {
            static std::vector<Entry> entries;
            return entries;
        }

        std::mutex &registryMutex()
        {
            static std::mutex m;
            return m;
        }

        // The namespace-less part of "ps2_syscalls::SifCallRpc".
        std::string_view bareName(std::string_view qualified)
        {
            const size_t pos = qualified.rfind("::");
            return pos == std::string_view::npos ? qualified : qualified.substr(pos + 2);
        }

        struct DeviationRecord
        {
            Kind kind;
            const char *name;
            const char *text;
        };

        std::vector<DeviationRecord> &deviationTable()
        {
            static std::vector<DeviationRecord> table;
            return table;
        }

        std::string jsonEscape(std::string_view text)
        {
            std::string out;
            for (const char c : text)
            {
                if (c == '"' || c == '\\')
                {
                    out.push_back('\\');
                    out.push_back(c);
                }
                else if (static_cast<unsigned char>(c) < 0x20u)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                }
                else
                {
                    out.push_back(c);
                }
            }
            return out;
        }

        std::atomic<int> g_forcedTyped{-1};

        bool typedFromEnvironment()
        {
            static const bool value = []
            {
                const char *env = std::getenv("PS2X_TYPED_STUBS");
                if (env && *env)
                {
                    return !(env[0] == '0' && env[1] == '\0');
                }
#ifdef PS2X_TYPED_STUBS_DEFAULT
                return PS2X_TYPED_STUBS_DEFAULT != 0;
#else
                return false;
#endif
            }();
            return value;
        }
    }

    Registrar::Registrar(const char *name, Kind kind, Signature sig, Handler typed, Handler legacy, Handler dispatch)
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        Entry e;
        e.name = name;
        e.kind = kind;
        e.sig = std::move(sig);
        e.typed = typed;
        e.legacy = legacy;
        e.dispatch = dispatch;
        mutableRegistry().push_back(std::move(e));
    }

    Registrar::Registrar(const char *name, Kind kind, Handler dispatch, const char *aliasOf, Kind aliasKind)
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        Entry e;
        e.name = name;
        e.kind = kind;
        e.typed = dispatch;
        e.dispatch = dispatch;
        e.aliasOf = aliasOf;
        e.aliasKind = aliasKind;
        mutableRegistry().push_back(std::move(e));
    }

    const std::vector<Entry> &registry()
    {
        return mutableRegistry();
    }

    const Entry *find(Kind kind, const char *name)
    {
        const std::string_view wanted(name);
        for (const Entry &e : registry())
        {
            if (e.kind == kind && wanted == e.name)
                return &e;
        }
        return nullptr;
    }

    const Entry *resolveAlias(const Entry &entry)
    {
        const Entry *current = &entry;
        for (int guard = 0; guard < 8 && current && current->aliasOf; ++guard)
        {
            const std::string target(bareName(current->aliasOf));
            current = find(current->aliasKind, target.c_str());
        }
        return current;
    }

    DeviationNote::DeviationNote(Kind kind, const char *name, const char *text)
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        deviationTable().push_back({kind, name, text});
    }

    const char *deviation(Kind kind, const char *name)
    {
        const std::string_view wanted(name);
        std::lock_guard<std::mutex> lock(registryMutex());
        for (const DeviationRecord &d : deviationTable())
        {
            if (d.kind == kind && wanted == d.name)
                return d.text;
        }
        return nullptr;
    }

    bool typedEnabled()
    {
        const int forced = g_forcedTyped.load(std::memory_order_relaxed);
        if (forced >= 0)
            return forced != 0;
        return typedFromEnvironment();
    }

    void setTypedEnabledForTests(int value)
    {
        g_forcedTyped.store(value < 0 ? -1 : (value != 0 ? 1 : 0), std::memory_order_relaxed);
    }

    std::string registryJson()
    {
        std::ostringstream os;
        os << "{\n \"abi\": \"ee-eabi64\",\n \"version\": 1,\n \"handlers\": [\n";
        // Sorted by table and name: static-initialisation order differs between builds.
        std::vector<const Entry *> sorted;
        for (const Entry &e : registry())
            sorted.push_back(&e);
        std::sort(sorted.begin(), sorted.end(), [](const Entry *a, const Entry *b)
                  {
                      if (a->kind != b->kind)
                          return a->kind < b->kind;
                      return std::string_view(a->name) < std::string_view(b->name);
                  });
        bool first = true;
        for (const Entry *ep : sorted)
        {
            const Entry &e = *ep;
            const Entry *sig = resolveAlias(e);
            os << (first ? "  {" : ",\n  {");
            first = false;
            os << "\"name\": \"" << e.name << "\", \"kind\": \"" << (e.kind == Kind::Stub ? "stub" : "syscall") << "\"";
            os << ", \"form\": \"" << (e.aliasOf ? "alias" : (e.legacy ? "typed+legacy" : "typed")) << "\"";
            if (e.aliasOf)
            {
                os << ", \"alias_of\": \"" << bareName(e.aliasOf) << "\", \"alias_kind\": \""
                   << (e.aliasKind == Kind::Stub ? "stub" : "syscall") << "\"";
            }
            const char *dev = deviation(e.kind, e.name);
            if (!dev && sig && sig != &e)
                dev = deviation(sig->kind, sig->name); // an alias inherits its target's note
            if (dev)
            {
                os << ", \"deviation\": \"" << jsonEscape(dev) << "\"";
            }
            if (sig)
            {
                const Signature &s = sig->sig;
                os << ", \"ret\": \"" << typeName(s.ret) << "\"";
                if (s.selfReturn)
                    os << ", \"self_return\": true";
                os << ", \"params\": [";
                for (size_t i = 0; i < s.params.size(); ++i)
                    os << (i ? ", \"" : "\"") << typeName(s.params[i]) << "\"";
                os << "], \"int_args\": " << s.intArgs << ", \"float_args\": " << s.floatArgs
                   << ", \"stack_slots\": " << s.stackSlots << ", \"varargs\": " << (s.varargs ? "true" : "false");
            }
            else
            {
                os << ", \"unresolved\": true";
            }
            os << "}";
        }
        os << "\n ]\n}\n";
        return os.str();
    }
}
