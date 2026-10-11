#include "Common.h"
#include "Audio.h"
#include "runtime/ee_scheduler.h"

namespace ps2_stubs
{
    namespace
    {
        constexpr uint32_t kLibSdCmdSetParam = 0x8010u;
        constexpr uint32_t kLibSdCmdVoiceTrans = 0x80D0u;
        constexpr uint32_t kLibSdCmdBlockTrans = 0x80E0u;
        constexpr uint32_t kLibSdCmdVoiceTransStatus = 0x80F0u;
        constexpr uint32_t kLibSdCmdBlockTransStatus = 0x8100u;
        constexpr uint32_t kAudioPositionMask = 0x00FFFFFFu;
        constexpr uint32_t kAudioTransferUnit = 1024u;
        constexpr uint32_t kLibSdCoreCount = 2u;
        constexpr uint32_t kLibSdTransModeIo = 0x08u;
        constexpr uint32_t kLibSdTransDirectionMask = 0x03u;
        constexpr uint32_t kLibSdTransStop = 0x02u;
        constexpr uint32_t kLibSdTransWrite = 0x00u;     // SD_TRANS_MODE_WRITE: IOP memory -> SPU2 (playback)
        constexpr uint32_t kLibSdTransWriteFrom = 0x03u; // SD_TRANS_MODE_WRITE_FROM: the same, from a given point
        constexpr uint32_t kLibSdTransLoop = 0x10u;

        struct VoiceTransferState
        {
            uint32_t sourceAddress = 0u;
            uint32_t destinationAddress = 0u;
            uint32_t size = 0u;
            uint16_t mode = 0u;
            bool completed = true;
        };

        struct BlockTransferState
        {
            uint32_t base = 0u;
            uint32_t size = 0u;
            uint32_t pauseBase = 0u;
            uint32_t offset = 0u;
            uint32_t statusTraceCount = 0u;
            uint16_t mode = 0u;
            bool active = false;
            bool loop = false;
            // Guest-clock position of the transfer (see blockTransferAdvance).
            bool clocked = false;
            uint64_t clockCycle = 0u;
            uint64_t clockRemainder = 0u;
            bool finished = false; // a one-shot transfer that reached its end
        };

        // SPU2 block transfer (AutoDMA): the core's input FIFO takes 48,000 stereo 16-bit
        // samples per second, 1024 bytes (256 samples per channel) per DMA block.
        constexpr uint64_t kBlockTransBytesPerSecond = 48000u * 2u * 2u;
        constexpr uint64_t kEeClockHz = 294912000u;

        // PS2X_SD_BLOCK_CLOCK=0: the status poll advances one block per call (the old model;
        // A/B only). Also the model without a runtime (unit tests that pass none), and for
        // SD_TRANS_MODE_READ (SPU2 -> IOP capture): the clock is the playback (write) rate;
        // the capture rate is not verified and no game here uses it, so reads keep the old model.
        bool blockTransferClockEnabled()
        {
            static const bool enabled = []
            {
                const char *v = std::getenv("PS2X_SD_BLOCK_CLOCK");
                return !(v && v[0] == '0' && v[1] == '\0');
            }();
            return enabled;
        }

        // Moves the transfer position on by the blocks the hardware would have sent since the
        // last poll (guest EE clock), or by one block per poll without a clock.
        // A one-shot transfer (no SD_BLOCK_LOOP) stops at the end of its buffer instead of
        // wrapping; sceSdBlockTransStatus then reports no transfer in progress (address 0, as
        // libsd reads it from the stopped DMA channel).
        void blockTransferMove(BlockTransferState &transfer, uint64_t bytes)
        {
            const uint64_t raw = static_cast<uint64_t>(transfer.offset) + bytes;
            if (!transfer.loop && raw >= transfer.size)
            {
                transfer.offset = transfer.size;
                transfer.active = false;
                transfer.finished = true;
                return;
            }
            transfer.offset = static_cast<uint32_t>(raw % transfer.size);
        }

