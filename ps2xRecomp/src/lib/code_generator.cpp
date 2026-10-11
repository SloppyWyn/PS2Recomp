#include "ps2recomp/code_generator.h"
#include "ps2recomp/control_flow_analyzer.h"
#include "ps2recomp/Emitters/function_emitter.h"
#include "ps2recomp/Emitters/function_table_emitter.h"
#include "ps2recomp/Translators/instruction_translator.h"
#include "ps2recomp/Translators/special_translator.h"
#include "ps2recomp/Translators/regimm_translator.h"
#include "ps2recomp/Translators/cop0_translator.h"
#include "ps2recomp/Translators/fpu_translator.h"
#include "ps2recomp/Translators/mmi_translator.h"
#include "ps2recomp/Translators/vu_translator.h"
#include "ps2recomp/Emitters/control_flow_emitter.h"
#include "ps2recomp/control_flow_utils.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/ps2_recompiler.h"
#include "ps2recomp/r5900_decoder.h"
#include "ps2recomp/recompiler_reporter.h"
#include "ps2recomp/types.h"
#include "ps2_runtime_calls.h"

#include <fmt/format.h>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <utility>
#include <unordered_map>
#include <iostream>
#include <cctype>
#include <cmath>
#include <vector>

namespace ps2recomp
{
    const std::unordered_set<std::string> kKeywords = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool",
        "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class",
        "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast",
        "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
        "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable",
        "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq",
        "private", "protected", "public", "register", "reinterpret_cast", "requires", "return",
        "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef",
        "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile",
        "wchar_t", "while", "xor", "xor_eq"};
}

namespace ps2recomp
{

