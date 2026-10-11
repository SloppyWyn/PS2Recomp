#include "runtime/ps2_deterministic.h"
#include "Common.h"
#include "MemoryCard.h"

namespace ps2_stubs
{
    namespace
    {
        constexpr int32_t kMcCmdGetInfo = 0x01;
        constexpr int32_t kMcCmdOpen = 0x02;
        constexpr int32_t kMcCmdClose = 0x03;
        constexpr int32_t kMcCmdSeek = 0x04;
        constexpr int32_t kMcCmdRead = 0x05;
        constexpr int32_t kMcCmdWrite = 0x06;
        constexpr int32_t kMcCmdFlush = 0x0A;
        constexpr int32_t kMcCmdMkdir = 0x0B;
        constexpr int32_t kMcCmdChdir = 0x0C;
        constexpr int32_t kMcCmdGetDir = 0x0D;
        constexpr int32_t kMcCmdSetFileInfo = 0x0E;
        constexpr int32_t kMcCmdDelete = 0x0F;
        constexpr int32_t kMcCmdFormat = 0x10;
        constexpr int32_t kMcCmdUnformat = 0x11;
        constexpr int32_t kMcCmdGetEntSpace = 0x12;
        constexpr int32_t kMcCmdRename = 0x13;
        // The *Fast variants report their own RPC function numbers through
        // sceMcSync (checked against SDK 3.1.0 libmc: sceMcReadFast stores 0x16,
        // sceMcWriteFast 0x1B into the pending-command slot).
        constexpr int32_t kMcCmdReadFast = 0x16;
        constexpr int32_t kMcCmdWriteFast = 0x1B;

        constexpr int32_t kMcResultSucceed = 0;
        constexpr int32_t kMcResultChangedCard = -1;
        constexpr int32_t kMcResultNoFormat = -2;
        constexpr int32_t kMcResultNoEntry = -4;
        constexpr int32_t kMcResultDeniedPermit = -5;
        constexpr int32_t kMcResultNotEmpty = -6;
        constexpr int32_t kMcResultUpLimitHandle = -7;

        constexpr int32_t kMcTypePs2 = 2;
        constexpr int32_t kMcFormatted = 1;
        constexpr int32_t kMcUnformatted = 0;
        constexpr int32_t kMcFreeClusters = 0x2000;
        constexpr size_t kMcMaxPathLen = 1024;
        constexpr size_t kMcMaxOpenFiles = 32;

        constexpr uint16_t kMcAttrReadable = 0x0001;
        constexpr uint16_t kMcAttrWriteable = 0x0002;
        constexpr uint16_t kMcAttrFile = 0x0010;
        constexpr uint16_t kMcAttrSubdir = 0x0020;
        constexpr uint16_t kMcAttrClosed = 0x0080;
        constexpr uint16_t kMcAttrExists = 0x8000;

        struct SceMcStDateTime
        {
            uint8_t Resv2 = 0;
            uint8_t Sec = 0;
            uint8_t Min = 0;
            uint8_t Hour = 0;
            uint8_t Day = 0;
            uint8_t Month = 0;
            uint16_t Year = 0;
        };

        struct SceMcTblGetDir
        {
            SceMcStDateTime _Create{};
            SceMcStDateTime _Modify{};
            uint32_t FileSizeByte = 0;
            uint16_t AttrFile = 0;
            uint16_t Reserve1 = 0;
            uint32_t Reserve2 = 0;
            uint32_t PdaAplNo = 0;
            char EntryName[32]{};
        };

        static_assert(sizeof(SceMcTblGetDir) == 64, "sceMcTblGetDir size mismatch");

        struct McOpenFile
        {
            FILE *file = nullptr;
            int32_t port = 0;
            std::filesystem::path hostPath;
        };

        struct McPortState
        {
            std::string currentDir = "/";
            bool formatted = true;
        };

        std::mutex g_mcStateMutex;
        int32_t g_mcNextFd = 1;
        int32_t g_mcLastCmd = 0;
        bool g_mcCommandPending = false;
        int32_t g_mcLastResult = 0;
        std::unordered_map<int32_t, McOpenFile> g_mcFiles;
        std::array<McPortState, 2> g_mcPorts{};
        int32_t g_cvMcFileCursor = 0;

        constexpr int32_t kCvMcSaveFileBytes = 0x838;
        constexpr int32_t kCvMcConfigFileBytes = 0x34;
        constexpr int32_t kCvMcIconInfoBytes = 0x3C4;
        constexpr int32_t kCvMcIconFileBytes = 0xB3F8;

        constexpr int32_t cvMcKilobytes(int32_t bytes)
        {
            return (bytes + 1023) / 1024;
        }

        constexpr int32_t kCvMcRequiredFreeKb =
            cvMcKilobytes(kCvMcSaveFileBytes) * 15 +
            cvMcKilobytes(kCvMcConfigFileBytes) +
            cvMcKilobytes(kCvMcIconInfoBytes) +
            cvMcKilobytes(kCvMcIconFileBytes) + 11;

        bool isValidMcPortSlot(int32_t port, int32_t slot)
        {
            return port >= 0 && port < static_cast<int32_t>(g_mcPorts.size()) && slot == 0;
        }

        std::filesystem::path getMcRootPath(int32_t port)
        {
            const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
            std::filesystem::path root = paths.mcRoot;
            if (root.empty())
            {
                if (!paths.elfDirectory.empty())
                {
                    root = paths.elfDirectory / "mc0";
                }
                else
                {
                    std::error_code ec;
                    const std::filesystem::path cwd = std::filesystem::current_path(ec);
                    root = ec ? std::filesystem::path("mc0") : (cwd / "mc0");
                }
            }

            root = root.lexically_normal();
            if (port <= 0)
            {
                return root;
            }

            const std::filesystem::path parent = root.parent_path();
            const std::string leaf = root.filename().string();
            const std::string lowerLeaf = toLowerAscii(leaf);
            if (lowerLeaf == "mc0")
            {
                return (parent / "mc1").lexically_normal();
            }
            if (leaf.empty())
            {
                return (root / "mc1").lexically_normal();
            }

            return (parent / (leaf + "_slot" + std::to_string(port))).lexically_normal();
        }

        void ensureMcRootExists(int32_t port)
        {
            std::error_code ec;
            std::filesystem::create_directories(getMcRootPath(port), ec);
        }

        std::vector<std::string> splitMcPathComponents(const std::string &value)
        {
            std::vector<std::string> parts;
            std::string current;
            for (char c : value)
            {
                if (c == '/' || c == '\\')
                {
                    if (!current.empty())
                    {
                        parts.push_back(current);
                        current.clear();
                    }
                }
                else
                {
                    current.push_back(c);
                }
            }

            if (!current.empty())
            {
                parts.push_back(current);
            }

            return parts;
        }

        std::string joinMcPathComponents(const std::vector<std::string> &parts)
        {
            if (parts.empty())
            {
                return "/";
            }

            std::string joined = "/";
            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (i != 0u)
                {
                    joined.push_back('/');
                }
                joined.append(parts[i]);
            }
            return joined;
        }

