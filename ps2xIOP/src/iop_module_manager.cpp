#include "iop_module_manager.h"

#include "ps2x/iop/ps2_path.h"

#include <algorithm>

namespace ps2x::iop::detail
{
    IopModuleManager::IopModuleManager()
    {
        // ROM modules that the no-BIOS HLE environment can legitimately provide. 
        // Entries with RPC services become routable only after load.
        constexpr std::string_view modules[] = {
            "sysmem",
            "loadcore",
            "intrman",
            "sifman",
            "sifcmd",
            "sifinit",
            "ioman",
            "iomanx",
            "modload",
            "stdio",
            "sysclib",
            "thbase",
            "thevent",
            "thsemap",
            "thmsgbx",
            "timrman",
            "vblank",
            "secrman",
            "sio2man",
            "xsio2man",
            "sio2d",
            "padman",
            "xpadman",
            "mcman",
            "xmcman",
            "mcserv",
            "libsd",
            "cdvdman",
            "cdvdfsv",
            "dev9",
            "usbd",
            "usbhdfsd",
            "udnl",
            "fileio",
            "poweroff",
            "netman",
            "ps2ip",
            "dbcman",
            "dbcm",
        };
        for (const std::string_view module : modules)
            m_builtinKeys.emplace(module);
    }

    void IopModuleManager::reset()
    {
        m_records.clear();
        m_hleIdsByKey.clear();
        m_loadedKeyReferences.clear();
        m_nextHleId = 0x40000000;
    }

    void IopModuleManager::setServiceModuleKeys(std::vector<std::string> keys)
    {
        m_serviceKeys.clear();
        for (std::string &key : keys)
        {
            const std::string normalized = ps2PathLeafKey(key);
            if (!normalized.empty())
                m_serviceKeys.emplace(normalized);
        }
    }

    ModuleLoadResult IopModuleManager::loadHle(std::string_view path)
    {
        ModuleLoadResult result{true, -1, -1};
        const std::string key = ps2PathLeafKey(path);
        if (key.empty() || (!m_builtinKeys.contains(key) && !m_serviceKeys.contains(key)))
            return result;

        const auto existing = m_hleIdsByKey.find(key);
        if (existing != m_hleIdsByKey.end())
        {
            Record &record = m_records[existing->second];
            ++record.references;
            addLoadedKey(key);
            result.moduleId = existing->second;
            result.startResult = 0;
            return result;
        }

        if (m_nextHleId <= 0)
            return result;
        const int32_t id = m_nextHleId++;
        m_records.emplace(id, Record{key, 1u, false});
        m_hleIdsByKey.emplace(key, id);
        addLoadedKey(key);
        result.moduleId = id;
        result.startResult = 0;
        return result;
    }

    void IopModuleManager::observePhysicalLoad(int32_t moduleId, std::string_view path)
    {
        if (moduleId <= 0)
            return;
        const std::string key = ps2PathLeafKey(path);
        if (key.empty())
            return;
        m_records[moduleId] = Record{key, 1u, true};
        addLoadedKey(key);
    }

    bool IopModuleManager::stopHle(int32_t moduleId, int32_t *result)
    {
        const auto found = m_records.find(moduleId);
        if (found == m_records.end() || found->second.physical)
            return false;

        Record &record = found->second;
        removeLoadedKey(record.key);
        if (record.references > 1u)
        {
            --record.references;
        }
        else
        {
            m_hleIdsByKey.erase(record.key);
            m_records.erase(found);
        }
        if (result)
            *result = 0;
        return true;
    }

    void IopModuleManager::observePhysicalStop(int32_t moduleId)
    {
        const auto found = m_records.find(moduleId);
        if (found == m_records.end() || !found->second.physical)
            return;
        removeLoadedKey(found->second.key);
        m_records.erase(found);
    }

    bool IopModuleManager::isLoaded(std::span<const std::string_view> aliases) const
    {
        if (aliases.empty())
            return true;
        return std::any_of(aliases.begin(), aliases.end(), [&](std::string_view alias)
                           {
                               const std::string key = ps2PathLeafKey(alias);
                               const auto found = m_loadedKeyReferences.find(key);
                               return found != m_loadedKeyReferences.end() && found->second != 0u; });
    }

    bool IopModuleManager::recognizes(std::string_view path) const
    {
        const std::string key = ps2PathLeafKey(path);
        return m_builtinKeys.contains(key) || m_serviceKeys.contains(key);
    }

    void IopModuleManager::addLoadedKey(std::string_view key)
    {
        ++m_loadedKeyReferences[std::string(key)];
    }

    void IopModuleManager::removeLoadedKey(std::string_view key)
    {
        const auto found = m_loadedKeyReferences.find(std::string(key));
        if (found == m_loadedKeyReferences.end())
            return;
        if (found->second > 1u)
            --found->second;
        else
            m_loadedKeyReferences.erase(found);
    }
}

namespace ps2x::iop::detail
{
    void IopModuleManager::saveState(ps2x::StateWriter &w) const
    {
        w.sortedMap(m_records, [&w](const Record &record)
                    {
                        w.str(record.key);
                        w.u32(record.references);
                        w.boolean(record.physical); });
        w.sortedMap(m_hleIdsByKey, [&w](int32_t id)
                    { w.i32(id); });
        w.sortedMap(m_loadedKeyReferences, [&w](uint32_t references)
                    { w.u32(references); });
        w.i32(m_nextHleId);
    }

    void IopModuleManager::loadState(ps2x::StateReader &r)
    {
        r.sortedMap(m_records, [&r](Record &record)
                    {
                        record.key = r.str();
                        record.references = r.u32();
                        record.physical = r.boolean(); });
        r.sortedMap(m_hleIdsByKey, [&r](int32_t &id)
                    { id = r.i32(); });
        r.sortedMap(m_loadedKeyReferences, [&r](uint32_t &references)
                    { references = r.u32(); });
        m_nextHleId = r.i32();
    }
}
