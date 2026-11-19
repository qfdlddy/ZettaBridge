#include "opensl_driver_backend.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

namespace zb {

namespace {

SLObjectItf as_object(void* value) { return reinterpret_cast<SLObjectItf>(value); }
SLEngineItf as_engine(void* value) { return reinterpret_cast<SLEngineItf>(value); }
SLPlayItf as_play(void* value) { return reinterpret_cast<SLPlayItf>(value); }
SLAndroidSimpleBufferQueueItf as_queue(void* value) {
    return reinterpret_cast<SLAndroidSimpleBufferQueueItf>(value);
}
SLAndroidConfigurationItf as_config(void* value) {
    return reinterpret_cast<SLAndroidConfigurationItf>(value);
}

}  // namespace

bool AndroidOpenSlBackend::available() { return true; }

std::uint32_t AndroidOpenSlBackend::create_engine(void** object) {
    SLObjectItf engine = nullptr;
    const SLresult result = slCreateEngine(&engine, 0, nullptr, 0, nullptr, nullptr);
    *object = const_cast<void*>(reinterpret_cast<const void*>(engine));
    return result;
}

std::uint32_t AndroidOpenSlBackend::create_output_mix(void* engine_interface, void** object) {
    const SLEngineItf engine = as_engine(engine_interface);
    if (engine == nullptr) return SL_RESULT_PARAMETER_INVALID;
    SLObjectItf mix = nullptr;
    const SLresult result = (*engine)->CreateOutputMix(engine, &mix, 0, nullptr, nullptr);
    *object = const_cast<void*>(reinterpret_cast<const void*>(mix));
    return result;
}

std::uint32_t AndroidOpenSlBackend::create_player(void* engine_interface, void* output_mix,
                                                  const PcmFormat& format,
                                                  std::uint32_t num_buffers, void** object) {
    const SLEngineItf engine = as_engine(engine_interface);
    const SLObjectItf mix = as_object(output_mix);
    if (engine == nullptr || mix == nullptr || num_buffers == 0) return SL_RESULT_PARAMETER_INVALID;

    SLDataLocator_AndroidSimpleBufferQueue buffer_queue = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
                                                           num_buffers};
    SLDataFormat_PCM pcm = {SL_DATAFORMAT_PCM,
                            format.num_channels,
                            format.samples_per_sec,
                            format.bits_per_sample,
                            format.container_size,
                            format.channel_mask,
                            format.endianness};
    SLDataSource source = {&buffer_queue, &pcm};

    SLDataLocator_OutputMix output = {SL_DATALOCATOR_OUTPUTMIX, mix};
    SLDataSink sink = {&output, nullptr};

    const SLInterfaceID ids[2] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION};
    const SLboolean required[2] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};

    SLObjectItf player = nullptr;
    const SLresult result =
        (*engine)->CreateAudioPlayer(engine, &player, &source, &sink, 2, ids, required);
    *object = const_cast<void*>(reinterpret_cast<const void*>(player));
    return result;
}

std::uint32_t AndroidOpenSlBackend::realize(void* object, bool async) {
    const SLObjectItf itf = as_object(object);
    if (itf == nullptr) return SL_RESULT_PARAMETER_INVALID;
    return (*itf)->Realize(itf, async ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE);
}

std::uint32_t AndroidOpenSlBackend::get_state(void* object, std::uint32_t& state) {
    const SLObjectItf itf = as_object(object);
    if (itf == nullptr) return SL_RESULT_PARAMETER_INVALID;
    SLuint32 value = 0;
    const SLresult result = (*itf)->GetState(itf, &value);
    state = value;
    return result;
}

void AndroidOpenSlBackend::destroy(void* object) {
    const SLObjectItf itf = as_object(object);
    if (itf == nullptr) return;
    (*itf)->Destroy(itf);
}

std::uint32_t AndroidOpenSlBackend::get_interface(void* object, Interface interface, void** real) {
    const SLObjectItf itf = as_object(object);
    if (itf == nullptr) {
        *real = nullptr;
        return SL_RESULT_PARAMETER_INVALID;
    }
    if (interface == Interface::Object) {
        // SL_IID_OBJECT is the object's own interface: the object pointer itself.
        *real = object;
        return SL_RESULT_SUCCESS;
    }
    SLInterfaceID id = nullptr;
    switch (interface) {
    case Interface::Engine:
        id = SL_IID_ENGINE;
        break;
    case Interface::Play:
        id = SL_IID_PLAY;
        break;
    case Interface::Record:
        id = SL_IID_RECORD;
        break;
    case Interface::OutputMix:
        id = SL_IID_OUTPUTMIX;
        break;
    case Interface::Volume:
        id = SL_IID_VOLUME;
        break;
    case Interface::BufferQueue:
        id = SL_IID_BUFFERQUEUE;
        break;
    case Interface::AndroidSimpleBufferQueue:
        id = SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
        break;
    case Interface::AndroidConfiguration:
        id = SL_IID_ANDROIDCONFIGURATION;
        break;
    case Interface::Object:
    default:
        *real = nullptr;
        return SL_RESULT_FEATURE_UNSUPPORTED;
    }
    void* value = nullptr;
    const SLresult result = (*itf)->GetInterface(itf, id, &value);
    *real = result == SL_RESULT_SUCCESS ? value : nullptr;
    return result;
}

