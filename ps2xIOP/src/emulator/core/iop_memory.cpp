#include "iop_memory.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2x::iop::detail
{
    namespace
    {
        // IOP DMA channel 4 (SPU2 core 0) and 7 (core 1): MADR, BCR, CHCR.
        constexpr uint32_t kDmaSpu0Madr = 0x1F8010C0u;
        constexpr uint32_t kDmaSpu0Chcr = 0x1F8010C8u;
        constexpr uint32_t kDmaSpu1Madr = 0x1F801500u;
        constexpr uint32_t kDmaSpu1Chcr = 0x1F801508u;
        constexpr uint32_t kDmaStart = 1u << 24u;
        constexpr uint32_t kDmaToDevice = 1u;

        // PS2X_IOP_SPU_LOG=1 prints each SPU2 register halfword the IOP reads, the first
        // time and then at every power-of-two read count, on stderr as [iop:spu-read].
        bool envOn(const char *name)
        {
            const char *value = std::getenv(name);
            return value && *value && *value != '0';
        }

        uint16_t debugSpu2Read(uint32_t address, uint16_t value)
        {
            static const bool s_log = envOn("PS2X_IOP_SPU_LOG");
            if (s_log)
            {
                static std::atomic<uint32_t> s_counts[0x800 / 2]{};
                const uint32_t slot = (address & 0x7FFu) >> 1u;
                const uint32_t n = s_counts[slot].fetch_add(1u, std::memory_order_relaxed) + 1u;
                if ((n & (n - 1u)) == 0u)
                    std::fprintf(stderr, "[iop:spu-read] 0x%08X = 0x%04X reads=%u\n", address, value, n);
            }
            return value;
        }

        uint32_t alignUp(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }
    }

    IopMemory::IopMemory()
        : m_ram(RamSize), m_owned(RamSize), m_scratch(ScratchSize)
    {
        m_spu2.bind(nullptr, m_ram.data(), RamSize);
        reset();
    }

    void IopMemory::bindClock(const uint64_t *clock) noexcept
    {
        m_spu2.bind(clock, m_ram.data(), RamSize);
    }

    void IopMemory::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint8_t{0});
        std::fill(m_owned.begin(), m_owned.end(), uint8_t{0});
        std::fill(m_scratch.begin(), m_scratch.end(), uint8_t{0});
        m_hardware.clear();
        m_allocations.clear();
        m_heapCursor = HeapBase;
        m_interruptStatus = 0;
        m_interruptMask = 0;
        m_interruptControl = 1;
        m_spu2.reset();
    }

    uint32_t IopMemory::physicalAddress(uint32_t address) noexcept
    {
        return address & 0x1FFFFFFFu;
    }

    uint8_t IopMemory::read8(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
            return m_ram[phys];
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
            return m_scratch[phys - ScratchBase];
        if (isSpu2Register(phys))
            return static_cast<uint8_t>(read16(phys & ~1u) >> ((phys & 1u) * 8u));
        const uint32_t value = readHardware32(phys & ~3u);
        return static_cast<uint8_t>(value >> ((phys & 3u) * 8u));
    }

    uint16_t IopMemory::read16(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            uint16_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if (isSpu2Register(phys))
            return debugSpu2Read(phys, m_spu2.readRegister(phys - Spu2Base));
        return static_cast<uint16_t>(read8(address) | (static_cast<uint16_t>(read8(address + 1u)) << 8u));
    }

    uint32_t IopMemory::read32(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            uint32_t value;
            std::memcpy(&value, m_ram.data() + phys, sizeof(value));
            return value;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            uint32_t value;
            std::memcpy(&value, m_scratch.data() + (phys - ScratchBase), sizeof(value));
            return value;
        }
        if (isSpu2Register(phys))
            return static_cast<uint32_t>(read16(phys & ~1u)) | (static_cast<uint32_t>(read16((phys & ~1u) + 2u)) << 16u);
        if ((phys & 3u) == 0u && isHardwareAddress(phys))
            return readHardware32(phys);

        return static_cast<uint32_t>(read8(address)) |
               (static_cast<uint32_t>(read8(address + 1u)) << 8u) |
               (static_cast<uint32_t>(read8(address + 2u)) << 16u) |
               (static_cast<uint32_t>(read8(address + 3u)) << 24u);
    }

    void IopMemory::write8(uint32_t address, uint8_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys < RamSize)
        {
            m_ram[phys] = value;
            markOwned(phys, sizeof(value));
            return;
        }
        if (phys >= ScratchBase && phys < ScratchBase + ScratchSize)
        {
            m_scratch[phys - ScratchBase] = value;
            return;
        }
        if (isSpu2Register(phys))
        {
            const uint32_t half = phys & ~1u;
            const uint32_t shift = (phys & 1u) * 8u;
            const uint16_t current = m_spu2.readRegister(half - Spu2Base);
            m_spu2.writeRegister(half - Spu2Base, static_cast<uint16_t>((current & ~(0xFFu << shift)) | (value << shift)));
            return;
        }
        const uint32_t aligned = phys & ~3u;
        uint32_t current = readHardware32(aligned);
        const uint32_t shift = (phys & 3u) * 8u;
        current = (current & ~(0xFFu << shift)) | (static_cast<uint32_t>(value) << shift);
        writeHardware32(aligned, current);
    }

    void IopMemory::write16(uint32_t address, uint16_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys + 1u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if (isSpu2Register(phys) && (phys & 1u) == 0u)
        {
            m_spu2.writeRegister(phys - Spu2Base, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
    }

    void IopMemory::write32(uint32_t address, uint32_t value)
    {
        const uint32_t phys = physicalAddress(address);
        if ((phys & 3u) == 0u && phys + 3u < RamSize)
        {
            std::memcpy(m_ram.data() + phys, &value, sizeof(value));
            markOwned(phys, sizeof(value));
            return;
        }
        if ((phys & 3u) == 0u && phys >= ScratchBase && phys + 3u < ScratchBase + ScratchSize)
        {
            std::memcpy(m_scratch.data() + (phys - ScratchBase), &value, sizeof(value));
            return;
        }
        if (isSpu2Register(phys) && (phys & 1u) == 0u)
        {
            m_spu2.writeRegister(phys - Spu2Base, static_cast<uint16_t>(value));
            m_spu2.writeRegister(phys + 2u - Spu2Base, static_cast<uint16_t>(value >> 16u));
            return;
        }
        if ((phys & 3u) == 0u)
        {
            writeHardware32(phys, value);
            return;
        }
        write8(address, static_cast<uint8_t>(value));
        write8(address + 1u, static_cast<uint8_t>(value >> 8u));
        write8(address + 2u, static_cast<uint8_t>(value >> 16u));
        write8(address + 3u, static_cast<uint8_t>(value >> 24u));
    }

    bool IopMemory::readRam(uint32_t address, void *destination, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if ((!destination && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
            std::memcpy(destination, m_ram.data() + phys, size);
        return true;
    }

    bool IopMemory::writeRam(uint32_t address, const void *source, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if ((!source && size != 0u) || phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memcpy(m_ram.data() + phys, source, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::zeroRam(uint32_t address, size_t size)
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        if (size != 0u)
        {
            std::memset(m_ram.data() + phys, 0, size);
            markOwned(phys, size);
        }
        return true;
    }

    bool IopMemory::ownsRamRange(uint32_t address, size_t size) const
    {
        const uint32_t phys = physicalAddress(address);
        if (phys > RamSize || size > RamSize - phys)
            return false;
        return std::all_of(m_owned.begin() + phys, m_owned.begin() + phys + size,
                           [](uint8_t value)
                           { return value != 0u; });
    }

    void IopMemory::markOwned(uint32_t address, size_t size)
    {
        if (address > RamSize || size > RamSize - address)
            return;
        std::fill(m_owned.begin() + address, m_owned.begin() + address + size, uint8_t{1});
    }

    bool IopMemory::isHardwareAddress(uint32_t address) const
    {
        const uint32_t phys = physicalAddress(address);
        return (phys >= HardwareBase && phys < HardwareEnd) ||
               (phys >= Spu2Base && phys < Spu2End) ||
               (phys >= SifBase && phys < SifEnd);
    }

    uint32_t IopMemory::readHardware32(uint32_t address) const
    {
        const auto value = m_hardware.find(address);
        if (address == kDmaSpu0Chcr || address == kDmaSpu1Chcr)
        {
            const uint32_t stored = value != m_hardware.end() ? value->second : 0u;
            const bool busy = m_spu2.dmaActive(address == kDmaSpu1Chcr ? 1u : 0u);
            return busy ? (stored | kDmaStart) : (stored & ~kDmaStart);
        }
        if (value != m_hardware.end())
            return value->second;
        switch (address)
        {
        case 0x1F801070u:
            return m_interruptStatus;
        case 0x1F801074u:
            return m_interruptMask;
        case 0x1F801078u:
            return m_interruptControl;
        default:
            return 0u;
        }
    }

    void IopMemory::writeHardware32(uint32_t address, uint32_t value)
    {
        switch (address)
        {
        case 0x1F801070u:
            m_interruptStatus &= value;
            return;
        case 0x1F801074u:
            m_interruptMask = value;
            return;
        case 0x1F801078u:
            m_interruptControl = value & 1u;
            return;
        default:
            break;
        }

        m_hardware[address] = value;
        if ((address != kDmaSpu0Chcr && address != kDmaSpu1Chcr) || (value & kDmaStart) == 0u)
            return;

        // CHCR start bit: hand the transfer to the SPU2 model, which completes it on the
        // sample clock and raises the channel's DMA interrupt (0x24 / 0x28).
        const uint32_t core = address == kDmaSpu1Chcr ? 1u : 0u;
        m_hardware[address] = value & ~kDmaStart;
        const uint32_t madrAddress = core ? kDmaSpu1Madr : kDmaSpu0Madr;
        uint32_t madr = 0u;
        if (const auto current = m_hardware.find(madrAddress); current != m_hardware.end())
            madr = current->second;
        uint32_t blockControl = 0u;
        if (const auto current = m_hardware.find(madrAddress + 4u); current != m_hardware.end())
            blockControl = current->second;
        const uint32_t wordsPerBlock = std::max<uint32_t>(blockControl & 0xFFFFu, 1u);
        const uint32_t blockCount = std::max<uint32_t>(blockControl >> 16u, 1u);
        const uint64_t bytes = static_cast<uint64_t>(wordsPerBlock) * blockCount * 4u;
        m_spu2.startDma(core, (value & kDmaToDevice) != 0u, physicalAddress(madr),
                        static_cast<uint32_t>(std::min<uint64_t>(bytes, Spu2::RamBytes)));
    }

    uint32_t IopMemory::allocate(uint32_t size, uint32_t alignment, std::optional<uint32_t> fixed)
    {
        size = alignUp(std::max(size, 1u), 16u);
        alignment = std::max<uint32_t>(alignment, 4u);
        if (fixed)
        {
            const uint32_t address = *fixed;
            if (address < HeapBase || address + size > HeapLimit)
                return 0u;
            for (const auto &block : m_allocations)
                if (address < block.address + block.size && block.address < address + size)
                    return 0u;
            m_allocations.push_back({address, size});
            markOwned(address, size);
            return address;
        }

        // First fit from the bottom of the heap, so freed blocks are reused: drivers that
        // allocate a work buffer per request (sector staging buffers) would otherwise walk a
        // bump cursor to the top of the heap and then fail every allocation.
        std::sort(m_allocations.begin(), m_allocations.end(),
                  [](const Allocation &a, const Allocation &b)
                  { return a.address < b.address; });
        uint32_t candidate = alignUp(HeapBase, alignment);
        for (const auto &block : m_allocations)
        {
            if (candidate + size <= block.address)
                break;
            candidate = std::max(candidate, alignUp(block.address + block.size, alignment));
        }
        if (candidate > HeapLimit || size > HeapLimit - candidate)
            return 0u;
        m_allocations.push_back({candidate, size});
        markOwned(candidate, size);
        m_heapCursor = std::max(m_heapCursor, candidate + size);
        return candidate;
    }

    bool IopMemory::freeAllocation(uint32_t address)
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        { return candidate.address == address; });
        if (block == m_allocations.end())
            return false;
        std::fill(m_owned.begin() + block->address,
                  m_owned.begin() + block->address + block->size,
                  uint8_t{0});
        m_allocations.erase(block);
        return true;
    }

    uint32_t IopMemory::maxFreeMemory() const
    {
        // The largest gap between allocations (QueryMaxFreeMemSize).
        std::vector<Allocation> blocks = m_allocations;
        std::sort(blocks.begin(), blocks.end(),
                  [](const Allocation &a, const Allocation &b)
                  { return a.address < b.address; });
        uint32_t largest = 0u;
        uint32_t cursor = HeapBase;
        for (const auto &block : blocks)
        {
            if (block.address > cursor)
                largest = std::max(largest, block.address - cursor);
            cursor = std::max(cursor, block.address + block.size);
        }
        if (cursor < HeapLimit)
            largest = std::max(largest, HeapLimit - cursor);
        return largest;
    }

    bool IopMemory::hasAllocationIn(uint32_t begin, uint32_t end) const
    {
        return std::any_of(m_allocations.begin(), m_allocations.end(),
                           [&](const Allocation &block)
                           { return block.address >= begin && block.address < end; });
    }

    std::optional<IopMemory::Allocation> IopMemory::allocationContaining(uint32_t address) const
    {
        const auto block = std::find_if(m_allocations.begin(), m_allocations.end(),
                                        [&](const Allocation &candidate)
                                        {
                                            return address >= candidate.address &&
                                                   address < candidate.address + candidate.size;
                                        });
        if (block == m_allocations.end())
            return std::nullopt;
        return *block;
    }

    std::string IopMemory::readString(uint32_t address, size_t limit) const
    {
        std::string result;
        result.reserve(std::min<size_t>(limit, 64u));
        for (size_t i = 0; i < limit; ++i)
        {
            const char ch = static_cast<char>(read8(address + static_cast<uint32_t>(i)));
            if (ch == '\0')
                break;
            result.push_back(ch);
        }
        return result;
    }
}

namespace ps2x::iop::detail
{
    void IopMemory::saveState(ps2x::StateWriter &w) const
    {
        w.podVector(m_ram);
        w.podVector(m_owned);
        w.podVector(m_scratch);
        w.podMap(m_hardware);
        w.podVector(m_allocations);
        w.u32(m_heapCursor);
        w.u32(m_interruptStatus);
        w.u32(m_interruptMask);
        w.u32(m_interruptControl);
        m_spu2.saveState(w);
    }

    void IopMemory::loadState(ps2x::StateReader &r)
    {
        r.podVector(m_ram);
        r.podVector(m_owned);
        r.podVector(m_scratch);
        if (m_ram.size() != RamSize || m_owned.size() != RamSize || m_scratch.size() != ScratchSize)
            throw ps2x::StateError("IOP memory sizes differ from this build");
        r.podMap(m_hardware);
        r.podVector(m_allocations);
        m_heapCursor = r.u32();
        m_interruptStatus = r.u32();
        m_interruptMask = r.u32();
        m_interruptControl = r.u32();
        m_spu2.loadState(r);
    }
}