        void blockTransferAdvance(BlockTransferState &transfer, const uint64_t *nowCycle)
        {
            const uint32_t direction = transfer.mode & kLibSdTransDirectionMask;
            const bool playback = direction == kLibSdTransWrite || direction == kLibSdTransWriteFrom;
            if (nowCycle == nullptr || !blockTransferClockEnabled() || !playback)
            {
                blockTransferMove(transfer, kAudioTransferUnit);
                return;
            }
            if (!transfer.clocked || *nowCycle < transfer.clockCycle)
            {
                transfer.clocked = true;
                transfer.clockCycle = *nowCycle;
                transfer.clockRemainder = 0u;
                return;
            }
            constexpr uint64_t kCyclesPerBlockScaled = kEeClockHz * kAudioTransferUnit;
            const uint64_t elapsed = *nowCycle - transfer.clockCycle;
            transfer.clockCycle = *nowCycle;
            const uint64_t scaled = elapsed * kBlockTransBytesPerSecond + transfer.clockRemainder;
            const uint64_t blocks = scaled / kCyclesPerBlockScaled;
            transfer.clockRemainder = scaled % kCyclesPerBlockScaled;
            blockTransferMove(transfer, transfer.loop ? (blocks % transfer.size) * kAudioTransferUnit
                                                      : blocks * kAudioTransferUnit);
        }

        struct AudioStubState
        {
            bool initialized = false;
            std::array<VoiceTransferState, kLibSdCoreCount> voiceTransfers{};
            std::array<BlockTransferState, kLibSdCoreCount> blockTransfers{};
        };

        std::mutex g_audio_stub_mutex;
        AudioStubState g_audio_stub_state;

        void resetAudioStubStateUnlocked()
        {
            g_audio_stub_state = {};
        }

        uint32_t currentBlockStatus(const BlockTransferState &transfer)
        {
            if (transfer.finished)
            {
                return 0u;
            }
            const uint32_t position = (transfer.base + transfer.offset) & kAudioPositionMask;
            const uint32_t halfSize = transfer.size / 2u;
            const uint32_t bank = (transfer.loop && halfSize != 0u && transfer.offset >= halfSize) ? 1u : 0u;
            return (bank << 24u) | position;
        }
    }

    void resetAudioStubState()
    {
        std::lock_guard<std::mutex> lock(g_audio_stub_mutex);
        resetAudioStubStateUnlocked();
    }

    void sceSdCallBack(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSdCallBack", rdram, ctx, runtime);
    }

    // sceSdRemote after decoding, shared by the typed and the legacy form; returns v0.
    static uint32_t sdRemoteBody(uint32_t cmd, uint32_t cmdArg0, uint32_t cmdArg1, uint32_t arg4, uint32_t arg5,
                                 uint32_t arg6, const uint64_t *nowCycle)
    {
        std::lock_guard<std::mutex> lock(g_audio_stub_mutex);
        g_audio_stub_state.initialized = true;
        const uint32_t core = cmdArg0 & (kLibSdCoreCount - 1u);
        VoiceTransferState &voiceTransfer = g_audio_stub_state.voiceTransfers[core];
        BlockTransferState &blockTransfer = g_audio_stub_state.blockTransfers[core];
        uint32_t returnValue = 0u;

        if (cmd == kLibSdCmdVoiceTrans)
        {
            voiceTransfer.sourceAddress = arg4;
            voiceTransfer.destinationAddress = arg5;
            voiceTransfer.size = arg6;
            voiceTransfer.mode = static_cast<uint16_t>(cmdArg1);
            voiceTransfer.completed = true;

            // sceSdVoiceTrans returns the transferred byte count for DMA
            // mode and zero for its synchronous programmed-I/O mode.
            returnValue = ((voiceTransfer.mode & kLibSdTransModeIo) != 0u)
                              ? 0u
                              : voiceTransfer.size;

            PS2_IF_AGRESSIVE_LOGS({
                std::cerr << "[Audio:VoiceTrans] core=" << core
                          << " src=0x" << std::hex << voiceTransfer.sourceAddress
                          << " dest=0x" << voiceTransfer.destinationAddress
                          << " size=0x" << voiceTransfer.size
                          << " mode=0x" << voiceTransfer.mode
                          << std::dec << std::endl;
            });
        }
        else if (cmd == kLibSdCmdBlockTrans)
        {
            const uint32_t direction = cmdArg1 & kLibSdTransDirectionMask;
            if (direction == kLibSdTransStop)
            {
                returnValue = currentBlockStatus(blockTransfer);
                blockTransfer = {};
            }
            else if (arg4 != 0u && arg5 != 0u)
            {
                blockTransfer.base = arg4 & kAudioPositionMask;
                blockTransfer.size = arg5;
                blockTransfer.pauseBase =
                    ((arg6 != 0u) ? arg6 : arg4) & kAudioPositionMask;
                blockTransfer.mode = static_cast<uint16_t>(cmdArg1);
                blockTransfer.loop = (cmdArg1 & kLibSdTransLoop) != 0u;

                const uint32_t pauseOffset =
                    (blockTransfer.pauseBase - blockTransfer.base) & kAudioPositionMask;
                blockTransfer.offset = (pauseOffset < blockTransfer.size) ? pauseOffset : 0u;
                blockTransfer.statusTraceCount = 0u;
                blockTransfer.active = true;
                blockTransfer.clocked = nowCycle != nullptr;
                blockTransfer.clockCycle = nowCycle != nullptr ? *nowCycle : 0u;
                blockTransfer.clockRemainder = 0u;
                returnValue = 0u;
            }
            else
            {
                returnValue = std::numeric_limits<uint32_t>::max();
            }

            PS2_IF_AGRESSIVE_LOGS({
                std::cerr << "[Audio:BlockTrans] core=" << core
                          << " active=" << blockTransfer.active
                          << " base=0x" << std::hex << blockTransfer.base
                          << " size=0x" << blockTransfer.size
                          << " pause=0x" << blockTransfer.pauseBase
                          << " offset=0x" << blockTransfer.offset
                          << std::dec << std::endl;
            });
        }
        else if (cmd == kLibSdCmdVoiceTransStatus)
        {
            returnValue = voiceTransfer.completed ? 1u : 0u;
        }
        else if (cmd == kLibSdCmdBlockTransStatus)
        {
            if (blockTransfer.active && blockTransfer.size != 0u)
            {
                blockTransferAdvance(blockTransfer, nowCycle);
                if (blockTransfer.statusTraceCount < 32u)
                {
                    PS2_IF_AGRESSIVE_LOGS({
                        std::cerr << "[Audio:BlockStatus] core=" << core
                                  << " status=0x" << std::hex << currentBlockStatus(blockTransfer)
                                  << " offset=0x" << blockTransfer.offset
                                  << " size=0x" << blockTransfer.size
                                  << std::dec << std::endl;
                    });
                    ++blockTransfer.statusTraceCount;
                }
            }
            returnValue = currentBlockStatus(blockTransfer);
        }
        else if (cmd == kLibSdCmdSetParam)
        {
            (void)cmdArg0;
            (void)cmdArg1;
            returnValue = 0u;
        }

        return returnValue;
    }

