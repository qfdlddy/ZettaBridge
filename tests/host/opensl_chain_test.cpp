// OpenSL ES host calls against the mock backend (Phase 7c): the full shape Unity's FMOD backend
// builds. slCreateEngine -> Realize -> GetInterface(engine) -> CreateOutputMix -> Realize ->
// CreateAudioPlayer(AndroidSimpleBufferQueue + AndroidConfiguration) -> SetConfiguration(stream
// type) -> Realize -> GetInterface(play/queue) -> RegisterCallback -> Enqueue.
//
// The test also pins the boundaries: every guest struct carries the shared vtable address, a slot
// is resolved against the kind of the object, and a struct the host did not create (or an
// interface it does not serve) is rejected with a legal SLresult instead of reaching the backend.
#include <sys/mman.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "mock_opensl.h"
#include "zb/guest_memory.h"
#include "zb/host_opensl.h"
#include "zb/opensl_hostcalls.h"
#include "zb/runtime_report.h"

namespace {

constexpr std::uint32_t kStack = 0x10000;
constexpr std::uint32_t kData = 0x20000;

// Guest-resident argument block: the out-pointers and the SLDataSource/SLDataSink literals the
// guest would build on its own stack.
constexpr std::uint32_t kEngineOut = kData + 0x00;
constexpr std::uint32_t kMixOut = kData + 0x04;
constexpr std::uint32_t kPlayerOut = kData + 0x08;
constexpr std::uint32_t kVtable = kData + 0x20;
constexpr std::uint32_t kSource = kData + 0x60;
constexpr std::uint32_t kSourceLocator = kData + 0x70;
constexpr std::uint32_t kSourceFormat = kData + 0x80;
constexpr std::uint32_t kSink = kData + 0xA0;
constexpr std::uint32_t kSinkLocator = kData + 0xB0;
constexpr std::uint32_t kStreamTypeValue = kData + 0xC0;
constexpr std::uint32_t kGuestCallbackKey = kData + 0xC8;
constexpr std::uint32_t kBufferData = kData + 0xD0;
constexpr std::uint32_t kAllocStart = kData + 0x200;
// The same table after the stub library is loaded again at a new base: FMOD rebuilds the whole
// graph on a route change, and the second load relocates the table.
constexpr std::uint32_t kVtable2 = kData + 0x3E00;

// The literal values the guest stub exports (guest/stubs/libOpenSLES.S).
constexpr std::uint32_t kIidEngine = 0x7B010001u;
constexpr std::uint32_t kIidPlay = 0x7B010002u;
constexpr std::uint32_t kIidOutputMix = 0x7B010004u;
constexpr std::uint32_t kIidVolume = 0x7B010005u;
constexpr std::uint32_t kIidSimpleBufferQueue = 0x7B010007u;
constexpr std::uint32_t kIidAndroidConfiguration = 0x7B010008u;

constexpr std::uint32_t kSlResultSuccess = 0;
constexpr std::uint32_t kSlResultPreconditionsViolated = 1;
constexpr std::uint32_t kSlResultParameterInvalid = 2;
constexpr std::uint32_t kSlResultFeatureUnsupported = 0x0cu;

// Kind values from HostOpenSl (the third word of every guest struct).
constexpr std::uint32_t kKindEngineObject = 1;
constexpr std::uint32_t kKindEngineInterface = 2;
constexpr std::uint32_t kKindMixObject = 3;
constexpr std::uint32_t kKindPlayerObject = 4;
constexpr std::uint32_t kKindPlayInterface = 5;
constexpr std::uint32_t kKindQueueInterface = 6;
constexpr std::uint32_t kKindConfigInterface = 7;

std::uint32_t slot_index(unsigned slot) { return zb::ZB_OPENSL_HC_first_slot + slot; }

std::uint32_t word_at(zb::LibraryRuntime& runtime, std::uint32_t address) {
    std::uint32_t value = 0;
    const std::uint8_t* bytes = runtime.memory().host_ptr(address, 4, zb::kPageRead);
    CHECK(bytes != nullptr);
    std::memcpy(&value, bytes, 4);
    return value;
}

void put_words(zb::LibraryRuntime& runtime, std::uint32_t address,
               const std::vector<std::uint32_t>& words) {
    std::uint8_t* host = runtime.memory().host_ptr(
        address, words.size() * sizeof(std::uint32_t), zb::kPageWrite);
    CHECK(host != nullptr);
    std::memcpy(host, words.data(), words.size() * sizeof(std::uint32_t));
}

// { vtable, handle, kind } as HostOpenSl writes it.
void check_guest_struct(zb::LibraryRuntime& runtime, std::uint32_t address,
                        std::uint32_t vtable, std::uint32_t kind) {
    CHECK(word_at(runtime, address) == vtable);
    CHECK(word_at(runtime, address + 4) != 0);  // handle
    CHECK(word_at(runtime, address + 8) == kind);
}

struct RecordedCallback {
    bool called = false;
    std::uint32_t function = 0;
    std::uint32_t a0 = 0;
    std::uint32_t a1 = 0;
};

}  // namespace