    static std::string sanitizeIdentifierBody(const std::string &name)
    {
        std::string sanitized;
        sanitized.reserve(name.size() + 1);

        for (char c : name)
        {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc) || c == '_')
            {
                sanitized.push_back(c);
            }
            else
            {
                sanitized.push_back('_');
            }
        }

        if (sanitized.empty())
        {
            return sanitized;
        }

        const unsigned char first = static_cast<unsigned char>(sanitized.front());
        if (!(std::isalpha(first) || sanitized.front() == '_'))
        {
            sanitized.insert(sanitized.begin(), '_');
        }

        return sanitized;
    }

    static bool isReservedCxxIdentifier(const std::string &name)
    {
        if (name.size() >= 2 && name[0] == '_' && name[1] == '_')
            return true;
        if (name.size() >= 2 && name[0] == '_' && std::isupper(static_cast<unsigned char>(name[1])))
            return true;
        return false;
    }

    static bool isReservedCxxKeyword(const std::string &name)
    {
        return kKeywords.contains(name);
    }

    CodeGenerator::CodeGenerator(const std::vector<Symbol> &symbols, const std::vector<Section> &sections)
        : m_sections(sections)
    {
        for (auto &symbol : symbols)
        {
            m_symbols.emplace(symbol.address, symbol);
        }
    }

    void CodeGenerator::setRenamedFunctions(const std::unordered_map<uint32_t, std::string> &renames)
    {
        m_renamedFunctions = renames;
    }

    void CodeGenerator::setBootstrapInfo(const BootstrapInfo &info)
    {
        m_bootstrapInfo = info;
    }

    void CodeGenerator::setRelocationCallNames(const std::unordered_map<uint32_t, std::string> &callNames)
    {
        m_relocationCallNames = callNames;
    }

    void CodeGenerator::setConfiguredJumpTables(const std::vector<JumpTable> &jumpTables)
    {
        m_configJumpTableTargetsByAddress.clear();
        for (const auto &table : jumpTables)
        {
            auto &targets = m_configJumpTableTargetsByAddress[table.address];
            for (const auto &entry : table.entries)
            {
                targets.push_back(entry.target);
            }
        }

        for (auto &[address, targets] : m_configJumpTableTargetsByAddress)
        {
            (void)address;
            std::sort(targets.begin(), targets.end());
            targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        }
    }

    void CodeGenerator::setResumeEntryTargets(const std::unordered_map<uint32_t, std::vector<uint32_t>> &resumeTargetsByOwner)
    {
        m_resumeEntryTargetsByOwner = resumeTargetsByOwner;
        m_resumeOwnerByTarget.clear();
        for (auto &[owner, targets] : m_resumeEntryTargetsByOwner)
        {
            std::sort(targets.begin(), targets.end());
            targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
            if (m_resumeIntoOwner)
            {
                for (uint32_t target : targets)
                {
                    // The function table keeps the first registration per address
                    // (FunctionTableEmitter); mirror that with the lowest owner start.
                    auto inserted = m_resumeOwnerByTarget.emplace(target, owner);
                    if (!inserted.second && owner < inserted.first->second)
                    {
                        inserted.first->second = owner;
                    }
                }
            }
        }
    }

    void CodeGenerator::setEmitInstructionComments(bool emitInstructionComments)
    {
        m_emitInstructionComments = emitInstructionComments;
    }

    void CodeGenerator::setReporter(RecompilerReporter *reporter)
    {
        m_reporter = reporter;
    }

    std::string CodeGenerator::getFunctionName(uint32_t address) const
    {
        auto it = m_renamedFunctions.find(address);
        if (it != m_renamedFunctions.end())
        {
            return it->second;
        }

        const Symbol *sym = findSymbolByAddress(address);
        if (sym && sym->isFunction)
        {
            return CodeGenerator::sanitizeFunctionName(sym->name);
        }

        return "";
    }

    std::string CodeGenerator::sanitizeFunctionName(const std::string &name) const
    {
        std::string sanitized = sanitizeIdentifierBody(name);
        if (sanitized.empty())
            return sanitized;

        // ugly but will do for now
        if (sanitized == "main")
            return "ps2_main";

        if (isReservedCxxKeyword(sanitized))
            return "ps2_" + sanitized;

        if (sanitized[0] == '_')
            return "ps2" + sanitized;

        if (!isReservedCxxIdentifier(sanitized))
            return sanitized;

        return "ps2_" + sanitized;
    }

    std::string CodeGenerator::handleBranchDelaySlots(
        const Instruction &branchInst,
        const Instruction &delaySlot,
        const Function &function,
        const AnalysisResult &analysisResult)
    {
        ControlFlowEmitter emitter(*this, branchInst, delaySlot, function, analysisResult);
        return emitter.emit();
    }

    std::string CodeGenerator::handleBranchDelaySlots(
        const Instruction &branchInst,
        const Instruction &delaySlot,
        const Function &function,
        const AnalysisResult &analysisResult,
        std::string delaySlotOverride)
    {
        ControlFlowEmitter emitter(*this, branchInst, delaySlot, function, analysisResult, std::move(delaySlotOverride));
        return emitter.emit();
    }

    CodeGenerator::~CodeGenerator() = default;

    std::string CodeGenerator::translateInstruction(const Instruction &inst)
    {
        return InstructionTranslator(*this).translate(inst, {});
    }

    std::string CodeGenerator::translateInstruction(const Instruction &inst, const MemoryAccessHint &memoryHint)
    {
        return InstructionTranslator(*this).translate(inst, memoryHint);
    }

    std::string CodeGenerator::translateSpecialInstruction(const Instruction &inst)
    {
        return SpecialTranslator(*this).translate(inst);
    }

    std::string CodeGenerator::translateRegimmInstruction(const Instruction &inst)
    {
        return RegimmTranslator(*this).translate(inst);
    }

    std::string CodeGenerator::translateCOP0Instruction(const Instruction &inst)
    {
        return Cop0Translator(*this).translate(inst);
    }

    std::string CodeGenerator::translateFPUInstruction(const Instruction &inst)
    {
        return FpuTranslator(*this).translate(inst);
    }

    std::string CodeGenerator::translateMMIInstruction(const Instruction &inst)
    {
        return MmiTranslator(*this).translate(inst);
    }

    std::string CodeGenerator::translateVUInstruction(const Instruction &inst)
    {
        return VuTranslator(*this).translate(inst);
    }

    CodeGenerator::AnalysisResult CodeGenerator::collectInternalBranchTargets(
        const Function &function, const std::vector<Instruction> &instructions, const std::vector<Function> *allFunctions)
    {
        ControlFlowAnalyzer analyzer(m_sections, m_configJumpTableTargetsByAddress, m_reporter, m_resumeIntoOwner);
        return analyzer.analyze(function, instructions, allFunctions);
    }

    std::string ps2recomp::CodeGenerator::generateFunction(
        const Function &function,
        const std::vector<Instruction> &instructions,
        const bool &useHeaders)
    {
        FunctionEmitter emitter(*this);
        return emitter.emit(function, instructions, useHeaders);
    }

    std::string CodeGenerator::emitUnhandledInstruction(const Instruction &inst, const std::string &message)
    {
        if (m_reporter)
        {
            m_reporter->recordUnhandledInstruction(m_currentFunctionName, inst.address, inst.raw, message);
        }

        return fmt::format("throw std::runtime_error(\"{} at 0x{:X} raw=0x{:08X}\");", message, inst.address, inst.raw);
    }

    std::string CodeGenerator::generateFunctionRegistration(const std::vector<Function> &functions, const std::map<uint32_t, std::string> &stubs)
    {
        FunctionTableEmitter emitter(*this);
        return emitter.emit(functions, stubs);
    }

    std::string CodeGenerator::leanIncludeLines() const
    {
        if (!m_lean.any())
        {
            return {};
        }
        std::string lines;
        if (m_lean.gprScalar)
        {
            lines += "#define PS2X_LEAN_GPR_SCALAR 1\n";
        }
        lines += std::string("#include \"") + kLeanMacrosHeader + "\"\n";
        return lines;
    }

    bool CodeGenerator::leanTranslationObservesPc(const std::string &code)
    {
        if (code.find("runtime->") != std::string::npos ||
            code.find("ps2_syscalls::") != std::string::npos ||
            code.find("ps2_stubs::") != std::string::npos ||
            code.find("throw") != std::string::npos ||
            code.find("ctx->pc") != std::string::npos)
        {
            return true;
        }

        // The lambda-style macros READ8/16/32/64/128( and WRITE8/16/32/64/128( call
        // runtime->Load/Store on special addresses; FAST_READ32( and PS2X_READ32( do not count.
        auto isIdentChar = [](char c)
        { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        for (const char *macro : {"READ", "WRITE"})
        {
            const size_t macroLength = std::strlen(macro);
            size_t pos = code.find(macro);
            while (pos != std::string::npos)
            {
                const bool wordStart = pos == 0 || !isIdentChar(code[pos - 1]);
                size_t digits = pos + macroLength;
                while (digits < code.size() && std::isdigit(static_cast<unsigned char>(code[digits])))
                {
                    ++digits;
                }
                if (wordStart && digits > pos + macroLength && digits < code.size() && code[digits] == '(')
                {
                    return true;
                }
                pos = code.find(macro, pos + macroLength);
            }
        }
        return false;
    }

    std::string CodeGenerator::leanFoldZeroRegisters(std::string code)
    {
        static const std::pair<const char *, const char *> kFolds[] = {
            {"GPR_U32(ctx, 0)", "0u"},
            {"GPR_S32(ctx, 0)", "0"},
            {"GPR_U64(ctx, 0)", "0ull"},
            {"GPR_S64(ctx, 0)", "0ll"},
            {"GPR_VEC(ctx, 0)", "_mm_setzero_si128()"},
        };
        auto isIdentChar = [](char c)
        { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
        for (const auto &[pattern, replacement] : kFolds)
        {
            const size_t patternLength = std::strlen(pattern);
            size_t pos = code.find(pattern);
            while (pos != std::string::npos)
            {
                // Not the tail of another identifier (SET_GPR_U32 has three arguments, so it
                // never matches, but keep the check explicit).
                if (pos == 0 || !isIdentChar(code[pos - 1]))
                {
                    code.replace(pos, patternLength, replacement);
                    pos = code.find(pattern, pos + std::strlen(replacement));
                }
                else
                {
                    pos = code.find(pattern, pos + patternLength);
                }
            }
        }
        return code;
    }

    const Symbol *CodeGenerator::findSymbolByAddress(uint32_t address) const
    {
        auto it = m_symbols.find(address);
        if (it != m_symbols.end())
        {
            return &it->second;
        }

        return nullptr;
    }
}