std::uint32_t AndroidOpenSlBackend::set_stream_type(void* config_interface,
                                                    std::int32_t stream_type) {
    const SLAndroidConfigurationItf config = as_config(config_interface);
    if (config == nullptr) return SL_RESULT_PARAMETER_INVALID;
    SLint32 value = stream_type;
    return (*config)->SetConfiguration(config, SL_ANDROID_KEY_STREAM_TYPE, &value, sizeof(value));
}

std::uint32_t AndroidOpenSlBackend::set_play_state(void* play_interface, std::uint32_t state) {
    const SLPlayItf play = as_play(play_interface);
    if (play == nullptr) return SL_RESULT_PARAMETER_INVALID;
    return (*play)->SetPlayState(play, state);
}

std::uint32_t AndroidOpenSlBackend::get_play_state(void* play_interface, std::uint32_t& state) {
    const SLPlayItf play = as_play(play_interface);
    if (play == nullptr) return SL_RESULT_PARAMETER_INVALID;
    SLuint32 value = 0;
    const SLresult result = (*play)->GetPlayState(play, &value);
    state = value;
    return result;
}

std::uint32_t AndroidOpenSlBackend::enqueue(void* queue_interface, const void* data,
                                            std::uint32_t size) {
    const SLAndroidSimpleBufferQueueItf queue = as_queue(queue_interface);
    if (queue == nullptr) return SL_RESULT_PARAMETER_INVALID;
    return (*queue)->Enqueue(queue, data, size);
}

std::uint32_t AndroidOpenSlBackend::clear(void* queue_interface) {
    const SLAndroidSimpleBufferQueueItf queue = as_queue(queue_interface);
    if (queue == nullptr) return SL_RESULT_PARAMETER_INVALID;
    return (*queue)->Clear(queue);
}

std::uint32_t AndroidOpenSlBackend::get_queue_state(void* queue_interface, std::uint32_t& count,
                                                    std::uint32_t& index) {
    const SLAndroidSimpleBufferQueueItf queue = as_queue(queue_interface);
    if (queue == nullptr) return SL_RESULT_PARAMETER_INVALID;
    SLAndroidSimpleBufferQueueState state{};
    const SLresult result = (*queue)->GetState(queue, &state);
    count = state.count;
    index = state.index;
    return result;
}

std::uint32_t AndroidOpenSlBackend::set_buffer_callback(void* queue_interface,
                                                        BufferCallback callback, void* context) {
    const SLAndroidSimpleBufferQueueItf queue = as_queue(queue_interface);
    if (queue == nullptr) return SL_RESULT_PARAMETER_INVALID;

    if (callback == nullptr) {
        const SLresult rc = (*queue)->RegisterCallback(queue, nullptr, nullptr);
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = bridges_.find(queue_interface);
        if (existing != bridges_.end()) {
            retired_.push_back(std::move(existing->second));
            bridges_.erase(existing);
        }
        return rc;
    }

    auto bridge = std::make_unique<CallbackBridge>();
    bridge->callback = callback;
    bridge->context = context;
    CallbackBridge* raw = bridge.get();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = bridges_.find(queue_interface);
        if (existing != bridges_.end()) {
            retired_.push_back(std::move(existing->second));
            bridges_.erase(existing);
        }
        bridges_[queue_interface] = std::move(bridge);
    }
    // Register outside mutex_ so a callback that re-enters RegisterCallback cannot deadlock.
    return (*queue)->RegisterCallback(queue, &AndroidOpenSlBackend::buffer_trampoline, raw);
}

void AndroidOpenSlBackend::buffer_trampoline(SLAndroidSimpleBufferQueueItf queue, void* context) {
    (void)queue;
    auto* bridge = static_cast<CallbackBridge*>(context);
    if (bridge == nullptr || bridge->callback == nullptr) return;
    bridge->callback(bridge->context);
}

}  // namespace zb
