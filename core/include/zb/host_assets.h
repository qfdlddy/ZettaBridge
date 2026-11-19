#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "zb/asset_backend.h"
#include "zb/guest_thread.h"
#include "zb/jni_handles.h"
#include "zb/library_runtime.h"

namespace zb {

class HostJni;

// AAsset* host-call dispatcher (Phase 5 Task 7): the six functions liblime.so imports
// (core/include/zb/asset_hostcalls.h). Chained into LibraryRuntime::set_host_call_handler
// alongside HostGl and HostJni, the same way GuestJniEngine wires them
// (core/src/jni/proxy_runtime.cpp). AAssetManager* and AAsset* never cross into the guest as host
// pointers: they are 32-bit handles into handle tables owned here.
class HostAssets {
public:
    HostAssets(LibraryRuntime& runtime, AssetBackend& backend, HostJni& host_jni)
        : runtime_(runtime), backend_(backend), host_jni_(host_jni) {}
    HostAssets(const HostAssets&) = delete;
    HostAssets& operator=(const HostAssets&) = delete;

    // Serves ZB_ASSET_HC_* indices; returns false for any other index (including the other,
    // unimplemented AAsset* stubs in the 142-160 range).
    bool handle_host_call(std::uint32_t index, GuestThread& thread);

    // A guest handle for an AAssetManager the platform handed us directly (the one inside the
    // framework's ANativeActivity), so the guest activity can carry it like any other.
    std::uint32_t handle_for_manager(std::uint64_t manager) {
        return manager == 0 ? 0 : managers_.add(manager);
    }

    // The host AAssetManager behind a guest handle, or 0. HostConfiguration needs it for
    // AConfiguration_fromAssetManager, which takes the manager the guest already holds.
    std::uint64_t manager_for(std::uint32_t handle) const { return require_manager(handle); }

private:
    std::uint64_t require_manager(std::uint32_t handle) const;
    std::uint64_t require_asset(std::uint32_t handle) const;

    // Asset loading is otherwise invisible: the guest reads its data files through here, and a
    // failed or short open is what leaves a game on a black screen with no error to look at. The
    // first kMaxLoggedOpens requests are recorded by name, the running totals always.
    static constexpr std::uint64_t kMaxLoggedOpens = 20;
    void note_open(const std::string& name, std::int32_t mode, std::uint64_t asset,
                   std::int64_t length);
    void note_read(std::int64_t result, std::uint32_t requested);

    LibraryRuntime& runtime_;
    AssetBackend& backend_;
    HostJni& host_jni_;
    std::uint64_t opens_ = 0;
    std::uint64_t open_failures_ = 0;
    std::uint64_t reads_ = 0;
    std::uint64_t read_bytes_ = 0;
    std::uint64_t read_errors_ = 0;
    std::uint64_t short_reads_ = 0;
    // AAsset_getBuffer copies, since the NDK buffer lives outside the guest address space. An
    // asset is read-only, so one copy per asset handle is enough and is freed on AAsset_close.
    std::uint32_t asset_buffer(std::uint32_t asset_handle, std::uint64_t asset);

    GlobalHandles managers_{HandleKind::Global};
    GlobalHandles assets_{HandleKind::Global};
    mutable std::mutex mutex_;
    bool logged_overflow_ = false;
    static constexpr std::uint64_t kMaxBufferedBytes = 64ull << 20;
    struct BufferedAsset {
        std::uint32_t guest_addr = 0;
        std::uint64_t size = 0;
    };
    std::unordered_map<std::uint32_t, BufferedAsset> asset_buffers_;
    std::uint64_t buffered_bytes_ = 0;
};

}  // namespace zb
