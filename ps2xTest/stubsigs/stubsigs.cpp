// ps2x_stubsigs <out.json>: writes the typed stub ABI registry (every handler declared through
// ps2_stub_abi.h: its signature, form and alias target) as JSON. Without an argument the JSON
// goes to stdout. Exit code 0, or 1 when the file cannot be written or an alias is unresolved.
//
// The handlers live in the ps2_runtime static library, and a translation unit is only linked
// (and its registrars only run) when something references it: every handler in the syscall
// and stub tables is referenced here, as the call tables of a game would.
#include "ps2_call_list.h"
#include "ps2_stub_abi.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"

#include <cstdio>
#include <fstream>
#include <iostream>

namespace
{
    using Handler = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    volatile Handler g_allHandlers[] = {
#define PS2X_SIGS_SYSCALL(name) &ps2_syscalls::name,
        PS2_SYSCALL_LIST(PS2X_SIGS_SYSCALL)
#undef PS2X_SIGS_SYSCALL
#define PS2X_SIGS_STUB(name) &ps2_stubs::name,
        PS2_STUB_LIST(PS2X_SIGS_STUB)
#undef PS2X_SIGS_STUB
    };
}

int main(int argc, char **argv)
{
    size_t referenced = 0;
    for (size_t i = 0; i < sizeof(g_allHandlers) / sizeof(g_allHandlers[0]); ++i)
        referenced += g_allHandlers[i] != nullptr ? 1u : 0u;

    const std::string json = ps2_abi::registryJson();
    int unresolved = 0;
    for (const ps2_abi::Entry &e : ps2_abi::registry())
        unresolved += ps2_abi::resolveAlias(e) ? 0 : 1;

    if (argc > 1)
    {
        std::ofstream out(argv[1], std::ios::binary);
        out << json;
        if (!out)
        {
            std::cerr << "ps2x_stubsigs: cannot write " << argv[1] << "\n";
            return 1;
        }
        std::cerr << "ps2x_stubsigs: " << ps2_abi::registry().size() << " typed handlers (of " << referenced
                  << " runtime handlers), " << unresolved << " unresolved aliases -> " << argv[1] << "\n";
    }
    else
    {
        std::cout << json;
    }
    return unresolved ? 1 : 0;
}