    // sceSdRemote(int arg, int command, ...): libsdr's variadic RPC call. The game's copy (0x127688)
    // takes a0-t3 and copies a2..t3 into the RPC buffer, nothing from the caller's stack, so it is
    // declared with seven fixed integer arguments (the varargs continue the a0-t3 sequence): a0
    // (`arg`, the RPC wait mode) is not used by the runtime; t3 is not read either.
    PS2_STUB_TYPED(uint32_t, sceSdRemote, uint32_t arg, uint32_t cmd, uint32_t cmdArg0, uint32_t cmdArg1, uint32_t arg4,
                   uint32_t arg5, uint32_t arg6)
    {
        (void)rdram;
        (void)ctx;
        (void)arg;
        const uint64_t now = runtime != nullptr ? runtime->eeScheduler().eeCycle() : 0u;
        return sdRemoteBody(cmd, cmdArg0, cmdArg1, arg4, arg5, arg6, runtime != nullptr ? &now : nullptr);
    }

    void sceSdRemote_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const uint32_t cmd = getRegU32(ctx, 5);
        const uint32_t cmdArg0 = getRegU32(ctx, 6);
        const uint32_t cmdArg1 = getRegU32(ctx, 7);
        // Arguments 5-7 are in t0-t2 (EE ABI). The game's copy (0x127688) copies a2..t3 into the
        // RPC buffer and reads nothing from the caller's stack, so a 0 in t0-t2 is a real 0.
        const uint32_t arg4 = getRegU32(ctx, 8);
        const uint32_t arg5 = getRegU32(ctx, 9);
        const uint32_t arg6 = getRegU32(ctx, 10);

