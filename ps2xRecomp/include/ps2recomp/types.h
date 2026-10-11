#ifndef PS2RECOMP_TYPES_H
#define PS2RECOMP_TYPES_H

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <map>

namespace ps2recomp
{

    // Instruction representation
    struct Instruction
    {
        uint32_t address;
        uint32_t opcode;
        uint32_t rs;         // Source register
        uint32_t rt;         // Target register
        uint32_t rd;         // Destination register
        uint32_t sa;         // Shift amount
        uint32_t function;   // Function code for R-type instructions
        uint32_t immediate;  // Immediate value for I-type instructions
        uint32_t simmediate; // Sign-extended immediate value (extended to 32 bits)
        uint32_t target;     // Jump target for J-type instructions
        uint32_t raw;        // Raw instruction value

        // Instruction type flags
        bool isMMI;        // Is MMI instruction (PS2 specific)
        bool isVU;         // Is VU instruction (PS2 specific)
        bool isBranch;     // Is branch instruction
        bool isJump;       // Is jump instruction
        bool isCall;       // Is function call
        bool isReturn;     // Is return instruction
        bool hasDelaySlot; // Has delay slot
        bool isMultimedia; // PS2-specific multimedia operations
        bool isStore;      // Is store instruction
        bool isLoad;       // Is load instruction

        // Additional PS2-specific fields
        uint8_t mmiType;        // 0=MMI0, 1=MMI1, 2=MMI2, 3=MMI3
        uint8_t mmiFunction;    // Function within MMI type
        uint8_t pmfhlVariation; // For PMFHL instructions
        uint8_t vuFunction;     // For VU instructions

        bool isMmio = false;
        uint32_t mmioAddress = 0;
        std::string disassembly;

        struct
        {
            bool isVector;       // Uses vector operations
            bool usesQReg;       // Uses Q register
            bool usesPReg;       // Uses P register
            bool modifiesMAC;    // Modifies MAC flags
            uint8_t vectorField; // xyzw field mask
            uint8_t fsf;         // Field select for FS reg (bits 10-11)
            uint8_t ftf;         // Source Field select for FT reg (bits 8-9)
        } vectorInfo;

        struct
        {
            bool modifiesGPR;     // Modifies general purpose register
            bool modifiesFPR;     // Modifies floating point register
            bool modifiesVFR;     // Modifies vector float register
            bool modifiesVIR;     // Modifies vector integer register
            bool modifiesVIC;     // Modifies vector integer control register
            bool modifiesMemory;  // Modifies memory
            bool modifiesControl; // Modifies control register
        } modificationInfo;

        Instruction() : address(0), opcode(0), rs(0), rt(0), rd(0), sa(0), function(0),
                        immediate(0), simmediate(0), target(0), raw(0),
                        isMMI(false), isVU(false), isBranch(false), isJump(false), isCall(false),
                        isReturn(false), hasDelaySlot(false), isMultimedia(false), isStore(false), isLoad(false),
                        mmiType(0), mmiFunction(0), pmfhlVariation(0), vuFunction(0), isMmio(false), mmioAddress(0)
        {
            vectorInfo = {};
            modificationInfo = {};
        }
    };

    struct MemoryAccessHint
    {
        bool hasAddress = false;
        uint32_t address = 0;
    };

    // Function information
    struct Function
    {
        std::string name;
        uint32_t start;
        uint32_t end;
        std::vector<Instruction> instructions;
        std::vector<uint32_t> callers;
        std::vector<uint32_t> callees;
        bool isRecompiled = false;
        bool isStub = false;
        bool isSkipped = false;
    };

    // Symbol information
    struct Symbol
    {
        std::string name;
        uint32_t address;
        uint32_t size;
        bool isFunction;
        bool isImported;
        bool isExported;
    };

    // Section information
    struct Section
    {
        std::string name;
        uint32_t address;
        uint32_t size;
        uint32_t offset;
        bool isCode;
        bool isData;
        bool isBSS;
        bool isReadOnly;
        uint8_t *data;
    };

    // Relocation information
    struct Relocation
    {
        uint32_t offset;
        uint32_t info;
        uint32_t symbol;
        std::string symbolName;
        uint32_t type;
        int32_t addend;
    };

    // Jump table entry
    struct JumpTableEntry
    {
        uint32_t index;
        uint32_t target;
    };

