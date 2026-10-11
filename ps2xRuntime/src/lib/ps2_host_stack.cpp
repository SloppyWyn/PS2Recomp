// Native stack walking and symbol names: see ps2_host_stack.h. Split out of
// ps2_watchdog.cpp so the crash handler can share it.
#include "ps2_host_stack.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib") // CMake also links dbghelp; this is a fallback
#endif

// The native stack walk reads x64 CONTEXT registers (Rip/Rsp) and x64 unwind data.
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define PS2X_HOST_STACKS 1
#else
#define PS2X_HOST_STACKS 0
#endif

namespace
{
#if PS2X_HOST_STACKS
    std::recursive_mutex g_symbolMutex; // the map vector and dbghelp are not thread-safe

    struct MapSymbol
    {
        uint64_t rva;
        std::string name;
    };
    std::vector<MapSymbol> g_mapSymbols;
    bool g_mapLoaded = false;
    bool g_symInitialized = false;

    std::string exePath()
    {
        char buffer[MAX_PATH * 2] = {};
        const DWORD n = GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(sizeof(buffer)));
        return std::string(buffer, n);
    }

    // MSVC linker map (/MAP): "Preferred load address is X" then lines
    // " 0001:00000000       ?name@@...      0000000140001000 f   obj".
    void loadMap()
    {
        if (g_mapLoaded)
        {
            return;
        }
        g_mapLoaded = true;
        std::string path = exePath();
        const size_t dot = path.find_last_of('.');
        if (dot != std::string::npos)
        {
            path = path.substr(0, dot);
        }
        path += ".map";
        std::ifstream in(path);
        if (!in)
        {
            std::fprintf(stderr, "[hoststack] no linker map at %s (exe frames stay as offsets)\n", path.c_str());
            return;
        }
        uint64_t preferred = 0x140000000ull;
        std::string line;
        while (std::getline(in, line))
        {
            if (const size_t at = line.find("Preferred load address is"); at != std::string::npos)
            {
                preferred = std::strtoull(line.c_str() + at + 25, nullptr, 16);
                continue;
            }
            unsigned section = 0;
            unsigned offset = 0;
            char name[1024];
            unsigned long long rvaBase = 0;
            if (line.size() < 20 || line[0] != ' ' || line[5] != ':')
            {
                continue;
            }
            if (std::sscanf(line.c_str(), " %x:%x %1023s %llx", &section, &offset, name, &rvaBase) == 4 &&
                section != 0 && rvaBase >= preferred)
            {
                g_mapSymbols.push_back(MapSymbol{rvaBase - preferred, name});
            }
        }
        std::sort(g_mapSymbols.begin(), g_mapSymbols.end(), [](const MapSymbol &a, const MapSymbol &b)
                  { return a.rva < b.rva; });
        std::fprintf(stderr, "[hoststack] %zu symbols from %s\n", g_mapSymbols.size(), path.c_str());
    }

    // The exe's range, for naming without the loader lock (GetModuleHandleEx takes it).
    uint64_t g_exeBase = 0;
    uint64_t g_exeEnd = 0;
    char g_exeName[MAX_PATH] = "exe";
    std::atomic<bool> g_ready{false}; // map loaded and dbghelp initialised (preload)

    void initExeRange()
    {
        if (g_exeBase != 0)
        {
            return;
        }
        const auto base = reinterpret_cast<const uint8_t *>(GetModuleHandleA(nullptr));
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        const char *name = std::strrchr(path, '\\');
        std::snprintf(g_exeName, sizeof(g_exeName), "%s", name ? name + 1 : path);
        g_exeEnd = reinterpret_cast<uint64_t>(base) + nt->OptionalHeader.SizeOfImage;
        g_exeBase = reinterpret_cast<uint64_t>(base);
    }

    void initDbghelp()
    {
        if (!g_symInitialized)
        {
            g_symInitialized = true;
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_EXACT_SYMBOLS);
            SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        }
    }

    // Caller holds g_symbolMutex. mayLoad = false: never parse the map or start dbghelp
    // (the crash path; preload() did that), so nothing here waits on the loader lock.
    void describeInto(uint64_t address, char *text, size_t size, bool mayLoad)
    {
        initExeRange();
        if (address >= g_exeBase && address < g_exeEnd)
        {
            const uint64_t rva = address - g_exeBase;
            if (mayLoad)
            {
                loadMap();
            }
            auto it = std::upper_bound(g_mapSymbols.begin(), g_mapSymbols.end(), rva,
                                       [](uint64_t value, const MapSymbol &s)
                                       { return value < s.rva; });
            if (it != g_mapSymbols.begin())
            {
                --it;
                char undecorated[1024] = {};
                const char *shown = it->name.c_str();
                if (UnDecorateSymbolName(it->name.c_str(), undecorated, sizeof(undecorated), UNDNAME_NAME_ONLY) != 0)
                {
                    shown = undecorated;
                }
                std::snprintf(text, size, "%s+0x%llx %s+0x%llx", g_exeName, static_cast<unsigned long long>(rva),
                              shown, static_cast<unsigned long long>(rva - it->rva));
                return;
            }
            std::snprintf(text, size, "%s+0x%llx", g_exeName, static_cast<unsigned long long>(rva));
            return;
        }
        if (mayLoad)
        {
            initDbghelp();
        }
        if (g_symInitialized)
        {
            const HANDLE process = GetCurrentProcess();
            IMAGEHLP_MODULE64 module{};
            module.SizeOfStruct = sizeof(module);
            const char *moduleName = SymGetModuleInfo64(process, address, &module) ? module.ModuleName : "?";
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 512] = {};
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, address, &displacement, symbol))
            {
                std::snprintf(text, size, "%s!%s+0x%llx", moduleName, symbol->Name,
                              static_cast<unsigned long long>(displacement));
                return;
            }
            if (module.BaseOfImage != 0)
            {
                std::snprintf(text, size, "%s+0x%llx", moduleName,
                              static_cast<unsigned long long>(address - module.BaseOfImage));
                return;
            }
        }
        std::snprintf(text, size, "0x%llx", static_cast<unsigned long long>(address));
    }

    std::string threadName(HANDLE thread)
    {
        using GetThreadDescriptionFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
        static auto fn = reinterpret_cast<GetThreadDescriptionFn>(
            GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "GetThreadDescription"));
        if (!fn)
        {
            return {};
        }
        PWSTR description = nullptr;
        if (FAILED(fn(thread, &description)) || !description)
        {
            return {};
        }
        std::string out;
        for (const wchar_t *p = description; *p; ++p)
        {
            out.push_back(*p < 0x80 ? static_cast<char>(*p) : '?');
        }
        LocalFree(description);
        return out;
    }

    // Unwinds a copy of the context. Nothing here allocates: a suspended thread may hold
    // the heap lock.
    size_t walkFrom(const CONTEXT *start, uint64_t *frames, size_t maxFrames)
    {
        CONTEXT context = *start;
        size_t count = 0;
        while (count < maxFrames && context.Rip != 0)
        {
            frames[count++] = context.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (!entry)
            {
                // Leaf function: the return address is on top of the stack.
                if (context.Rsp == 0)
                {
                    break;
                }
                context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
                context.Rsp += 8;
                continue;
            }
            PVOID handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, entry, &context, &handlerData,
                             &establisherFrame, nullptr);
        }
        return count;
    }