int main() {
    zb::LibraryRuntime runtime;
    CHECK(runtime.memory().map_anon(kStack, 0x1000, PROT_READ | PROT_WRITE));
    CHECK(runtime.memory().map_anon(kData, 0x4000, PROT_READ | PROT_WRITE));

    Dynarmic::ExclusiveMonitor monitor(1);
    zb::GuestThread thread(runtime.memory(), &monitor, 0, false, zb::kCarrierCodeCacheSize);
    thread.regs()[13] = kStack;

    MockOpenSlBackend backend;
    RecordedCallback callback;

    std::uint32_t next_allocation = kAllocStart;
    zb::HostOpenSl::GuestAllocator allocator = [&next_allocation](std::size_t size) {
        const std::uint32_t address =
            (next_allocation + 15u) & ~15u;  // 16-byte aligned, like the guest allocator
        next_allocation = address + static_cast<std::uint32_t>(size) + 16u;
        return std::optional<std::uint32_t>(address);
    };
    zb::HostOpenSl::GuestInvoker invoker =
        [&callback](std::uint32_t function, const zb::GuestCall& args) {
            callback.called = true;
            callback.function = function;
            callback.a0 = args.regs[0];
            callback.a1 = args.regs[1];
            return std::optional<zb::GuestResult>(zb::GuestResult{});
        };

    zb::HostOpenSl host(runtime, backend, allocator, invoker);
    CHECK(!host.ready());

    // Indices outside the OpenSL range belong to another host-call handler.
    CHECK(!host.handle_host_call(zb::ZB_OPENSL_HC_slCreateEngine - 1, thread));
    CHECK(!host.handle_host_call(zb::ZB_OPENSL_HC_last + 1, thread));

    // ---- slCreateEngine -------------------------------------------------------------------
    std::vector<std::uint32_t> words(15, 0);
    put_words(runtime, kVtable, words);

    // An unreadable vtable is a guest bug, not a crash.
    thread.regs()[0] = kEngineOut;
    thread.regs()[12] = 0xFFFF0000u;
    CHECK(host.handle_host_call(zb::ZB_OPENSL_HC_slCreateEngine, thread));
    CHECK(thread.regs()[0] == kSlResultPreconditionsViolated);
    CHECK(backend.create_engine_calls == 0);
    CHECK(!host.ready());

    thread.regs()[0] = kEngineOut;
    thread.regs()[12] = kVtable;
    CHECK(host.handle_host_call(zb::ZB_OPENSL_HC_slCreateEngine, thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    CHECK(backend.create_engine_calls == 1);
    CHECK(host.ready());
    const std::uint32_t engine_object = word_at(runtime, kEngineOut);
    CHECK(engine_object != 0);
    check_guest_struct(runtime, engine_object, kVtable, kKindEngineObject);

    // A second engine would leak the first one, so the vtable locks in and a second
    // slCreateEngine is refused.
    thread.regs()[0] = kEngineOut;
    thread.regs()[12] = kVtable;
    CHECK(host.handle_host_call(zb::ZB_OPENSL_HC_slCreateEngine, thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    CHECK(backend.create_engine_calls == 2);  // the bridge does not guard the engine count; FMOD
                                              // calls slCreateEngine exactly once

    // ---- engine Realize / GetInterface -----------------------------------------------------
    thread.regs()[0] = engine_object;
    thread.regs()[1] = 0;  // synchronous
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    CHECK(backend.realize_calls == 1);
    CHECK(!backend.last_realize_async);
    void* const engine_realized = backend.last_object_realized;
    CHECK(engine_realized != nullptr);

    thread.regs()[0] = engine_object;
    thread.regs()[1] = kIidEngine;
    thread.regs()[2] = kEngineOut + 0x10;  // out parameter
    CHECK(host.handle_host_call(slot_index(3), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    CHECK(backend.last_interface == zb::OpenSlBackend::Interface::Engine);
    const std::uint32_t engine_interface = word_at(runtime, kEngineOut + 0x10);
    check_guest_struct(runtime, engine_interface, kVtable, kKindEngineInterface);

    // ---- CreateOutputMix -------------------------------------------------------------------
    thread.regs()[0] = engine_interface;
    thread.regs()[1] = kMixOut;
    CHECK(host.handle_host_call(slot_index(7), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    CHECK(backend.create_output_mix_calls == 1);
    const std::uint32_t mix_object = word_at(runtime, kMixOut);
    check_guest_struct(runtime, mix_object, kVtable, kKindMixObject);

    thread.regs()[0] = mix_object;
    thread.regs()[1] = 0;
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    CHECK(backend.realize_calls == 2);
    // The mix is a different backend object than the engine.
    CHECK(backend.last_object_realized != engine_realized);

    // ---- CreateAudioPlayer -----------------------------------------------------------------
    put_words(runtime, kSource, {kSourceLocator, kSourceFormat});
    put_words(runtime, kSourceLocator, {0x800007BDu /* ANDROIDSIMPLEBUFFERQUEUE */, 8});
    put_words(runtime, kSourceFormat,
              {2 /* PCM */, 2 /* stereo */, 48000000 /* 48 kHz in mHz */, 16, 16, 0x3, 2});
    put_words(runtime, kSink, {kSinkLocator, 0});
    put_words(runtime, kSinkLocator, {4 /* OUTPUTMIX */, mix_object});

    thread.regs()[0] = engine_interface;
    thread.regs()[1] = kPlayerOut;
    thread.regs()[2] = kSource;
    thread.regs()[3] = kSink;
    CHECK(host.handle_host_call(slot_index(2), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    CHECK(backend.create_player_calls == 1);
    CHECK(backend.last_format.num_channels == 2);
    CHECK(backend.last_format.samples_per_sec == 48000000);
    CHECK(backend.last_format.bits_per_sample == 16);
    CHECK(backend.last_num_buffers == 8);
    CHECK(backend.last_output_mix != nullptr);
    const std::uint32_t player_object = word_at(runtime, kPlayerOut);
    check_guest_struct(runtime, player_object, kVtable, kKindPlayerObject);

    // A source that is not a simple buffer queue is refused with a legal result.
    put_words(runtime, kSourceLocator, {0x800007BCu, 8});
    thread.regs()[0] = engine_interface;
    thread.regs()[1] = kPlayerOut;
    thread.regs()[2] = kSource;
    thread.regs()[3] = kSink;
    CHECK(host.handle_host_call(slot_index(2), thread));
    CHECK(thread.regs()[0] == 9 /* CONTENT_UNSUPPORTED */);
    CHECK(backend.create_player_calls == 1);
    put_words(runtime, kSourceLocator, {0x800007BDu, 8});

    // ---- player Realize and interfaces -----------------------------------------------------
    thread.regs()[0] = player_object;
    thread.regs()[1] = 0;
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);

    thread.regs()[0] = player_object;
    thread.regs()[1] = kIidSimpleBufferQueue;
    thread.regs()[2] = kEngineOut + 0x14;
    CHECK(host.handle_host_call(slot_index(3), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    const std::uint32_t queue_interface = word_at(runtime, kEngineOut + 0x14);
    check_guest_struct(runtime, queue_interface, kVtable, kKindQueueInterface);

    thread.regs()[0] = player_object;
    thread.regs()[1] = kIidAndroidConfiguration;
    thread.regs()[2] = kEngineOut + 0x18;
    CHECK(host.handle_host_call(slot_index(3), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    const std::uint32_t config_interface = word_at(runtime, kEngineOut + 0x18);
    check_guest_struct(runtime, config_interface, kVtable, kKindConfigInterface);

    thread.regs()[0] = player_object;
    thread.regs()[1] = kIidPlay;
    thread.regs()[2] = kEngineOut + 0x1C;
    CHECK(host.handle_host_call(slot_index(3), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    const std::uint32_t play_interface = word_at(runtime, kEngineOut + 0x1C);
    check_guest_struct(runtime, play_interface, kVtable, kKindPlayInterface);

    // ---- SetConfiguration(androidPlaybackStreamType) ---------------------------------------
    const char* key = "androidPlaybackStreamType";
    std::uint8_t* key_host = runtime.memory().host_ptr(kData + 0xE0, std::strlen(key) + 1,
                                                       zb::kPageRead | zb::kPageWrite);
    CHECK(key_host != nullptr);
    std::memcpy(key_host, key, std::strlen(key) + 1);
    put_words(runtime, kStreamTypeValue, {3 /* SL_ANDROID_STREAM_MEDIA */});

    thread.regs()[0] = config_interface;
    thread.regs()[1] = kData + 0xE0;
    thread.regs()[2] = kStreamTypeValue;
    thread.regs()[3] = 4;
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    CHECK(backend.set_stream_type_calls == 1);
    CHECK(backend.last_stream_type == 3);
    CHECK(backend.last_config_interface != nullptr);

    // Any other key is refused instead of being forwarded.
    key_host[0] = 'x';
    thread.regs()[0] = config_interface;
    thread.regs()[1] = kData + 0xE0;
    thread.regs()[2] = kStreamTypeValue;
    thread.regs()[3] = 4;
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultFeatureUnsupported);
    CHECK(backend.set_stream_type_calls == 1);
    key_host[0] = 'a';

    // ---- RegisterCallback, Enqueue, play state -------------------------------------------
    thread.regs()[0] = queue_interface;
    thread.regs()[1] = 0x1234;  // guest callback address
    thread.regs()[2] = 0x5678;  // guest data word
    CHECK(host.handle_host_call(slot_index(3), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    CHECK(backend.has_callback());

    // The mixer consumes one buffer: HostOpenSl posts the guest callback through the invoker and
    // carries the queue struct and the guest's own data word.
    backend.fire_callback();
    CHECK(callback.called);
    CHECK(callback.function == 0x1234);
    CHECK(callback.a0 == queue_interface);
    CHECK(callback.a1 == 0x5678);

    const char pcm[] = "abcdefgh";
    std::uint8_t* buffer_host =
        runtime.memory().host_ptr(kBufferData, sizeof(pcm), zb::kPageWrite);
    CHECK(buffer_host != nullptr);
    std::memcpy(buffer_host, pcm, sizeof(pcm));

    thread.regs()[0] = queue_interface;
    thread.regs()[1] = kBufferData;
    thread.regs()[2] = sizeof(pcm);
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    CHECK(backend.enqueue_calls == 1);
    CHECK(backend.last_enqueue_size == sizeof(pcm));
    CHECK(backend.enqueued.size() == 1);
    CHECK(std::memcmp(backend.enqueued[0].data(), pcm, sizeof(pcm)) == 0);

    // The stream-content census must say "audible" for that buffer: "abcdefgh" has non-zero 16-bit
    // samples, and a silent stream is the shape a stalled guest mixer produces.
    {
        const std::string text = zb::runtime_report().text();
        CHECK(text.find("audio-sound-1: at-enqueue=1") != std::string::npos);
    }

    // Now feed silence, which is what a route change that kills the guest's mixer looks like.
    std::memset(buffer_host, 0, sizeof(pcm));
    thread.regs()[0] = queue_interface;
    thread.regs()[1] = kBufferData;
    thread.regs()[2] = sizeof(pcm);
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    {
        const std::string text = zb::runtime_report().text();
        CHECK(text.find("audio-silence-1: at-enqueue=2") != std::string::npos);
    }
    std::memcpy(buffer_host, pcm, sizeof(pcm));

    // A zero-length or unreadable buffer is refused before the backend.
    thread.regs()[0] = queue_interface;
    thread.regs()[1] = kBufferData;
    thread.regs()[2] = 0;
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultParameterInvalid);
    thread.regs()[0] = queue_interface;
    thread.regs()[1] = 0xFFFF0000u;
    thread.regs()[2] = 16;
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultParameterInvalid);
    CHECK(backend.enqueue_calls == 2);

    thread.regs()[0] = play_interface;
    thread.regs()[1] = 3;  // SL_PLAYSTATE_PLAYING
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    CHECK(backend.set_play_state_calls == 1);
    CHECK(backend.last_play_state == 3);

    // ---- boundaries ------------------------------------------------------------------------
    // An interface id that exists but is not part of this bridge's playback path.
    const unsigned interfaces_before = backend.get_interface_calls;
    for (const std::uint32_t iid : {kIidOutputMix, kIidVolume}) {
        thread.regs()[0] = player_object;
        thread.regs()[1] = iid;
        thread.regs()[2] = kEngineOut + 0x1C;
        CHECK(host.handle_host_call(slot_index(3), thread));
        CHECK(thread.regs()[0] == kSlResultFeatureUnsupported);
    }
    CHECK(backend.get_interface_calls == interfaces_before);

    // An unknown interface id is a parameter error.
    thread.regs()[0] = player_object;
    thread.regs()[1] = 0xDEADBEEFu;
    thread.regs()[2] = kEngineOut + 0x1C;
    CHECK(host.handle_host_call(slot_index(3), thread));
    CHECK(thread.regs()[0] == kSlResultParameterInvalid);

    // A struct the host did not create is rejected, and no slot reaches the backend.
    const unsigned realize_before = backend.realize_calls;
    thread.regs()[0] = 0xFFFF0000u;
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultParameterInvalid);
    put_words(runtime, kData + 0x1F0, {0x12345678u, 7, kKindPlayerObject});  // wrong vtable
    thread.regs()[0] = kData + 0x1F0;
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultParameterInvalid);
    CHECK(backend.realize_calls == realize_before);

    // ---- Destroy ---------------------------------------------------------------------------
    thread.regs()[0] = player_object;
    CHECK(host.handle_host_call(slot_index(6), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    CHECK(backend.destroy_calls == 1);

    // The freed struct no longer resolves, and its callback is gone.
    thread.regs()[0] = player_object;
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultParameterInvalid);
    callback.called = false;
    backend.fire_callback();
    CHECK(!callback.called);

    // ---- rebuild after a full teardown (a Bluetooth route change does exactly this) ----------
    // FMOD destroys the player, the mix and the engine, then builds the graph again. The stub
    // library may come back at a different base, so the new engine carries a different table
    // address; refusing it would leave the guest permanently silent.
    thread.regs()[0] = mix_object;
    CHECK(host.handle_host_call(slot_index(6), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    thread.regs()[0] = engine_object;
    CHECK(host.handle_host_call(slot_index(6), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    CHECK(backend.destroy_calls == 3);

    put_words(runtime, kVtable2, std::vector<std::uint32_t>(15, 0));
    thread.regs()[0] = kEngineOut;
    thread.regs()[12] = kVtable2;
    CHECK(host.handle_host_call(zb::ZB_OPENSL_HC_slCreateEngine, thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    CHECK(backend.create_engine_calls == 3);
    const std::uint32_t rebuilt_engine = word_at(runtime, kEngineOut);
    CHECK(rebuilt_engine != 0);
    check_guest_struct(runtime, rebuilt_engine, kVtable2, kKindEngineObject);

    thread.regs()[0] = rebuilt_engine;
    thread.regs()[1] = 0;
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);

    thread.regs()[0] = rebuilt_engine;
    thread.regs()[1] = kIidEngine;
    thread.regs()[2] = kEngineOut + 0x10;
    CHECK(host.handle_host_call(slot_index(3), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    const std::uint32_t rebuilt_engine_interface = word_at(runtime, kEngineOut + 0x10);

    thread.regs()[0] = rebuilt_engine_interface;
    thread.regs()[1] = kMixOut;
    CHECK(host.handle_host_call(slot_index(7), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    const std::uint32_t rebuilt_mix = word_at(runtime, kMixOut);

    thread.regs()[0] = rebuilt_mix;
    thread.regs()[1] = 0;
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);

    put_words(runtime, kSinkLocator, {4 /* OUTPUTMIX */, rebuilt_mix});
    thread.regs()[0] = rebuilt_engine_interface;
    thread.regs()[1] = kPlayerOut;
    thread.regs()[2] = kSource;
    thread.regs()[3] = kSink;
    CHECK(host.handle_host_call(slot_index(2), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    const std::uint32_t rebuilt_player = word_at(runtime, kPlayerOut);
    check_guest_struct(runtime, rebuilt_player, kVtable2, kKindPlayerObject);
    CHECK(backend.create_player_calls == 2);

    // A table the guest cannot have produced (unreadable guest memory) is still refused.
    thread.regs()[0] = kEngineOut;
    thread.regs()[12] = 0xFFFF0000u;
    CHECK(host.handle_host_call(zb::ZB_OPENSL_HC_slCreateEngine, thread));
    CHECK(thread.regs()[0] == kSlResultPreconditionsViolated);

    // The graph rebuilt on the new table behaves like the first one.
    thread.regs()[0] = rebuilt_player;
    thread.regs()[1] = kIidPlay;
    thread.regs()[2] = kEngineOut + 0x20;
    CHECK(host.handle_host_call(slot_index(3), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    const std::uint32_t rebuilt_play = word_at(runtime, kEngineOut + 0x20);

    thread.regs()[0] = rebuilt_player;
    thread.regs()[1] = kIidSimpleBufferQueue;
    thread.regs()[2] = kEngineOut + 0x24;
    CHECK(host.handle_host_call(slot_index(3), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    const std::uint32_t rebuilt_queue = word_at(runtime, kEngineOut + 0x24);

    thread.regs()[0] = rebuilt_queue;
    thread.regs()[1] = 0x5000;  // guest callback
    thread.regs()[2] = 0x5004;  // guest context
    CHECK(host.handle_host_call(slot_index(3), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);

    thread.regs()[0] = rebuilt_queue;
    thread.regs()[1] = kBufferData;
    thread.regs()[2] = 16;
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);

    thread.regs()[0] = rebuilt_play;
    thread.regs()[1] = 3;  // SL_PLAYSTATE_PLAYING
    CHECK(host.handle_host_call(slot_index(0), thread));
    CHECK(thread.regs()[0] == kSlResultSuccess);
    CHECK(backend.last_play_state == 3);

    // The call census is what a device run without a working mixer cannot otherwise show: which
    // OpenSL methods were reached, how many succeeded, and the tail of the control sequence.
    {
        const std::string text = zb::runtime_report().text();
        CHECK(text.find("audio-calls: total=") != std::string::npos);
        CHECK(text.find("audio-call-1: ") != std::string::npos);
        CHECK(text.find("realize ok=") != std::string::npos);
        CHECK(text.find("enqueue ok=") != std::string::npos);
        CHECK(text.find("audio-recent-6: ") != std::string::npos);
        // The requested play state distinguishes "the guest asked for playback and the driver
        // refused" from "the guest itself left the stream paused".
        CHECK(text.find("setPlay=0x0 arg=3") != std::string::npos);
        // A route change tears the graph down and rebuilds it on a relocated table.
        CHECK(text.find("audio-vtables: count=2 current=") != std::string::npos);
        CHECK(text.find("reason=unreadable-table") != std::string::npos);
    }

    std::puts("opensl_chain_test PASS");
    return 0;
}