    // Jump table
    struct JumpTable
    {
        uint32_t address;
        uint32_t baseRegister;
        std::vector<JumpTableEntry> entries;
    };

    // Control flow graph
    struct CFGNode
    {
        uint32_t startAddress;
        uint32_t endAddress;
        std::vector<Instruction> instructions;
        std::vector<uint32_t> predecessors;
        std::vector<uint32_t> successors;
        bool isJumpTarget;
        bool hasJumpTable;
        JumpTable jumpTable;
    };

    // Function call
    struct FunctionCall
    {
        uint32_t callerAddress;
        uint32_t calleeAddress;
        std::string calleeName;
    };

    // Recompiler configuration
    struct RecompilerConfig
    {
        std::string inputPath;
        std::string outputPath;
        std::string ghidraMapPath;
        bool singleFileOutput = false;
        // Without single_file_output: when non-zero, functions are written grouped by guest
        // address, one .cpp per aligned window of this many bytes (e.g. 0x2000), so a change
        // to one function rewrites one file and the generated headers stay out of every TU.
        // 0 (default) = one .cpp per function (the original layout).
        uint32_t outputGroupBytes = 0;
        // resume_into_owner: an entry_* code label nested inside a real function (Ghidra
        // exports every referenced label as one) is not emitted as its own fragment
        // function; its address is registered to the owner function instead, whose
        // `switch (ctx->pc)` jumps to the label. The scheduler then resumes a thread in the
        // middle of the owner and the code runs on natively instead of returning to the
        // scheduler at every label. Labels outside any real function stay fragments.
        // false (default) = fragments as before.
        bool resumeIntoOwner = false;
        // lean_codegen: leaner generated code with the same guest semantics (see
        // ps2xRuntime/include/ps2_runtime_lean_macros.h and docs/LEAN-CODEGEN.md):
        // `ctx->pc` is stored only where the runtime can observe it (before calls, dispatches,
        // syscalls, traps, checkpoints, returns and branch targets, and in the slow path of a
        // memory access that reaches the runtime), the delay-slot flags only around delay
        // slots that can reach the runtime, memory accesses go through inline functions
        // instead of lambdas (no ps2TraceGuestWrite no-op), the GPR accessors are scalar
        // 32/64-bit loads and stores of the low half (upper 64 bits untouched, as before) and
        // $zero operands are folded to literals. false (default) = output byte-identical to
        // before. lean_codegen = true turns every trim on; each has its own key too
        // (lean_pc_stores, lean_memory_helpers, lean_delay_slot_flags, lean_gpr_scalar,
        // lean_zero_fold), read after lean_codegen so it can switch one trim off (or on alone).
        bool leanCodegen = false;
        struct LeanOptions
        {
            bool pcStores = false;       // drop ctx->pc stores nothing can observe
            bool memoryHelpers = false;  // PS2X_READn/WRITEn inline helpers instead of the lambdas
            bool delaySlotFlags = false; // in_delay_slot/branch_pc only around runtime-reaching delay slots
            bool gprScalar = false;      // scalar low-half GPR accessors
            bool zeroFold = false;       // GPR_*(ctx, 0) -> literal
            bool any() const { return pcStores || memoryHelpers || delaySlotFlags || gprScalar || zeroFold; }
            static LeanOptions all() { return LeanOptions{true, true, true, true, true}; }
        };
        LeanOptions lean;
        // The trims to apply: `lean` as merged by ConfigManager (lean_codegen plus per-trim
        // keys), or every trim when leanCodegen was set in code without touching `lean`.
        LeanOptions effectiveLeanOptions() const { return (leanCodegen && !lean.any()) ? LeanOptions::all() : lean; }
        bool lowMemoryMode = false;
        uint32_t outputWorkerThreads = 0;
        bool patchSyscalls = false;
        bool patchCop0 = true;
        bool patchCache = true;
        std::vector<std::string> skipFunctions;
        std::unordered_map<uint32_t, std::string> patches;
        std::vector<std::string> stubImplementations;
        std::vector<std::string> entryPointHints;
        std::unordered_map<uint32_t, uint32_t> mmioByInstructionAddress;
        std::vector<JumpTable> jumpTables;
    };

} // namespace ps2recomp

#endif // PS2RECOMP_TYPES_H