        std::string normalizeGuestMcPathLocked(int32_t port, std::string path)
        {
            std::replace(path.begin(), path.end(), '\\', '/');
            const std::string lower = toLowerAscii(path);
            if (lower.rfind("mc0:", 0) == 0 || lower.rfind("mc1:", 0) == 0)
            {
                path = path.substr(4);
            }

            const bool absolute = !path.empty() && path.front() == '/';
            std::vector<std::string> parts;
            if (!absolute && port >= 0 && port < static_cast<int32_t>(g_mcPorts.size()))
            {
                parts = splitMcPathComponents(g_mcPorts[static_cast<size_t>(port)].currentDir);
            }

            for (const std::string &part : splitMcPathComponents(path))
            {
                if (part.empty() || part == ".")
                {
                    continue;
                }

                if (part == "..")
                {
                    if (!parts.empty())
                    {
                        parts.pop_back();
                    }
                    continue;
                }

                parts.push_back(part);
            }

            return joinMcPathComponents(parts);
        }

        std::filesystem::path guestMcPathToHostPath(int32_t port, const std::string &guestPath)
        {
            std::filesystem::path resolved = getMcRootPath(port);
            if (guestPath.size() > 1u)
            {
                resolved /= std::filesystem::path(guestPath.substr(1));
            }
            return resolved.lexically_normal();
        }

        std::time_t fileTimeToTimeTMc(std::filesystem::file_time_type value)
        {
            if (ps2x::det::enabled())
                return ps2x::det::epoch(); // host file times differ between runs
            const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                value - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            return std::chrono::system_clock::to_time_t(systemTime);
        }

        void writeMcCString(uint8_t *rdram, uint32_t addr, const std::string &value)
        {
            if (addr == 0u)
            {
                return;
            }

            uint8_t *dst = getMemPtr(rdram, addr);
            if (!dst)
            {
                return;
            }

            std::memcpy(dst, value.c_str(), value.size() + 1u);
        }

        void writeMcDateTime(SceMcStDateTime &out, std::time_t value)
        {
            std::tm tm{};
            if (!ps2x::det::calendar(&value, &tm))
            {
                std::memset(&out, 0, sizeof(out));
                return;
            }

            out.Resv2 = 0;
            out.Sec = static_cast<uint8_t>(tm.tm_sec);
            out.Min = static_cast<uint8_t>(tm.tm_min);
            out.Hour = static_cast<uint8_t>(tm.tm_hour);
            out.Day = static_cast<uint8_t>(tm.tm_mday);
            out.Month = static_cast<uint8_t>(tm.tm_mon + 1);
            out.Year = static_cast<uint16_t>(tm.tm_year + 1900);
        }

        void fillMcDirTableEntry(SceMcTblGetDir &entry,
                                 const std::string &name,
                                 bool isDirectory,
                                 uint32_t sizeBytes,
                                 std::time_t modifiedTime)
        {
            std::memset(&entry, 0, sizeof(entry));
            writeMcDateTime(entry._Create, modifiedTime);
            writeMcDateTime(entry._Modify, modifiedTime);
            entry.FileSizeByte = isDirectory ? 0u : sizeBytes;
            entry.AttrFile = static_cast<uint16_t>(kMcAttrReadable |
                                                   kMcAttrWriteable |
                                                   (isDirectory ? kMcAttrSubdir : kMcAttrFile) |
                                                   kMcAttrClosed |
                                                   kMcAttrExists);
            std::strncpy(entry.EntryName, name.c_str(), sizeof(entry.EntryName) - 1u);
            entry.EntryName[sizeof(entry.EntryName) - 1u] = '\0';
        }

        bool wildcardMatch(const std::string &pattern, const std::string &value)
        {
            size_t patternPos = 0u;
            size_t valuePos = 0u;
            size_t starPos = std::string::npos;
            size_t matchPos = 0u;

            while (valuePos < value.size())
            {
                if (patternPos < pattern.size() &&
                    (pattern[patternPos] == '?' || pattern[patternPos] == value[valuePos]))
                {
                    ++patternPos;
                    ++valuePos;
                }
                else if (patternPos < pattern.size() && pattern[patternPos] == '*')
                {
                    starPos = patternPos++;
                    matchPos = valuePos;
                }
                else if (starPos != std::string::npos)
                {
                    patternPos = starPos + 1u;
                    valuePos = ++matchPos;
                }
                else
                {
                    return false;
                }
            }

            while (patternPos < pattern.size() && pattern[patternPos] == '*')
            {
                ++patternPos;
            }

            return patternPos == pattern.size();
        }

        void setMcCommandResultLocked(int32_t cmd, int32_t result)
        {
            g_mcLastCmd = cmd;
            g_mcLastResult = result;
            g_mcCommandPending = true;
        }

        void closeMcFilesLocked()
        {
            for (auto &[fd, openFile] : g_mcFiles)
            {
                if (openFile.file)
                {
                    std::fclose(openFile.file);
                    openFile.file = nullptr;
                }
            }
            g_mcFiles.clear();
        }

