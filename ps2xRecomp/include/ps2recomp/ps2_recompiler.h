#ifndef PS2RECOMP_PS2_RECOMPILER_H
#define PS2RECOMP_PS2_RECOMPILER_H

#include "code_generator.h"
#include "config_manager.h"
#include "recompiler_reporter.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <set>

namespace ps2recomp
{
    class R5900Decoder;
    class ElfParser;

    enum class StubTarget
    {
        Unknown,
        Syscall,
        Stub
    };

    class PS2Recompiler
    {
    public:
        explicit PS2Recompiler(const std::string &configPath);
        ~PS2Recompiler();

        bool initialize();
        bool recompile();
        void generateOutput();
        void printReport() const;
        const RecompilerReporter::Counters &reportCounters() const { return m_reporter.counters(); }

        static StubTarget resolveStubTarget(const std::string& name);
        static bool IsCorrectnessCriticalFunctionName(const std::string &name);
        static size_t DiscoverAdditionalEntryPoints(
            std::vector<Function> &functions,
            std::unordered_map<uint32_t, std::vector<Instruction>> &decodedFunctions,
            const std::vector<Section> &sections);
        static size_t ResliceEntryFunctions(std::vector<Function> &functions, std::unordered_map<uint32_t, std::vector<Instruction>> &decodedFunctions);
        // resume_into_owner: drops every entry_* function nested in a real one (isRecompiled
        // cleared, decoded instructions removed) and adds its start to targetsByOwner for the
        // outermost owner (lowest start). Returns the number of fragments absorbed.
        static size_t AbsorbNestedEntryFragments(
            std::vector<Function> &functions,
            std::unordered_map<uint32_t, std::vector<Instruction>> &decodedFunctions,
            std::unordered_map<uint32_t, std::vector<uint32_t>> &targetsByOwner);
        static size_t CollectInternalEntryTargets(
            const std::vector<Function> &functions,
            const std::unordered_map<uint32_t, std::vector<Instruction>> &decodedFunctions,
            const std::unordered_set<uint32_t> &entryAddresses,
            std::unordered_map<uint32_t, std::vector<uint32_t>> &targetsByOwner);

        static std::string ClampFilenameLength(const std::string& baseName, const std::string& extension, std::size_t maxLength);
        // Names from `names` that `code` calls directly ("<name>(rdram, ctx, runtime)", not
        // namespace-qualified), sorted: the declarations a grouped output file needs.
        static std::vector<std::string> CollectDirectCallees(const std::string &code, const std::unordered_set<std::string> &names);
        // Name of the grouped output file for the window starting at windowStart.
        static std::string GroupOutputFileName(uint32_t windowStart);
        // The list of files the last run wrote, kept in the output folder to remove stale ones.
        static constexpr const char *kOutputManifestName = "ps2x_generated_files.txt";

    private:
        ConfigManager m_configManager;
        std::unique_ptr<ElfParser> m_elfParser;
        std::unique_ptr<R5900Decoder> m_decoder;
        std::unique_ptr<CodeGenerator> m_codeGenerator;
        RecompilerConfig m_config;
        RecompilerReporter m_reporter;

        std::vector<Function> m_functions;
        std::vector<Symbol> m_symbols;
        std::vector<Section> m_sections;
        std::vector<Relocation> m_relocations;

        std::unordered_map<uint32_t, std::vector<Instruction>> m_decodedFunctions;
        std::unordered_map<std::string, bool> m_skipFunctions;
        std::unordered_set<uint32_t> m_skipFunctionStarts;
        std::unordered_set<std::string> m_stubFunctions;
        std::unordered_set<uint32_t> m_stubFunctionStarts;
        std::unordered_map<uint32_t, std::string> m_stubHandlerBindingsByStart;
        std::unordered_set<uint32_t> m_entryPointHintStarts;
        // Config names (stub bindings, named entry points) for the runtime name table.
        std::unordered_map<uint32_t, std::string> m_guestDisplayNames;
        std::unordered_set<uint32_t> m_correctnessCriticalFunctionStarts;
        std::map<uint32_t, std::string> m_generatedStubs;
        std::unordered_map<uint32_t, std::string> m_functionRenames;
        std::unordered_map<uint32_t, std::vector<uint32_t>> m_resumeEntryTargetsByOwner;
        CodeGenerator::BootstrapInfo m_bootstrapInfo;
        std::mutex m_outputFilesMutex;
        std::set<std::string> m_outputFilesThisRun; // names of the files in the output folder written this run

        bool decodeFunction(Function &function);
        void discoverAdditionalEntryPoints();
        bool shouldSkipFunction(const Function &function) const;
        bool isStubFunction(const Function &function) const;
        bool isCorrectnessCriticalFunction(const Function &function) const;
        bool hasResolvedStubHandler(const Function &function) const;
        void collectCorrectnessCriticalFunctionStarts();
        bool generateFunctionHeader();
        bool generateStubHeader();
        bool writeToFile(const std::string &path, const std::string &content);
        std::filesystem::path getOutputPath(const Function &function) const;
        std::filesystem::path getGroupOutputPath(uint32_t windowStart) const;
        void finalizeOutputManifest();
        static std::string clampFilenameLength(const std::string& baseName, const std::string& extension, std::size_t maxLength);
        std::string sanitizeFunctionName(const std::string &name) const;       
    };

}

#endif
