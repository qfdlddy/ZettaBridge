#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "zb/opensl_backend.h"

// Recording OpenSlBackend for host tests. Every object and interface is a distinct non-null
// pointer from a small fake arena, so the test can prove that HostOpenSl never confuses two
// interfaces of the same object. Every call is counted and the last arguments are kept; the test
// fires buffer callbacks explicitly, which is what the real mixer thread would do.
class MockOpenSlBackend final : public zb::OpenSlBackend {
public:
    // Results the test can force for the next call of a kind.
    std::uint32_t engine_result = kSuccess;
    std::uint32_t mix_result = kSuccess;
    std::uint32_t player_result = kSuccess;
    std::uint32_t realize_result = kSuccess;
    std::uint32_t get_interface_result = kSuccess;
    std::uint32_t enqueue_result = kSuccess;
    std::uint32_t stream_type_result = kSuccess;
    std::uint32_t play_state_result = kSuccess;
    std::uint32_t register_callback_result = kSuccess;

    // Observed calls.
    unsigned create_engine_calls = 0;
    unsigned create_output_mix_calls = 0;
    unsigned create_player_calls = 0;
    unsigned realize_calls = 0;
    unsigned get_state_calls = 0;
    unsigned destroy_calls = 0;
    unsigned get_interface_calls = 0;
    unsigned set_stream_type_calls = 0;
    unsigned set_play_state_calls = 0;
    unsigned enqueue_calls = 0;
    unsigned clear_calls = 0;

    zb::OpenSlBackend::PcmFormat last_format;
    std::uint32_t last_num_buffers = 0;
    void* last_engine_interface = nullptr;
    void* last_output_mix = nullptr;
    void* last_object_realized = nullptr;
    bool last_realize_async = true;
    std::int32_t last_stream_type = -1;
    std::uint32_t last_play_state = 0;
    std::uint32_t last_state_written = 0;
    std::vector<std::vector<std::uint8_t>> enqueued;

    bool available() override { return true; }

    std::uint32_t create_engine(void** object) override {
        ++create_engine_calls;
        *object = engine_result == kSuccess ? allocate() : nullptr;
        return engine_result;
    }

    std::uint32_t create_output_mix(void* engine_interface, void** object) override {
        ++create_output_mix_calls;
        last_engine_interface = engine_interface;
        *object = mix_result == kSuccess ? allocate() : nullptr;
        return mix_result;
    }

    std::uint32_t create_player(void* engine_interface, void* output_mix,
                                const PcmFormat& format, std::uint32_t num_buffers,
                                void** object) override {
        ++create_player_calls;
        last_engine_interface = engine_interface;
        last_output_mix = output_mix;
        last_format = format;
        last_num_buffers = num_buffers;
        *object = player_result == kSuccess ? allocate() : nullptr;
        return player_result;
    }

    std::uint32_t realize(void* object, bool async) override {
        ++realize_calls;
        last_object_realized = object;
        last_realize_async = async;
        return realize_result;
    }

    std::uint32_t get_state(void* object, std::uint32_t& state) override {
        (void)object;
        ++get_state_calls;
        state = 2;  // SL_OBJECT_STATE_REALIZED
        return kSuccess;
    }

    void destroy(void* object) override {
        ++destroy_calls;
        destroyed.push_back(object);
    }

    std::uint32_t get_interface(void* object, Interface interface, void** real) override {
        ++get_interface_calls;
        last_interface_object = object;
        last_interface = interface;
        if (get_interface_result != kSuccess) {
            *real = nullptr;
            return get_interface_result;
        }
        const auto key = std::make_pair(object, static_cast<std::uint32_t>(interface));
        auto found = interfaces_.find(key);
        if (found == interfaces_.end()) {
            found = interfaces_.emplace(key, allocate()).first;
        }
        *real = found->second;
        last_interface_real = found->second;
        return kSuccess;
    }

    std::uint32_t set_stream_type(void* config_interface, std::int32_t stream_type) override {
        ++set_stream_type_calls;
        last_config_interface = config_interface;
        last_stream_type = stream_type;
        return stream_type_result;
    }

    std::uint32_t set_play_state(void* play_interface, std::uint32_t state) override {
        ++set_play_state_calls;
        last_play_interface = play_interface;
        last_play_state = state;
        return play_state_result;
    }

    std::uint32_t get_play_state(void* play_interface, std::uint32_t& state) override {
        last_play_interface = play_interface;
        state = last_play_state;
        return play_state_result;
    }

    std::uint32_t enqueue(void* queue_interface, const void* data, std::uint32_t size) override {
        ++enqueue_calls;
        last_queue_interface = queue_interface;
        last_enqueue_size = size;
        if (enqueue_result == kSuccess) {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            enqueued.emplace_back(bytes, bytes + size);
        }
        return enqueue_result;
    }

    std::uint32_t clear(void* queue_interface) override {
        ++clear_calls;
        last_queue_interface = queue_interface;
        return kSuccess;
    }

    std::uint32_t set_buffer_callback(void* queue_interface, BufferCallback callback,
                                      void* context) override {
        last_queue_interface = queue_interface;
        callback_ = callback;
        callback_context_ = context;
        return register_callback_result;
    }

    // What the real mixer thread does when it has consumed one buffer.
    void fire_callback() {
        if (callback_ != nullptr) callback_(callback_context_);
    }

    bool has_callback() const { return callback_ != nullptr; }

    void* last_interface_object = nullptr;
    void* last_interface_real = nullptr;
    Interface last_interface = Interface::Object;
    void* last_config_interface = nullptr;
    void* last_play_interface = nullptr;
    void* last_queue_interface = nullptr;
    std::uint32_t last_enqueue_size = 0;
    std::vector<void*> destroyed;

private:
    struct PairHash {
        std::size_t operator()(const std::pair<void*, std::uint32_t>& value) const {
            return std::hash<void*>()(value.first) ^ (static_cast<std::size_t>(value.second) << 8);
        }
    };

    // A stable, distinct, non-dereferenceable address per object/interface: the guest only ever
    // stores these in its own structs, and the test only ever compares them.
    void* allocate() {
        const auto value = reinterpret_cast<void*>(next_++);
        return value;
    }

    std::uintptr_t next_ = 0x1000;
    std::unordered_map<std::pair<void*, std::uint32_t>, void*, PairHash> interfaces_;
    BufferCallback callback_ = nullptr;
    void* callback_context_ = nullptr;
};