        void closeMcFilesForPortLocked(int32_t port)
        {
            for (auto it = g_mcFiles.begin(); it != g_mcFiles.end();)
            {
                if (it->second.port == port)
                {
                    if (it->second.file)
                    {
                        std::fclose(it->second.file);
                    }
                    it = g_mcFiles.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        int32_t allocateMcFdLocked(FILE *file, int32_t port, const std::filesystem::path &hostPath)
        {
            if (!file)
            {
                return kMcResultDeniedPermit;
            }
            if (g_mcFiles.size() >= kMcMaxOpenFiles)
            {
                return kMcResultUpLimitHandle;
            }

            for (int attempt = 0; attempt < 0x10000; ++attempt)
            {
                if (g_mcNextFd <= 0)
                {
                    g_mcNextFd = 1;
                }

                const int32_t fd = g_mcNextFd++;
                if (g_mcFiles.find(fd) != g_mcFiles.end())
                {
                    continue;
                }

                g_mcFiles.emplace(fd, McOpenFile{file, port, hostPath});
                return fd;
            }

            return kMcResultUpLimitHandle;
        }

        FILE *openMcHostFile(const std::filesystem::path &hostPath, uint32_t flags)
        {
            const uint32_t access = flags & PS2_FIO_O_RDWR;
            const bool read = (access == PS2_FIO_O_RDONLY) || (access == PS2_FIO_O_RDWR);
            const bool write = (access == PS2_FIO_O_WRONLY) || (access == PS2_FIO_O_RDWR);
            const bool append = (flags & PS2_FIO_O_APPEND) != 0u;
            const bool create = (flags & PS2_FIO_O_CREAT) != 0u;
            const bool truncate = (flags & PS2_FIO_O_TRUNC) != 0u;

            std::error_code ec;
            const bool exists = std::filesystem::exists(hostPath, ec) && !ec;

            const char *mode = "rb";
            if (read && write)
            {
                if (append)
                {
                    mode = exists ? "a+b" : "w+b";
                }
                else if (truncate || (create && !exists))
                {
                    mode = "w+b";
                }
                else
                {
                    mode = "r+b";
                }
            }
            else if (write)
            {
                if (append)
                {
                    mode = exists ? "ab" : "wb";
                }
                else if (truncate || (create && !exists))
                {
                    mode = "wb";
                }
                else
                {
                    mode = "r+b";
                }
            }

            return std::fopen(hostPath.string().c_str(), mode);
        }
    }

    MemoryCardDebugSnapshot getMemoryCardDebugSnapshot()
    {
        MemoryCardDebugSnapshot snapshot{};
        std::lock_guard<std::mutex> lock(g_mcStateMutex);
        snapshot.nextFd = g_mcNextFd;
        snapshot.lastCmd = g_mcLastCmd;
        snapshot.lastResult = g_mcLastResult;
        snapshot.cvFileCursor = g_cvMcFileCursor;

        for (size_t i = 0; i < g_mcPorts.size(); ++i)
        {
            snapshot.ports[i].port = static_cast<int32_t>(i);
            snapshot.ports[i].currentDir = g_mcPorts[i].currentDir;
            snapshot.ports[i].formatted = g_mcPorts[i].formatted;
            snapshot.ports[i].rootPath = getMcRootPath(static_cast<int32_t>(i));
        }

        snapshot.openFiles.reserve(g_mcFiles.size());
        for (const auto &[fd, file] : g_mcFiles)
        {
            MemoryCardDebugOpenFile row{};
            row.fd = fd;
            row.port = file.port;
            row.hostPath = file.hostPath;
            snapshot.openFiles.push_back(std::move(row));
        }
        std::sort(snapshot.openFiles.begin(), snapshot.openFiles.end(), [](const MemoryCardDebugOpenFile &a, const MemoryCardDebugOpenFile &b)
                  { return a.fd < b.fd; });
        return snapshot;
    }

    // int sceMcChangeThreadPriority(int level)
    PS2_STUB_TYPED(int32_t, sceMcChangeThreadPriority, int32_t level)
    {
        (void)level;
        return 0;
    }

    void sceMcChangeThreadPriority_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceMcChangeThreadPriority)

    // The body of sceMcChdir after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sceMcChdirBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7)
    {
        (void)ctx;
        (void)runtime;
        const int32_t port = static_cast<int32_t>(r4);
        const int32_t slot = static_cast<int32_t>(r5);
        const uint32_t pathAddr = r6;
        const uint32_t currentDirAddr = r7;
        const std::string requestedDir =
            (pathAddr != 0u) ? readPs2CStringBounded(rdram, pathAddr, kMcMaxPathLen) : std::string{};

        std::string currentDir = "/";
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                currentDir = state.currentDir;
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    ensureMcRootExists(port);
                    const std::string resolvedDir =
                        requestedDir.empty() ? state.currentDir : normalizeGuestMcPathLocked(port, requestedDir);
                    const std::filesystem::path hostDir = guestMcPathToHostPath(port, resolvedDir);
                    std::error_code ec;
                    if (std::filesystem::exists(hostDir, ec) && !ec &&
                        std::filesystem::is_directory(hostDir, ec))
                    {
                        state.currentDir = resolvedDir;
                        currentDir = resolvedDir;
                        result = kMcResultSucceed;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdChdir, result);
        }

        RUNTIME_LOG("[MC] Chdir port=" << port << " '" << requestedDir
                                       << "' -> result=" << result << " cwd='" << currentDir << "'");
        writeMcCString(rdram, currentDirAddr, currentDir);
        return 0;
    }

    // int sceMcChdir(int port, int slot, const char *newDir, char *currentDir)
    PS2_STUB_TYPED(int32_t, sceMcChdir, int32_t port, int32_t slot, ps2_abi::gptr newDir, ps2_abi::gptr currentDir)
    {
        return sceMcChdirBody(rdram, ctx, runtime, static_cast<uint32_t>(port), static_cast<uint32_t>(slot), static_cast<uint32_t>(newDir), static_cast<uint32_t>(currentDir));
    }

