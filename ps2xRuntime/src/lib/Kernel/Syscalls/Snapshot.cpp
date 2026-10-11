#include "Common.h"
#include "ps2_snapshot_hooks.h"

// Snapshot hooks for the syscall layer's shared state (Helpers/State.h); DECI2 sessions
// and VAG accumulators are file-local and saved by Deci2.cpp / FileIO.cpp.
namespace ps2_stubs::snapshot
{
    std::function<void(const R5900Context &, R5900Context &)> makeRpcClientComplete(PS2Runtime &runtime,
                                                                                   const GuestInvocationCompleteTag &tag)
    {
        PS2Runtime *rt = &runtime;
        const uint32_t clientAddress = static_cast<uint32_t>(tag.arg);
        const uint32_t callSequence = static_cast<uint32_t>(tag.arg >> 32u);
        const uint32_t semaphore = static_cast<uint32_t>(tag.arg2);
        const uint32_t signals = static_cast<uint32_t>(tag.arg2 >> 32u);
        return [rt, clientAddress, callSequence, semaphore, signals](const R5900Context &, R5900Context &)
        {
            for (uint32_t i = 0u; i < signals && semaphore != 0u && semaphore <= 0xFFFFu; ++i)
                (void)rt->eeScheduler().signalSemaphore(static_cast<int>(semaphore), true);
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            const auto client = g_rpc_clients.find(clientAddress);
            // A newer call on the same client (started after this one was sent) keeps it busy.
            if (client != g_rpc_clients.end() && client->second.callSequence == callSequence)
                client->second.busy = false;
        };
    }
}

namespace ps2_syscalls::snapshot
{
    void saveDeci2(ps2x::StateWriter &w);
    void loadDeci2(ps2x::StateReader &r);
    void saveVagAccumulators(ps2x::StateWriter &w);
    void loadVagAccumulators(ps2x::StateReader &r);

    void save(ps2x::StateWriter &w)
    {
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            w.podMap(g_rpc_servers);
            w.podMap(g_rpc_clients);
            w.boolean(g_rpc_initialized);
            w.u32(g_rpc_next_id);
            w.u32(g_rpc_packet_index);
            w.u32(g_rpc_server_index);
            w.u32(g_rpc_active_queue);
        }
        {
            std::lock_guard<std::mutex> lock(g_bootmode_mutex);
            w.boolean(g_bootmode_initialized);
            w.u32(g_bootmode_pool_offset);
            w.podMap(g_bootmode_addresses);
        }
        {
            std::lock_guard<std::mutex> lock(g_tls_mutex);
            w.u32(g_tls_index);
        }
        {
            std::lock_guard<std::mutex> lock(g_osd_mutex);
            w.boolean(g_osd_config_initialized);
            w.u32(g_osd_config_raw);
            w.u32(g_osd_config2_raw);
        }
        {
            std::lock_guard<std::mutex> lock(g_ps2_path_mutex);
            w.boolean(g_ps2_paths_initialized);
            w.str(g_host_base.string());
            w.str(g_cdrom_base.string());
            w.str(g_host_cwd.string());
            w.str(g_cdrom_cwd.string());
            w.str(g_ps2_cwd_device);
        }
        {
            std::lock_guard<std::mutex> lock(g_sif_module_mutex);
            w.sortedMap(g_sif_modules_by_id, [&w](const SifModuleRecord &record)
                        {
                            w.i32(record.id);
                            w.str(record.path);
                            w.str(record.pathKey);
                            w.u32(record.refCount);
                            w.boolean(record.loaded); });
            w.sortedMap(g_sif_module_id_by_path, [&w](int32_t id)
                        { w.i32(id); });
            w.i32(g_next_sif_module_id);
        }
        saveDeci2(w);
        saveVagAccumulators(w);
    }

    void load(ps2x::StateReader &r, PS2Runtime &runtime)
    {
        (void)runtime;
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            r.podMap(g_rpc_servers);
            r.podMap(g_rpc_clients);
            g_rpc_initialized = r.boolean();
            g_rpc_next_id = r.u32();
            g_rpc_packet_index = r.u32();
            g_rpc_server_index = r.u32();
            g_rpc_active_queue = r.u32();
        }
        {
            std::lock_guard<std::mutex> lock(g_bootmode_mutex);
            g_bootmode_initialized = r.boolean();
            g_bootmode_pool_offset = r.u32();
            r.podMap(g_bootmode_addresses);
        }
        {
            std::lock_guard<std::mutex> lock(g_tls_mutex);
            g_tls_index = r.u32();
        }
        {
            std::lock_guard<std::mutex> lock(g_osd_mutex);
            g_osd_config_initialized = r.boolean();
            g_osd_config_raw = r.u32();
            g_osd_config2_raw = r.u32();
        }
        {
            std::lock_guard<std::mutex> lock(g_ps2_path_mutex);
            g_ps2_paths_initialized = r.boolean();
            g_host_base = std::filesystem::path(r.str());
            g_cdrom_base = std::filesystem::path(r.str());
            g_host_cwd = std::filesystem::path(r.str());
            g_cdrom_cwd = std::filesystem::path(r.str());
            g_ps2_cwd_device = r.str();
        }
        {
            std::lock_guard<std::mutex> lock(g_sif_module_mutex);
            r.sortedMap(g_sif_modules_by_id, [&r](SifModuleRecord &record)
                        {
                            record.id = r.i32();
                            record.path = r.str();
                            record.pathKey = r.str();
                            record.refCount = r.u32();
                            record.loaded = r.boolean(); });
            r.sortedMap(g_sif_module_id_by_path, [&r](int32_t &id)
                        { id = r.i32(); });
            g_next_sif_module_id = r.i32();
        }
        loadDeci2(r);
        loadVagAccumulators(r);
    }
}