        const uint64_t now = runtime != nullptr ? runtime->eeScheduler().eeCycle() : 0u;
        setReturnU32(ctx, sdRemoteBody(cmd, cmdArg0, cmdArg1, arg4, arg5, arg6, runtime != nullptr ? &now : nullptr));
    }

    PS2_STUB_DISPATCH(sceSdRemote)
    PS2_STUB_DEVIATION(sceSdRemote, "SDK: int sceSdRemote(int arg, int command, ...), variadic; declared with 7 fixed integer arguments (the varargs continue a0-t3; the game's copy reads nothing from the stack) and a u32 return, as the old decoding")

    // int sceSdRemoteInit(void) (libsdr)
    PS2_STUB0_TYPED(int32_t, sceSdRemoteInit)
    {
        (void)rdram;
        (void)runtime;

        std::lock_guard<std::mutex> lock(g_audio_stub_mutex);
        resetAudioStubStateUnlocked();
        g_audio_stub_state.initialized = true;
        return 0;
    }

    void sceSdRemoteInit_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        std::lock_guard<std::mutex> lock(g_audio_stub_mutex);
        resetAudioStubStateUnlocked();
        g_audio_stub_state.initialized = true;
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceSdRemoteInit)

    void sceSdTransToIOP(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSdTransToIOP", rdram, ctx, runtime);
    }

    void sceSSyn_BreakAtick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSSyn_BreakAtick", rdram, ctx, runtime);
    }

    void sceSSyn_ClearBreakAtick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSSyn_ClearBreakAtick", rdram, ctx, runtime);
    }

    void sceSSyn_SendExcMsg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSSyn_SendExcMsg", rdram, ctx, runtime);
    }

    void sceSSyn_SendNrpnMsg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSSyn_SendNrpnMsg", rdram, ctx, runtime);
    }

    void sceSSyn_SendRpnMsg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSSyn_SendRpnMsg", rdram, ctx, runtime);
    }

    void sceSSyn_SendShortMsg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSSyn_SendShortMsg", rdram, ctx, runtime);
    }

    void sceSSyn_SetChPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSSyn_SetChPriority", rdram, ctx, runtime);
    }

    void sceSSyn_SetMasterVolume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSSyn_SetMasterVolume", rdram, ctx, runtime);
    }

    void sceSSyn_SetOutPortVolume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSSyn_SetOutPortVolume", rdram, ctx, runtime);
    }

    void sceSSyn_SetOutputAssign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSSyn_SetOutputAssign", rdram, ctx, runtime);
    }

    // int sceSSyn_SetOutputMode(int mode)
    PS2_STUB_TYPED(int32_t, sceSSyn_SetOutputMode, int32_t mode)
    {
        (void)mode;
        return 0;
    }

    void sceSSyn_SetOutputMode_legacy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    PS2_STUB_DISPATCH(sceSSyn_SetOutputMode)

    void sceSSyn_SetPortMaxPoly(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSSyn_SetPortMaxPoly", rdram, ctx, runtime);
    }

    void sceSSyn_SetPortVolume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSSyn_SetPortVolume", rdram, ctx, runtime);
    }

    void sceSSyn_SetTvaEnvMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSSyn_SetTvaEnvMode", rdram, ctx, runtime);
    }

    void sceSynthesizerAmpProcI(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerAmpProcI", rdram, ctx, runtime);
    }

    void sceSynthesizerAmpProcNI(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerAmpProcNI", rdram, ctx, runtime);
    }

    void sceSynthesizerAssignAllNoteOff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerAssignAllNoteOff", rdram, ctx, runtime);
    }

    void sceSynthesizerAssignAllSoundOff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerAssignAllSoundOff", rdram, ctx, runtime);
    }

    void sceSynthesizerAssignHoldChange(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerAssignHoldChange", rdram, ctx, runtime);
    }

    void sceSynthesizerAssignNoteOff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerAssignNoteOff", rdram, ctx, runtime);
    }

    void sceSynthesizerAssignNoteOn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerAssignNoteOn", rdram, ctx, runtime);
    }

    void sceSynthesizerCalcEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerCalcEnv", rdram, ctx, runtime);
    }

    void sceSynthesizerCalcPortamentPitch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerCalcPortamentPitch", rdram, ctx, runtime);
    }

    void sceSynthesizerCalcTvfCoefAll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerCalcTvfCoefAll", rdram, ctx, runtime);
    }

    void sceSynthesizerCalcTvfCoefF0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerCalcTvfCoefF0", rdram, ctx, runtime);
    }

    void sceSynthesizerCent2PhaseInc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerCent2PhaseInc", rdram, ctx, runtime);
    }

    void sceSynthesizerChangeEffectSend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangeEffectSend", rdram, ctx, runtime);
    }

    void sceSynthesizerChangeHsPanpot(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangeHsPanpot", rdram, ctx, runtime);
    }

    void sceSynthesizerChangeNrpnCutOff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangeNrpnCutOff", rdram, ctx, runtime);
    }

    void sceSynthesizerChangeNrpnLfoDepth(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangeNrpnLfoDepth", rdram, ctx, runtime);
    }

    void sceSynthesizerChangeNrpnLfoRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangeNrpnLfoRate", rdram, ctx, runtime);
    }

    void sceSynthesizerChangeOutAttrib(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangeOutAttrib", rdram, ctx, runtime);
    }

    void sceSynthesizerChangeOutVol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangeOutVol", rdram, ctx, runtime);
    }

    void sceSynthesizerChangePanpot(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangePanpot", rdram, ctx, runtime);
    }

    void sceSynthesizerChangePartBendSens(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangePartBendSens", rdram, ctx, runtime);
    }

    void sceSynthesizerChangePartExpression(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangePartExpression", rdram, ctx, runtime);
    }

    void sceSynthesizerChangePartHsExpression(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangePartHsExpression", rdram, ctx, runtime);
    }

    void sceSynthesizerChangePartHsPitchBend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangePartHsPitchBend", rdram, ctx, runtime);
    }

    void sceSynthesizerChangePartModuration(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangePartModuration", rdram, ctx, runtime);
    }

    void sceSynthesizerChangePartPitchBend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangePartPitchBend", rdram, ctx, runtime);
    }

    void sceSynthesizerChangePartVolume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangePartVolume", rdram, ctx, runtime);
    }

    void sceSynthesizerChangePortamento(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangePortamento", rdram, ctx, runtime);
    }

    void sceSynthesizerChangePortamentoTime(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerChangePortamentoTime", rdram, ctx, runtime);
    }

    void sceSynthesizerClearKeyMap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerClearKeyMap", rdram, ctx, runtime);
    }

    void sceSynthesizerClearSpr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerClearSpr", rdram, ctx, runtime);
    }

    void sceSynthesizerCopyOutput(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerCopyOutput", rdram, ctx, runtime);
    }

    void sceSynthesizerDmaFromSPR(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerDmaFromSPR", rdram, ctx, runtime);
    }

    void sceSynthesizerDmaSpr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerDmaSpr", rdram, ctx, runtime);
    }

    void sceSynthesizerDmaToSPR(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerDmaToSPR", rdram, ctx, runtime);
    }

    void sceSynthesizerGetPartial(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerGetPartial", rdram, ctx, runtime);
    }

    void sceSynthesizerGetPartOutLevel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerGetPartOutLevel", rdram, ctx, runtime);
    }

    void sceSynthesizerGetSampleParam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerGetSampleParam", rdram, ctx, runtime);
    }

    void sceSynthesizerHsMessage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerHsMessage", rdram, ctx, runtime);
    }

    void sceSynthesizerLfoNone(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerLfoNone", rdram, ctx, runtime);
    }

    void sceSynthesizerLfoProc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerLfoProc", rdram, ctx, runtime);
    }

    void sceSynthesizerLfoSawDown(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerLfoSawDown", rdram, ctx, runtime);
    }

    void sceSynthesizerLfoSawUp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerLfoSawUp", rdram, ctx, runtime);
    }

    void sceSynthesizerLfoSquare(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerLfoSquare", rdram, ctx, runtime);
    }

    void sceSynthesizerReadNoise(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerReadNoise", rdram, ctx, runtime);
    }

    void sceSynthesizerReadNoiseAdd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerReadNoiseAdd", rdram, ctx, runtime);
    }

    void sceSynthesizerReadSample16(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerReadSample16", rdram, ctx, runtime);
    }

    void sceSynthesizerReadSample16Add(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerReadSample16Add", rdram, ctx, runtime);
    }

    void sceSynthesizerReadSample8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerReadSample8", rdram, ctx, runtime);
    }

    void sceSynthesizerReadSample8Add(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerReadSample8Add", rdram, ctx, runtime);
    }

    void sceSynthesizerResetPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerResetPart", rdram, ctx, runtime);
    }

    void sceSynthesizerRestorDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerRestorDma", rdram, ctx, runtime);
    }

    void sceSynthesizerSelectPatch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerSelectPatch", rdram, ctx, runtime);
    }

    void sceSynthesizerSendShortMessage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerSendShortMessage", rdram, ctx, runtime);
    }

    void sceSynthesizerSetMasterVolume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerSetMasterVolume", rdram, ctx, runtime);
    }

    void sceSynthesizerSetRVoice(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerSetRVoice", rdram, ctx, runtime);
    }

    void sceSynthesizerSetupDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerSetupDma", rdram, ctx, runtime);
    }

    void sceSynthesizerSetupLfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerSetupLfo", rdram, ctx, runtime);
    }

    void sceSynthesizerSetupMidiModuration(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerSetupMidiModuration", rdram, ctx, runtime);
    }

    void sceSynthesizerSetupMidiPanpot(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerSetupMidiPanpot", rdram, ctx, runtime);
    }

    void sceSynthesizerSetupNewNoise(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerSetupNewNoise", rdram, ctx, runtime);
    }

    void sceSynthesizerSetupReleaseEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerSetupReleaseEnv", rdram, ctx, runtime);
    }

    void sceSynthesizerSetuptEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerSetuptEnv", rdram, ctx, runtime);
    }

    void sceSynthesizerSetupTruncateTvaEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerSetupTruncateTvaEnv", rdram, ctx, runtime);
    }

    void sceSynthesizerSetupTruncateTvfPitchEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerSetupTruncateTvfPitchEnv", rdram, ctx, runtime);
    }

    void sceSynthesizerTonegenerator(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerTonegenerator", rdram, ctx, runtime);
    }

    void sceSynthesizerTransposeMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerTransposeMatrix", rdram, ctx, runtime);
    }

    void sceSynthesizerTvfProcI(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerTvfProcI", rdram, ctx, runtime);
    }

    void sceSynthesizerTvfProcNI(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerTvfProcNI", rdram, ctx, runtime);
    }

    void sceSynthesizerWaitDmaFromSPR(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerWaitDmaFromSPR", rdram, ctx, runtime);
    }

    void sceSynthesizerWaitDmaToSPR(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthesizerWaitDmaToSPR", rdram, ctx, runtime);
    }

    void sceSynthsizerGetDrumPatch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthsizerGetDrumPatch", rdram, ctx, runtime);
    }

    void sceSynthsizerGetMeloPatch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthsizerGetMeloPatch", rdram, ctx, runtime);
    }

    void sceSynthsizerLfoNoise(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthsizerLfoNoise", rdram, ctx, runtime);
    }

    void sceSynthSizerLfoTriangle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSynthSizerLfoTriangle", rdram, ctx, runtime);
    }
}