    void sceMcChdir_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, sceMcChdirBody(rdram, ctx, runtime, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7)));
    }

    PS2_STUB_DISPATCH(sceMcChdir)

    // The body of sceMcClose after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sceMcCloseBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t r4)
    {
        (void)ctx;
        (void)runtime;
        const int32_t fd = static_cast<int32_t>(r4);
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (it != g_mcFiles.end())
            {
                if (!it->second.file || std::fclose(it->second.file) == 0)
                {
                    result = kMcResultSucceed;
                }
                g_mcFiles.erase(it);
            }
            setMcCommandResultLocked(kMcCmdClose, result);
        }
        return 0;
    }

    // int sceMcClose(int fd)
    PS2_STUB_TYPED(int32_t, sceMcClose, int32_t fd)
    {
        return sceMcCloseBody(rdram, ctx, runtime, static_cast<uint32_t>(fd));
    }

    void sceMcClose_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, sceMcCloseBody(rdram, ctx, runtime, getRegU32(ctx, 4)));
    }

    PS2_STUB_DISPATCH(sceMcClose)

    // The body of sceMcDelete after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sceMcDeleteBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t r4, uint32_t r5, uint32_t r6)
    {
        (void)ctx;
        (void)runtime;
        const int32_t port = static_cast<int32_t>(r4);
        const int32_t slot = static_cast<int32_t>(r5);
        const std::string path = readPs2CStringBounded(rdram, r6, kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::string guestPath = normalizeGuestMcPathLocked(port, path);
                    if (guestPath != "/")
                    {
                        const std::filesystem::path hostPath = guestMcPathToHostPath(port, guestPath);
                        std::error_code ec;
                        if (std::filesystem::exists(hostPath, ec) && !ec)
                        {
                            if (std::filesystem::is_directory(hostPath, ec) &&
                                !std::filesystem::is_empty(hostPath, ec))
                            {
                                result = kMcResultNotEmpty;
                            }
                            else if (std::filesystem::remove(hostPath, ec) && !ec)
                            {
                                result = kMcResultSucceed;
                            }
                            else
                            {
                                result = kMcResultDeniedPermit;
                            }
                        }
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdDelete, result);
        }
        return 0;
    }

    // int sceMcDelete(int port, int slot, const char *name)
    PS2_STUB_TYPED(int32_t, sceMcDelete, int32_t port, int32_t slot, ps2_abi::gptr name)
    {
        return sceMcDeleteBody(rdram, ctx, runtime, static_cast<uint32_t>(port), static_cast<uint32_t>(slot), static_cast<uint32_t>(name));
    }

    void sceMcDelete_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, sceMcDeleteBody(rdram, ctx, runtime, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6)));
    }

    PS2_STUB_DISPATCH(sceMcDelete)

    // int sceMcEnd(void)
    PS2_STUB0_TYPED(int32_t, sceMcEnd)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            closeMcFilesLocked();
            g_mcNextFd = 1;
            g_mcLastCmd = 0;
            g_mcLastResult = 0;
            g_mcCommandPending = false;
            for (McPortState &state : g_mcPorts)
            {
                state.currentDir = "/";
            }
        }

        // libmc teardown is just a local state reset in this immediate runtime model.
        return 0;
    }

    void sceMcEnd_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            closeMcFilesLocked();
            g_mcNextFd = 1;
            g_mcLastCmd = 0;
            g_mcLastResult = 0;
            g_mcCommandPending = false;
            for (McPortState &state : g_mcPorts)
            {
                state.currentDir = "/";
            }
        }

        // libmc teardown is just a local state reset in this immediate runtime model.
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceMcEnd)

    // The body of sceMcFlush after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sceMcFlushBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t r4)
    {
        (void)ctx;
        (void)runtime;
        const int32_t fd = static_cast<int32_t>(r4);
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (it != g_mcFiles.end() && it->second.file)
            {
                result = (std::fflush(it->second.file) == 0) ? kMcResultSucceed : kMcResultDeniedPermit;
            }
            setMcCommandResultLocked(kMcCmdFlush, result);
        }
        return 0;
    }

    // int sceMcFlush(int fd)
    PS2_STUB_TYPED(int32_t, sceMcFlush, int32_t fd)
    {
        return sceMcFlushBody(rdram, ctx, runtime, static_cast<uint32_t>(fd));
    }

    void sceMcFlush_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, sceMcFlushBody(rdram, ctx, runtime, getRegU32(ctx, 4)));
    }

    PS2_STUB_DISPATCH(sceMcFlush)

    // The body of sceMcFormat after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sceMcFormatBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t r4, uint32_t r5)
    {
        (void)ctx;
        (void)runtime;
        const int32_t port = static_cast<int32_t>(r4);
        const int32_t slot = static_cast<int32_t>(r5);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                closeMcFilesForPortLocked(port);
                const std::filesystem::path root = getMcRootPath(port);
                std::error_code ec;
                std::filesystem::remove_all(root, ec);
                ec.clear();
                std::filesystem::create_directories(root, ec);
                if (!ec)
                {
                    McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                    state.currentDir = "/";
                    state.formatted = true;
                    result = kMcResultSucceed;
                }
            }

            setMcCommandResultLocked(kMcCmdFormat, result);
        }
        return 0;
    }

    // int sceMcFormat(int port, int slot)
    PS2_STUB_TYPED(int32_t, sceMcFormat, int32_t port, int32_t slot)
    {
        return sceMcFormatBody(rdram, ctx, runtime, static_cast<uint32_t>(port), static_cast<uint32_t>(slot));
    }

    void sceMcFormat_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, sceMcFormatBody(rdram, ctx, runtime, getRegU32(ctx, 4), getRegU32(ctx, 5)));
    }

    PS2_STUB_DISPATCH(sceMcFormat)

    // The body of sceMcGetDir after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sceMcGetDirBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r8, uint32_t r9)
    {
        (void)ctx;
        (void)runtime;
        const int32_t port = static_cast<int32_t>(r4);
        const int32_t slot = static_cast<int32_t>(r5);
        const std::string rawPath = readPs2CStringBounded(rdram, r6, kMcMaxPathLen);
        const int32_t maxEntries = static_cast<int32_t>(r8);
        const uint32_t tableAddr = r9;

        std::vector<SceMcTblGetDir> entries;
        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    ensureMcRootExists(port);
                    const std::string guestQuery =
                        normalizeGuestMcPathLocked(port, rawPath.empty() ? "." : rawPath);
                    const bool hasWildcard =
                        guestQuery.find('*') != std::string::npos || guestQuery.find('?') != std::string::npos;

                    const std::filesystem::path queryRel =
                        (guestQuery.size() > 1u) ? std::filesystem::path(guestQuery.substr(1)) : std::filesystem::path{};

                    std::filesystem::path parentRel;
                    std::string pattern;
                    if (hasWildcard)
                    {
                        parentRel = queryRel.parent_path();
                        pattern = queryRel.filename().string();
                    }
                    else
                    {
                        const std::filesystem::path queryHostPath = guestMcPathToHostPath(port, guestQuery);
                        std::error_code queryEc;
                        if (std::filesystem::exists(queryHostPath, queryEc) && !queryEc &&
                            std::filesystem::is_directory(queryHostPath, queryEc))
                        {
                            parentRel = queryRel;
                            pattern = "*";
                        }
                        else
                        {
                            parentRel = queryRel.parent_path();
                            pattern = queryRel.filename().string();
                        }
                    }

                    if (pattern.empty())
                    {
                        pattern = "*";
                    }

                    std::filesystem::path hostDir = getMcRootPath(port);
                    if (!parentRel.empty())
                    {
                        hostDir /= parentRel;
                    }
                    hostDir = hostDir.lexically_normal();

                    std::error_code ec;
                    if (std::filesystem::exists(hostDir, ec) && !ec &&
                        std::filesystem::is_directory(hostDir, ec))
                    {
                        const std::time_t now = ps2x::det::now();
                        auto appendSpecial = [&](const std::string &name)
                        {
                            if (!wildcardMatch(pattern, name))
                            {
                                return;
                            }
                            SceMcTblGetDir entry{};
                            fillMcDirTableEntry(entry, name, true, 0u, now);
                            entries.push_back(entry);
                        };

                        appendSpecial(".");
                        appendSpecial("..");

                        std::vector<std::filesystem::directory_entry> dirEntries;
                        for (const auto &entry : std::filesystem::directory_iterator(
                                 hostDir, std::filesystem::directory_options::skip_permission_denied, ec))
                        {
                            if (ec)
                            {
                                break;
                            }
                            dirEntries.push_back(entry);
                        }

                        std::sort(dirEntries.begin(), dirEntries.end(),
                                  [](const std::filesystem::directory_entry &lhs,
                                     const std::filesystem::directory_entry &rhs)
                                  {
                                      return toLowerAscii(lhs.path().filename().string()) <
                                             toLowerAscii(rhs.path().filename().string());
                                  });

                        for (const auto &entry : dirEntries)
                        {
                            const std::string name = entry.path().filename().string();
                            if (!wildcardMatch(pattern, name))
                            {
                                continue;
                            }

                            std::error_code entryEc;
                            const bool isDirectory = entry.is_directory(entryEc) && !entryEc;
                            const uint32_t sizeBytes =
                                isDirectory ? 0u : static_cast<uint32_t>(entry.file_size(entryEc));
                            entryEc.clear();
                            const std::time_t modifiedTime = fileTimeToTimeTMc(entry.last_write_time(entryEc));
                            SceMcTblGetDir tableEntry{};
                            fillMcDirTableEntry(tableEntry,
                                                name,
                                                isDirectory,
                                                sizeBytes,
                                                entryEc ? now : modifiedTime);
                            entries.push_back(tableEntry);
                        }

                        const size_t entryCount =
                            std::min(entries.size(), maxEntries > 0 ? static_cast<size_t>(maxEntries) : 0u);
                        if (entryCount == 0u || tableAddr == 0u)
                        {
                            result = static_cast<int32_t>(entryCount);
                        }
                        else if (uint8_t *dst = getMemPtr(rdram, tableAddr))
                        {
                            std::memcpy(dst, entries.data(), entryCount * sizeof(SceMcTblGetDir));
                            result = static_cast<int32_t>(entryCount);
                        }
                        else
                        {
                            result = kMcResultDeniedPermit;
                        }
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdGetDir, result);
        }
        RUNTIME_LOG("[MC] GetDir port=" << port << " '" << rawPath
                                        << "' maxent=" << maxEntries << " -> result=" << result);
        return 0;
    }

    // int sceMcGetDir(int port, int slot, const char *name, unsigned mode, int maxent, sceMcTblGetDir *table)
    PS2_STUB_TYPED(int32_t, sceMcGetDir, int32_t port, int32_t slot, ps2_abi::gptr name, uint32_t mode, int32_t maxent, ps2_abi::gptr table)
    {
        (void)mode;
        return sceMcGetDirBody(rdram, ctx, runtime, static_cast<uint32_t>(port), static_cast<uint32_t>(slot), static_cast<uint32_t>(name), static_cast<uint32_t>(maxent), static_cast<uint32_t>(table));
    }

    void sceMcGetDir_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, sceMcGetDirBody(rdram, ctx, runtime, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 8), getRegU32(ctx, 9)));
    }

    PS2_STUB_DISPATCH(sceMcGetDir)

    // int sceMcGetEntSpace(int port, int slot, const char *path)
    PS2_STUB_TYPED(int32_t, sceMcGetEntSpace, int32_t port, int32_t slot, ps2_abi::gptr path)
    {
        (void)port;
        (void)slot;
        (void)path;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            setMcCommandResultLocked(kMcCmdGetEntSpace, 1024);
        }
        return 0;
    }

    void sceMcGetEntSpace_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            setMcCommandResultLocked(kMcCmdGetEntSpace, 1024);
        }
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceMcGetEntSpace)

    // The body of sceMcGetInfo after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sceMcGetInfoBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8)
    {
        (void)ctx;
        (void)runtime;
        const int32_t port = static_cast<int32_t>(r4);
        const int32_t slot = static_cast<int32_t>(r5);
        const uint32_t typePtr = r6;
        const uint32_t freePtr = r7;
        const uint32_t formatPtr = r8;

        int32_t cardType = 0;
        int32_t freeBlocks = 0;
        int32_t format = kMcUnformatted;
        int32_t result = kMcResultNoEntry;

        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                cardType = kMcTypePs2;
                freeBlocks = state.formatted ? kMcFreeClusters : 0;
                format = state.formatted ? kMcFormatted : kMcUnformatted;
                result = state.formatted ? kMcResultSucceed : kMcResultNoFormat;
            }

            setMcCommandResultLocked(kMcCmdGetInfo, result);
        }

        if (typePtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, typePtr))
            {
                std::memcpy(out, &cardType, sizeof(cardType));
            }
        }
        if (freePtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, freePtr))
            {
                std::memcpy(out, &freeBlocks, sizeof(freeBlocks));
            }
        }
        if (formatPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, formatPtr))
            {
                std::memcpy(out, &format, sizeof(format));
            }
        }

        RUNTIME_LOG("[MC] GetInfo port=" << port << " type=" << cardType
                                         << " free=" << freeBlocks << " format=" << format
                                         << " result=" << result);
        return 0;
    }

    // int sceMcGetInfo(int port, int slot, int *type, int *free, int *format)
    PS2_STUB_TYPED(int32_t, sceMcGetInfo, int32_t port, int32_t slot, ps2_abi::gptr type, ps2_abi::gptr freeBlocks, ps2_abi::gptr format)
    {
        return sceMcGetInfoBody(rdram, ctx, runtime, static_cast<uint32_t>(port), static_cast<uint32_t>(slot), static_cast<uint32_t>(type), static_cast<uint32_t>(freeBlocks), static_cast<uint32_t>(format));
    }

    void sceMcGetInfo_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, sceMcGetInfoBody(rdram, ctx, runtime, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7), getRegU32(ctx, 8)));
    }

    PS2_STUB_DISPATCH(sceMcGetInfo)

    // int sceMcGetSlotMax(int port)
    PS2_STUB_TYPED(int32_t, sceMcGetSlotMax, int32_t port)
    {
        (void)port;
        return 1;
    }

    void sceMcGetSlotMax_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    PS2_STUB_DISPATCH(sceMcGetSlotMax)

    // int sceMcInit(void)
    PS2_STUB0_TYPED(int32_t, sceMcInit)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            closeMcFilesLocked();
            g_mcNextFd = 1;
            g_mcLastCmd = 0;
            g_mcLastResult = 0;
            g_mcCommandPending = false;
            for (McPortState &state : g_mcPorts)
            {
                state.currentDir = "/";
                state.formatted = true;
            }
        }
        ensureMcRootExists(0);
        ensureMcRootExists(1);
        return 0;
    }

    void sceMcInit_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            closeMcFilesLocked();
            g_mcNextFd = 1;
            g_mcLastCmd = 0;
            g_mcLastResult = 0;
            g_mcCommandPending = false;
            for (McPortState &state : g_mcPorts)
            {
                state.currentDir = "/";
                state.formatted = true;
            }
        }
        ensureMcRootExists(0);
        ensureMcRootExists(1);
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceMcInit)

    // The body of sceMcMkdir after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sceMcMkdirBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t r4, uint32_t r5, uint32_t r6)
    {
        (void)ctx;
        (void)runtime;
        const int32_t port = static_cast<int32_t>(r4);
        const int32_t slot = static_cast<int32_t>(r5);
        const std::string path = readPs2CStringBounded(rdram, r6, kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    ensureMcRootExists(port);
                    const std::string guestPath = normalizeGuestMcPathLocked(port, path);
                    const std::filesystem::path hostPath = guestMcPathToHostPath(port, guestPath);
                    std::error_code ec;
                    if (std::filesystem::exists(hostPath, ec) && !ec)
                    {
                        result = std::filesystem::is_directory(hostPath, ec) ? kMcResultSucceed : kMcResultDeniedPermit;
                    }
                    else if (std::filesystem::create_directory(hostPath, ec) && !ec)
                    {
                        result = kMcResultSucceed;
                    }
                    else
                    {
                        result = std::filesystem::exists(hostPath.parent_path(), ec) && !ec
                                     ? kMcResultDeniedPermit
                                     : kMcResultNoEntry;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdMkdir, result);
        }
        return 0;
    }

    // int sceMcMkdir(int port, int slot, const char *name)
    PS2_STUB_TYPED(int32_t, sceMcMkdir, int32_t port, int32_t slot, ps2_abi::gptr name)
    {
        return sceMcMkdirBody(rdram, ctx, runtime, static_cast<uint32_t>(port), static_cast<uint32_t>(slot), static_cast<uint32_t>(name));
    }

    void sceMcMkdir_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, sceMcMkdirBody(rdram, ctx, runtime, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6)));
    }

    PS2_STUB_DISPATCH(sceMcMkdir)

    // The body of sceMcOpen after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sceMcOpenBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7)
    {
        (void)ctx;
        (void)runtime;
        const int32_t port = static_cast<int32_t>(r4);
        const int32_t slot = static_cast<int32_t>(r5);
        const std::string path = readPs2CStringBounded(rdram, r6, kMcMaxPathLen);
        const uint32_t flags = r7;

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::string guestPath = normalizeGuestMcPathLocked(port, path);
                    const std::filesystem::path hostPath = guestMcPathToHostPath(port, guestPath);
                    std::error_code ec;
                    const bool create = (flags & PS2_FIO_O_CREAT) != 0u;
                    const bool exists = std::filesystem::exists(hostPath, ec) && !ec;
                    if (guestPath == "/")
                    {
                        result = kMcResultDeniedPermit;
                    }
                    else if (exists && std::filesystem::is_directory(hostPath, ec))
                    {
                        result = kMcResultDeniedPermit;
                    }
                    else if (!exists && !create)
                    {
                        result = kMcResultNoEntry;
                    }
                    else if (!std::filesystem::exists(hostPath.parent_path(), ec) || ec)
                    {
                        result = kMcResultNoEntry;
                    }
                    else
                    {
                        FILE *file = openMcHostFile(hostPath, flags);
                        if (!file)
                        {
                            result = exists ? kMcResultDeniedPermit : kMcResultNoEntry;
                        }
                        else
                        {
                            result = allocateMcFdLocked(file, port, hostPath);
                            if (result < 0)
                            {
                                std::fclose(file);
                            }
                        }
                    }
                }
            }
            setMcCommandResultLocked(kMcCmdOpen, result);
        }
        return 0;
    }

    // int sceMcOpen(int port, int slot, const char *name, int mode)
    PS2_STUB_TYPED(int32_t, sceMcOpen, int32_t port, int32_t slot, ps2_abi::gptr name, int32_t mode)
    {
        return sceMcOpenBody(rdram, ctx, runtime, static_cast<uint32_t>(port), static_cast<uint32_t>(slot), static_cast<uint32_t>(name), static_cast<uint32_t>(mode));
    }

    void sceMcOpen_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, sceMcOpenBody(rdram, ctx, runtime, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7)));
    }

    PS2_STUB_DISPATCH(sceMcOpen)

    namespace
    {
        // Shared body of sceMcRead and sceMcReadFast: (fd, buf, size) in a0..a2,
        // the byte count (or a negative libmc error) reported through sceMcSync.
        // After decoding: (fd, buf, size); the result goes to sceMcSync, v0 is 0. Shared by the typed
        // and the legacy form (mcReadCommon decodes from the registers, as before).
        void mcReadBody(uint8_t *rdram, int32_t cmd, int32_t fd, uint32_t dstAddr, int32_t size)
        {
            uint8_t *dst = (size > 0) ? getMemPtr(rdram, dstAddr) : nullptr;

            int32_t result = kMcResultNoEntry;
            {
                std::lock_guard<std::mutex> lock(g_mcStateMutex);
                auto it = g_mcFiles.find(fd);
                if (size <= 0)
                {
                    result = 0;
                }
                else if (it == g_mcFiles.end() || !it->second.file)
                {
                    result = kMcResultNoEntry;
                }
                else if (!dst)
                {
                    result = kMcResultDeniedPermit;
                }
                else
                {
                    const size_t bytesRead = std::fread(dst, 1u, static_cast<size_t>(size), it->second.file);
                    result = std::ferror(it->second.file) ? kMcResultDeniedPermit : static_cast<int32_t>(bytesRead);
                    if (std::ferror(it->second.file))
                    {
                        std::clearerr(it->second.file);
                    }
                }

                setMcCommandResultLocked(cmd, result);
            }
        }

        void mcReadCommon(uint8_t *rdram, R5900Context *ctx, int32_t cmd)
        {
            const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
            const uint32_t dstAddr = getRegU32(ctx, 5);
            const int32_t size = static_cast<int32_t>(getRegU32(ctx, 6));
            mcReadBody(rdram, cmd, fd, dstAddr, size);
            setReturnS32(ctx, 0);
        }
    }

    // int sceMcRead(int fd, void *buff, int size)
    PS2_STUB_TYPED(int32_t, sceMcRead, int32_t fd, ps2_abi::gptr buff, int32_t size)
    {
        (void)runtime;
        mcReadBody(rdram, kMcCmdRead, fd, buff, size);
        return 0;
    }

    void sceMcRead_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        mcReadCommon(rdram, ctx, kMcCmdRead);
    }

    PS2_STUB_DISPATCH(sceMcRead)

    // int sceMcReadFast(int fd, void *buf, int size): the IOP DMAs straight into
    // buf (the SDK copy bounces misaligned head/tail bytes through its own buffer);
    // for the host the result is the same as sceMcRead.
    // int sceMcReadFast(int fd, void *buff, int size)
    PS2_STUB_TYPED(int32_t, sceMcReadFast, int32_t fd, ps2_abi::gptr buff, int32_t size)
    {
        (void)runtime;
        mcReadBody(rdram, kMcCmdReadFast, fd, buff, size);
        return 0;
    }

    void sceMcReadFast_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        mcReadCommon(rdram, ctx, kMcCmdReadFast);
    }

    PS2_STUB_DISPATCH(sceMcReadFast)

    // The body of sceMcRename after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sceMcRenameBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7)
    {
        (void)ctx;
        (void)runtime;
        const int32_t port = static_cast<int32_t>(r4);
        const int32_t slot = static_cast<int32_t>(r5);
        const std::string oldPath = readPs2CStringBounded(rdram, r6, kMcMaxPathLen);
        const std::string newPath = readPs2CStringBounded(rdram, r7, kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::filesystem::path oldHostPath =
                        guestMcPathToHostPath(port, normalizeGuestMcPathLocked(port, oldPath));
                    const std::filesystem::path newHostPath =
                        guestMcPathToHostPath(port, normalizeGuestMcPathLocked(port, newPath));
                    std::error_code ec;
                    if (std::filesystem::exists(oldHostPath, ec) && !ec &&
                        std::filesystem::exists(newHostPath.parent_path(), ec) && !ec)
                    {
                        std::filesystem::rename(oldHostPath, newHostPath, ec);
                        result = ec ? kMcResultDeniedPermit : kMcResultSucceed;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdRename, result);
        }
        return 0;
    }

    // int sceMcRename(int port, int slot, const char *org, const char *newName)
    PS2_STUB_TYPED(int32_t, sceMcRename, int32_t port, int32_t slot, ps2_abi::gptr org, ps2_abi::gptr newName)
    {
        return sceMcRenameBody(rdram, ctx, runtime, static_cast<uint32_t>(port), static_cast<uint32_t>(slot), static_cast<uint32_t>(org), static_cast<uint32_t>(newName));
    }

    void sceMcRename_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, sceMcRenameBody(rdram, ctx, runtime, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7)));
    }

    PS2_STUB_DISPATCH(sceMcRename)

    // The body of sceMcSeek after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sceMcSeekBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t r4, uint32_t r5, uint32_t r6)
    {
        (void)ctx;
        (void)runtime;
        const int32_t fd = static_cast<int32_t>(r4);
        const int32_t offset = static_cast<int32_t>(r5);
        const int32_t origin = static_cast<int32_t>(r6);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            auto it = g_mcFiles.find(fd);
            if (it != g_mcFiles.end() && it->second.file)
            {
                int whence = SEEK_SET;
                if (origin == PS2_FIO_SEEK_CUR)
                {
                    whence = SEEK_CUR;
                }
                else if (origin == PS2_FIO_SEEK_END)
                {
                    whence = SEEK_END;
                }

                if (std::fseek(it->second.file, offset, whence) == 0)
                {
                    const long position = std::ftell(it->second.file);
                    result = (position >= 0) ? static_cast<int32_t>(position) : kMcResultDeniedPermit;
                }
                else
                {
                    result = kMcResultDeniedPermit;
                }
            }

            setMcCommandResultLocked(kMcCmdSeek, result);
        }
        return 0;
    }

    // int sceMcSeek(int fd, int offset, int mode)
    PS2_STUB_TYPED(int32_t, sceMcSeek, int32_t fd, int32_t offset, int32_t origin)
    {
        return sceMcSeekBody(rdram, ctx, runtime, static_cast<uint32_t>(fd), static_cast<uint32_t>(offset), static_cast<uint32_t>(origin));
    }

    void sceMcSeek_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, sceMcSeekBody(rdram, ctx, runtime, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6)));
    }

    PS2_STUB_DISPATCH(sceMcSeek)

    // The body of sceMcSetFileInfo after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sceMcSetFileInfoBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t r4, uint32_t r5, uint32_t r6)
    {
        (void)ctx;
        (void)runtime;
        const int32_t port = static_cast<int32_t>(r4);
        const int32_t slot = static_cast<int32_t>(r5);
        const std::string path = readPs2CStringBounded(rdram, r6, kMcMaxPathLen);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                if (!state.formatted)
                {
                    result = kMcResultNoFormat;
                }
                else
                {
                    const std::filesystem::path hostPath =
                        guestMcPathToHostPath(port, normalizeGuestMcPathLocked(port, path));
                    std::error_code ec;
                    if (std::filesystem::exists(hostPath, ec) && !ec)
                    {
                        result = kMcResultSucceed;
                    }
                }
            }

            setMcCommandResultLocked(kMcCmdSetFileInfo, result);
        }
        return 0;
    }

    // int sceMcSetFileInfo(int port, int slot, const char *name, const char *info, unsigned valid)
    PS2_STUB_TYPED(int32_t, sceMcSetFileInfo, int32_t port, int32_t slot, ps2_abi::gptr name, ps2_abi::gptr info, uint32_t valid)
    {
        (void)info;
        (void)valid;
        return sceMcSetFileInfoBody(rdram, ctx, runtime, static_cast<uint32_t>(port), static_cast<uint32_t>(slot), static_cast<uint32_t>(name));
    }

    void sceMcSetFileInfo_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, sceMcSetFileInfoBody(rdram, ctx, runtime, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6)));
    }

    PS2_STUB_DISPATCH(sceMcSetFileInfo)

    // The body of sceMcSync after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sceMcSyncBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t r5, uint32_t r6)
    {
        (void)ctx;
        (void)runtime;
        const uint32_t cmdPtr = r5;
        const uint32_t resultPtr = r6;
        int32_t cmd = 0;
        int32_t result = 0;
        bool hadPending = false;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            hadPending = g_mcCommandPending;
            g_mcCommandPending = false;
            cmd = g_mcLastCmd;
            result = g_mcLastResult;
        }

        // libmc semantics: -1 means no async operation was executing; games rely
        // on it to tell idle polling apart from command completion.
        if (!hadPending)
        {
            return -1;
        }

        RUNTIME_LOG("[MC] Sync cmd=" << cmd << " result=" << result);

        if (cmdPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, cmdPtr))
            {
                std::memcpy(out, &cmd, sizeof(cmd));
            }
        }
        if (resultPtr != 0u)
        {
            if (uint8_t *out = getMemPtr(rdram, resultPtr))
            {
                std::memcpy(out, &result, sizeof(result));
            }
        }

        // 1 = command finished in this runtime's immediate model.
        return 1;
    }

    // int sceMcSync(int mode, int *cmd, int *result)
    PS2_STUB_TYPED(int32_t, sceMcSync, int32_t mode, ps2_abi::gptr cmd, ps2_abi::gptr result)
    {
        (void)mode;
        return sceMcSyncBody(rdram, ctx, runtime, static_cast<uint32_t>(cmd), static_cast<uint32_t>(result));
    }

    void sceMcSync_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, sceMcSyncBody(rdram, ctx, runtime, getRegU32(ctx, 5), getRegU32(ctx, 6)));
    }

    PS2_STUB_DISPATCH(sceMcSync)

    // The body of sceMcUnformat after decoding, shared by the typed and the legacy form; returns v0.
    static int32_t sceMcUnformatBody(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t r4, uint32_t r5)
    {
        (void)ctx;
        (void)runtime;
        const int32_t port = static_cast<int32_t>(r4);
        const int32_t slot = static_cast<int32_t>(r5);

        int32_t result = kMcResultNoEntry;
        {
            std::lock_guard<std::mutex> lock(g_mcStateMutex);
            if (isValidMcPortSlot(port, slot))
            {
                closeMcFilesForPortLocked(port);
                const std::filesystem::path root = getMcRootPath(port);
                std::error_code ec;
                std::filesystem::remove_all(root, ec);
                ec.clear();
                std::filesystem::create_directories(root, ec);
                if (!ec)
                {
                    McPortState &state = g_mcPorts[static_cast<size_t>(port)];
                    state.currentDir = "/";
                    state.formatted = false;
                    result = kMcResultSucceed;
                }
            }

            setMcCommandResultLocked(kMcCmdUnformat, result);
        }
        return 0;
    }

    // int sceMcUnformat(int port, int slot)
    PS2_STUB_TYPED(int32_t, sceMcUnformat, int32_t port, int32_t slot)
    {
        return sceMcUnformatBody(rdram, ctx, runtime, static_cast<uint32_t>(port), static_cast<uint32_t>(slot));
    }

    void sceMcUnformat_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, sceMcUnformatBody(rdram, ctx, runtime, getRegU32(ctx, 4), getRegU32(ctx, 5)));
    }

    PS2_STUB_DISPATCH(sceMcUnformat)

    namespace
    {
        // Shared body of sceMcWrite and sceMcWriteFast.
        // After decoding: (fd, buf, size); the result goes to sceMcSync, v0 is 0. Shared by the typed
        // and the legacy form (mcWriteCommon decodes from the registers, as before).
        void mcWriteBody(uint8_t *rdram, int32_t cmd, int32_t fd, uint32_t srcAddr, int32_t size)
        {
            const uint8_t *src = (size > 0) ? getConstMemPtr(rdram, srcAddr) : nullptr;

            int32_t result = kMcResultNoEntry;
            {
                std::lock_guard<std::mutex> lock(g_mcStateMutex);
                auto it = g_mcFiles.find(fd);
                if (size <= 0)
                {
                    result = 0;
                }
                else if (it == g_mcFiles.end() || !it->second.file)
                {
                    result = kMcResultNoEntry;
                }
                else if (!src)
                {
                    result = kMcResultDeniedPermit;
                }
                else
                {
                    const size_t bytesWritten = std::fwrite(src, 1u, static_cast<size_t>(size), it->second.file);
                    result = std::ferror(it->second.file) ? kMcResultDeniedPermit : static_cast<int32_t>(bytesWritten);
                    if (!std::ferror(it->second.file))
                    {
                        std::fflush(it->second.file);
                    }
                    else
                    {
                        std::clearerr(it->second.file);
                    }
                }

                setMcCommandResultLocked(cmd, result);
            }
        }

        void mcWriteCommon(uint8_t *rdram, R5900Context *ctx, int32_t cmd)
        {
            const int32_t fd = static_cast<int32_t>(getRegU32(ctx, 4));
            const uint32_t srcAddr = getRegU32(ctx, 5);
            const int32_t size = static_cast<int32_t>(getRegU32(ctx, 6));
            mcWriteBody(rdram, cmd, fd, srcAddr, size);
            setReturnS32(ctx, 0);
        }
    }

    // int sceMcWrite(int fd, const void *buff, int size)
    PS2_STUB_TYPED(int32_t, sceMcWrite, int32_t fd, ps2_abi::gptr buff, int32_t size)
    {
        (void)runtime;
        mcWriteBody(rdram, kMcCmdWrite, fd, buff, size);
        return 0;
    }

    void sceMcWrite_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        mcWriteCommon(rdram, ctx, kMcCmdWrite);
    }

    PS2_STUB_DISPATCH(sceMcWrite)

    // int sceMcWriteFast(int fd, const void *buf, int size): same as sceMcWrite
    // for the host (the SDK copy only skips the EE-side staging buffer).
    // int sceMcWriteFast(int fd, const void *buff, int size)
    PS2_STUB_TYPED(int32_t, sceMcWriteFast, int32_t fd, ps2_abi::gptr buff, int32_t size)
    {
        (void)runtime;
        mcWriteBody(rdram, kMcCmdWriteFast, fd, buff, size);
        return 0;
    }

    void sceMcWriteFast_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        mcWriteCommon(rdram, ctx, kMcCmdWriteFast);
    }

    PS2_STUB_DISPATCH(sceMcWriteFast)

    void mcCallMessageTypeSe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcCheckReadStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckReadStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckWriteStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCheckWriteStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateConfigInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateFileSelectWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateIconInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcCreateSaveFileInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcDispFileName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispFileNumber(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplayFileSelectWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplaySelectFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDisplaySelectFileInfoMesCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispWindowCurSol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcDispWindowFoundtion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceGetInfoApdx(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceIntrReadFixAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mceStorePwd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcGetConfigCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcConfigFileBytes);
    }

    void mcGetFileSelectWindowCursol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, g_cvMcFileCursor);
    }

    void mcGetFreeCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcRequiredFreeKb);
    }

    void mcGetIconCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcIconInfoBytes);
    }

    void mcGetIconFileCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcIconFileBytes);
    }

    void mcGetPortSelectDirInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcGetSaveFileCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, kCvMcSaveFileBytes);
    }

    void mcGetStringEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t strAddr = getRegU32(ctx, 4);
        const std::string value = readPs2CStringBounded(rdram, runtime, strAddr, 1024);
        setReturnU32(ctx, strAddr + static_cast<uint32_t>(value.size()));
    }

    void mcMoveFileSelectWindowCursor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t delta = static_cast<int32_t>(getRegU32(ctx, 5));
        g_cvMcFileCursor += delta;
        g_cvMcFileCursor = std::clamp(g_cvMcFileCursor, -1, 15);
        setReturnS32(ctx, 0);
    }

    void mcNewCreateConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcNewCreateIcon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcNewCreateSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadIconData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcReadStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcSelectFileInfoInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = 0;
        setReturnS32(ctx, 1);
    }

    void mcSelectSaveFileCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcSetFileSelectWindowCursol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = static_cast<int32_t>(getRegU32(ctx, 5));
        g_cvMcFileCursor = std::clamp(g_cvMcFileCursor, -1, 15);
        setReturnS32(ctx, 0);
    }

    void mcSetFileSelectWindowCursolInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_cvMcFileCursor = 0;
        setReturnS32(ctx, 0);
    }

    void mcSetStringSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcSetTyepWriteMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void mcWriteIconData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcWriteStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void mcWriteStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }
}

