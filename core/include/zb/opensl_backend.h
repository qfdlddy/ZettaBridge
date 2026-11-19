#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace zb {

// Portable seam behind the guest's OpenSL ES calls (Phase 7c). HostOpenSl owns the guest-side
// object model (the { vtable, handle } structs the guest dereferences, the 32-bit handles, the
// slot -> method mapping); this interface only forwards the operations that model needs to a real
// implementation. Implementations: the Android platform's own libOpenSLES.so
// (core/android/opensl_driver_backend.*) and a recording mock for host tests
// (tests/host/mock_opensl.h).
//
// Every opaque object/interface pointer here is a host pointer. It never crosses into the guest.
// Only the Android device has a real OpenSL implementation, so everything but the mock is
// Android-build-only.
//
// Deliberate restriction: only PCM playback into an output mix through a simple buffer queue is
// implemented. That is exactly the shape Unity's FMOD backend builds (evidence: the init function
// at libunity.so 0x7bf0a4 requests SL_IID_ANDROIDSIMPLEBUFFERQUEUE and
// SL_IID_ANDROIDCONFIGURATION from CreateAudioPlayer, sets the stream type through the Android
// configuration interface, realizes, then enqueues a ring of buffers). Anything else returns
// SL_RESULT_CONTENT_UNSUPPORTED, which is a legal OpenSL result and lets a guest fall back.
class OpenSlBackend {
public:
    virtual ~OpenSlBackend() = default;

    // The interface ids the guest knows. The guest stub exports its SL_IID_* symbols as pointer
    // variables holding these literals (guest/stubs/libOpenSLES.S); FMOD dereferences dlsym's
    // result, so the value that arrives here is the literal itself.
    enum class Interface : std::uint32_t {
        Object = 0,
        Engine = 1,
        Play = 2,
        Record = 3,
        OutputMix = 4,
        Volume = 5,
        BufferQueue = 6,
        AndroidSimpleBufferQueue = 7,
        AndroidConfiguration = 8,
    };

    // SLresult values used here. Everything the bridge can fail with is one of these, so a guest
    // sees a normal error instead of a crash.
    static constexpr std::uint32_t kSuccess = 0;
    static constexpr std::uint32_t kParameterInvalid = 2;
    static constexpr std::uint32_t kFeatureUnsupported = 0x0c;

    // SLDataFormat_PCM: pointer-free, so a guest struct is copied field by field.
    struct PcmFormat {
        std::uint32_t format_type = 0;   // SL_DATAFORMAT_PCM (2)
        std::uint32_t num_channels = 0;
        std::uint32_t samples_per_sec = 0;
        std::uint32_t bits_per_sample = 0;
        std::uint32_t container_size = 0;
        std::uint32_t channel_mask = 0;
        std::uint32_t endianness = 0;
    };

    virtual bool available() = 0;

    // slCreateEngine. Returns an SLObjectItf in `object`.
    virtual std::uint32_t create_engine(void** object) = 0;

    // SleEngineItf::CreateOutputMix with no explicit interfaces.
    virtual std::uint32_t create_output_mix(void* engine_interface, void** object) = 0;

    // SLEngineItf::CreateAudioPlayer for a PCM source in `num_buffers` simple-buffer-queue
    // slots, playing into `output_mix` (an object this backend created), requesting the Android
    // configuration and simple buffer queue interfaces (a plain player without them is useless
    // to the guest).
    virtual std::uint32_t create_player(void* engine_interface, void* output_mix,
                                        const PcmFormat& format,
                                        std::uint32_t num_buffers, void** object) = 0;

    // SLObjectItf::Realize / Destroy / GetState for any object the backend created.
    virtual std::uint32_t realize(void* object, bool async) = 0;
    virtual std::uint32_t get_state(void* object, std::uint32_t& state) = 0;
    virtual void destroy(void* object) = 0;

    // SLObjectItf::GetInterface. `real` receives the backend's interface pointer (for an object
    // interface this is the object itself).
    virtual std::uint32_t get_interface(void* object, Interface interface, void** real) = 0;

    // SLAndroidConfigurationItf::SetConfiguration for the playback stream type
    // (SL_ANDROID_KEY_STREAM_TYPE, an SLint32).
    virtual std::uint32_t set_stream_type(void* config_interface, std::int32_t stream_type) = 0;

    // SLPlayItf.
    virtual std::uint32_t set_play_state(void* play_interface, std::uint32_t state) = 0;
    virtual std::uint32_t get_play_state(void* play_interface, std::uint32_t& state) = 0;

    // SLAndroidSimpleBufferQueueItf. `data` is host memory the backend may read; the caller keeps
    // it alive until the buffer callback reports the buffer consumed.
    virtual std::uint32_t enqueue(void* queue_interface, const void* data, std::uint32_t size) = 0;
    virtual std::uint32_t clear(void* queue_interface) = 0;
    virtual std::uint32_t get_queue_state(void* queue_interface, std::uint32_t& count,
                                          std::uint32_t& index) {
        (void)queue_interface;
        count = 0;
        index = 0;
        return kSuccess;
    }

    // Runs `callback(context)` on the backend's own thread each time one enqueued buffer is
    // consumed. The callback must not block and must be safe on a non-guest host thread.
    using BufferCallback = void (*)(void* context);
    virtual std::uint32_t set_buffer_callback(void* queue_interface, BufferCallback callback,
                                              void* context) = 0;
};

}  // namespace zb