#include "ps2_snapshot_hooks.h"

namespace ps2_stubs::snapshot
{
    void saveAudio(ps2x::StateWriter &w)
    {
        snapshotSaveSupport(w);
        std::lock_guard<std::mutex> lock(g_audio_stub_mutex);
        w.boolean(g_audio_stub_state.initialized);
        w.pod(g_audio_stub_state.voiceTransfers);
        for (const BlockTransferState &b : g_audio_stub_state.blockTransfers)
        {
            w.u32(b.base);
            w.u32(b.size);
            w.u32(b.pauseBase);
            w.u32(b.offset);
            w.u32(b.statusTraceCount);
            w.u32(b.mode);
            w.boolean(b.active);
            w.boolean(b.loop);
            w.boolean(b.clocked);
            w.u64(b.clockCycle);
            w.u64(b.clockRemainder);
            w.boolean(b.finished);
        }
    }

    void loadAudio(ps2x::StateReader &r, PS2Runtime &runtime)
    {
        snapshotLoadSupport(r);
        std::lock_guard<std::mutex> lock(g_audio_stub_mutex);
        g_audio_stub_state = {};
        g_audio_stub_state.initialized = r.boolean();
        r.pod(g_audio_stub_state.voiceTransfers);
        // The EE scheduler section (and its eeCycle) loads before the stub sections.
        const uint64_t now = runtime.eeScheduler().eeCycle();
        for (BlockTransferState &b : g_audio_stub_state.blockTransfers)
        {
            b.base = r.u32();
            b.size = r.u32();
            b.pauseBase = r.u32();
            b.offset = r.u32();
            b.statusTraceCount = r.u32();
            b.mode = static_cast<uint16_t>(r.u32());
            b.active = r.boolean();
            b.loop = r.boolean();
            b.clocked = r.boolean();
            b.clockCycle = r.u64();
            b.clockRemainder = r.u64();
            b.finished = r.boolean();
            // The position clock runs on the guest EE clock, which the load restores: the saved
            // cycle stays valid. Rebase it if it lies ahead of the loaded clock (a state put
            // into a runtime whose clock is behind), so the next poll does not wait for it.
            if (b.clocked && b.clockCycle > now)
            {
                b.clockCycle = now;
            }
        }
    }
}