#include "ps2_snapshot_hooks.h"

namespace ps2_stubs::snapshot
{
    void saveMemoryCard(ps2x::StateWriter &w)
    {
        snapshotSaveSupport(w);
        std::lock_guard<std::mutex> lock(g_mcStateMutex);
        w.i32(g_mcNextFd);
        w.i32(g_mcLastCmd);
        w.boolean(g_mcCommandPending);
        w.i32(g_mcLastResult);
        w.sortedMap(g_mcFiles, [&w](const McOpenFile &file)
                    {
                        w.i32(file.port);
                        w.str(file.hostPath.string());
                        w.i64(file.file ? static_cast<int64_t>(std::ftell(file.file)) : -1); });
        for (const McPortState &port : g_mcPorts)
        {
            w.str(port.currentDir);
            w.boolean(port.formatted);
        }
        w.i32(g_cvMcFileCursor);
    }

    void loadMemoryCard(ps2x::StateReader &r, PS2Runtime &runtime)
    {
        (void)runtime;
        snapshotLoadSupport(r);
        std::lock_guard<std::mutex> lock(g_mcStateMutex);
        for (auto &[fd, file] : g_mcFiles)
        {
            if (file.file)
                std::fclose(file.file);
        }
        g_mcFiles.clear();
        g_mcNextFd = r.i32();
        g_mcLastCmd = r.i32();
        g_mcCommandPending = r.boolean();
        g_mcLastResult = r.i32();
        const size_t count = r.size();
        for (size_t i = 0; i < count; ++i)
        {
            const int32_t fd = r.pod<int32_t>();
            McOpenFile file{};
            file.port = r.i32();
            file.hostPath = std::filesystem::path(r.str());
            const int64_t position = r.i64();
            // The open mode is not kept: reopen for update without truncating, read-only
            // if the file is not writable.
            file.file = std::fopen(file.hostPath.string().c_str(), "r+b");
            if (!file.file)
                file.file = std::fopen(file.hostPath.string().c_str(), "rb");
            if (!file.file)
            {
                std::cerr << "[snapshot] memory card fd " << fd << " ('" << file.hostPath.string()
                          << "') could not be reopened" << std::endl;
                continue;
            }
            if (position > 0)
                std::fseek(file.file, static_cast<long>(position), SEEK_SET);
            g_mcFiles.emplace(fd, std::move(file));
        }
        for (McPortState &port : g_mcPorts)
        {
            port.currentDir = r.str();
            port.formatted = r.boolean();
        }
        g_cvMcFileCursor = r.i32();
    }
}