#endif
}

namespace ps2x_hoststack
{
    bool supported() noexcept
    {
        return PS2X_HOST_STACKS != 0;
    }

#if PS2X_HOST_STACKS
    std::string describeAddress(uint64_t address)
    {
        std::lock_guard lock(g_symbolMutex);
        char text[1400];
        describeInto(address, text, sizeof(text), true);
        return text;
    }

    void preload()
    {
        static std::atomic<bool> started{false};
        if (started.exchange(true))
        {
            return;
        }
        std::thread([]
                    {
                        std::lock_guard lock(g_symbolMutex);
                        initExeRange();
                        loadMap();
                        initDbghelp();
                        g_ready.store(true, std::memory_order_release);
                    })
            .detach();
    }

    bool preloaded() noexcept
    {
        return g_ready.load(std::memory_order_acquire);
    }

    size_t describeAddressNoWait(uint64_t address, char *text, size_t size)
    {
        if (!text || size == 0)
        {
            return 0;
        }
        std::unique_lock lock(g_symbolMutex, std::try_to_lock);
        if (!lock.owns_lock() || !g_ready.load(std::memory_order_acquire))
        {
            // Busy (another thread is naming, or it crashed holding the lock) or not
            // loaded yet: the bare address, exe-relative when it is in the exe.
            if (g_exeBase != 0 && address >= g_exeBase && address < g_exeEnd)
            {
                std::snprintf(text, size, "%s+0x%llx", g_exeName, static_cast<unsigned long long>(address - g_exeBase));
            }
            else
            {
                std::snprintf(text, size, "0x%llx", static_cast<unsigned long long>(address));
            }
        }
        else
        {
            describeInto(address, text, size, false);
        }
        return std::strlen(text);
    }

    void testHoldSymbolLock()
    {
        std::thread([]
                    {
                        g_symbolMutex.lock();
                        Sleep(INFINITE);
                    })
            .detach();
        Sleep(100);
    }

    size_t walkSuspendedThread(void *threadHandle, uint64_t *frames, size_t maxFrames)
    {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(static_cast<HANDLE>(threadHandle), &context))
        {
            return 0;
        }
        return walkFrom(&context, frames, maxFrames);
    }

    size_t walkContext(const void *context, uint64_t *frames, size_t maxFrames)
    {
        if (!context)
        {
            return 0;
        }
        return walkFrom(static_cast<const CONTEXT *>(context), frames, maxFrames);
    }

    std::string threadName(void *threadHandle)
    {
        return ::threadName(static_cast<HANDLE>(threadHandle));
    }
#else
    std::string describeAddress(uint64_t address)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
        return text;
    }
    void preload() {}
    bool preloaded() noexcept { return false; }
    size_t describeAddressNoWait(uint64_t address, char *text, size_t size)
    {
        return static_cast<size_t>(std::snprintf(text, size, "0x%llx", static_cast<unsigned long long>(address)));
    }
    void testHoldSymbolLock() {}
    size_t walkSuspendedThread(void *, uint64_t *, size_t) { return 0; }
    size_t walkContext(const void *, uint64_t *, size_t) { return 0; }
    std::string threadName(void *) { return {}; }
#endif
}
