#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <SLES/OpenSLES_AndroidConfiguration.h>

#include "zb/opensl_backend.h"

namespace zb {

// The real OpenSlBackend (Phase 7c): a thin wrapper over the platform's own 64-bit
// libOpenSLES.so through the NDK <SLES/OpenSLES.h>. HostOpenSl owns the guest-side object model;
// the pointers here are the real SLObjectItf/SLEngineItf/... values and never reach the guest.
//
// The guest's arm32 libOpenSLES.so is a stub in the guest address space; this is a different
// library in the arm64 process, so both can coexist.
class AndroidOpenSlBackend final : public OpenSlBackend {
public:
    bool available() override;
    std::uint32_t create_engine(void** object) override;
    std::uint32_t create_output_mix(void* engine_interface, void** object) override;
    std::uint32_t create_player(void* engine_interface, void* output_mix, const PcmFormat& format,
                                std::uint32_t num_buffers, void** object) override;
    std::uint32_t realize(void* object, bool async) override;
    std::uint32_t get_state(void* object, std::uint32_t& state) override;
    void destroy(void* object) override;
    std::uint32_t get_interface(void* object, Interface interface, void** real) override;
    std::uint32_t set_stream_type(void* config_interface, std::int32_t stream_type) override;
    std::uint32_t set_play_state(void* play_interface, std::uint32_t state) override;
    std::uint32_t get_play_state(void* play_interface, std::uint32_t& state) override;
    std::uint32_t enqueue(void* queue_interface, const void* data, std::uint32_t size) override;
    std::uint32_t clear(void* queue_interface) override;
    std::uint32_t get_queue_state(void* queue_interface, std::uint32_t& count,
                                  std::uint32_t& index) override;
    std::uint32_t set_buffer_callback(void* queue_interface, BufferCallback callback,
                                      void* context) override;

private:
    // SLAndroidSimpleBufferQueueItf's callback signature carries the queue as its first
    // argument; the seam's does not. One bridge per registration translates between them.
    struct CallbackBridge {
        BufferCallback callback = nullptr;
        void* context = nullptr;
    };
    static void buffer_trampoline(SLAndroidSimpleBufferQueueItf queue, void* context);

    std::mutex mutex_;
    std::unordered_map<void*, std::unique_ptr<CallbackBridge>> bridges_;
    // Superseded bridges stay alive for the process lifetime: a callback can already be running
    // on OpenSL's mixer thread when a registration is replaced.
    std::vector<std::unique_ptr<CallbackBridge>> retired_;
};

}  // namespace zb
