#include "zb/host_opensl.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

#include "cpu_tuning.h"
#include "zb/log.h"
#include "zb/opensl_hostcalls.h"
#include "zb/process.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

// OpenSL ES constants the guest sends. SL_IID_* are the literal values the guest stub exports
// (guest/stubs/libOpenSLES.S); FMOD dereferences dlsym's result, so it forwards these words.
constexpr std::uint32_t kSlIidObject = 0x7B010000u;
constexpr std::uint32_t kSlIidEngine = 0x7B010001u;
constexpr std::uint32_t kSlIidPlay = 0x7B010002u;
constexpr std::uint32_t kSlIidRecord = 0x7B010003u;
constexpr std::uint32_t kSlIidOutputMix = 0x7B010004u;
constexpr std::uint32_t kSlIidVolume = 0x7B010005u;
constexpr std::uint32_t kSlIidBufferQueue = 0x7B010006u;
constexpr std::uint32_t kSlIidAndroidSimpleBufferQueue = 0x7B010007u;
constexpr std::uint32_t kSlIidAndroidConfiguration = 0x7B010008u;
// The reserved space all guest SL_IID_* values live in (guest/stubs/libOpenSLES.S). Every id
// Android exports is there, including the ones this bridge does not serve, so GetInterface can
// tell "a real interface we do not implement" apart from "not an interface id at all".
constexpr std::uint32_t kSlIidSpaceBase = 0x7B010000u;
constexpr std::uint32_t kSlIidSpaceMask = 0xFFFF0000u;

constexpr std::uint32_t kSlDataLocatorAndroidSimpleBufferQueue = 0x800007BDu;
constexpr std::uint32_t kSlDataLocatorOutputMix = 4u;
constexpr std::uint32_t kSlDataFormatPcm = 2u;

constexpr std::uint32_t kSlResultPreconditionsViolated = 1u;
constexpr std::uint32_t kSlResultMemoryFailure = 3u;
constexpr std::uint32_t kSlResultContentUnsupported = 9u;

constexpr std::uint32_t kGuestStructWords = 3;  // { vtable, handle, kind }
constexpr std::uint32_t kGuestStructBytes = kGuestStructWords * 4;

constexpr std::uint32_t kSlObjectSlotRealize = 0;
constexpr std::uint32_t kSlObjectSlotResume = 1;
constexpr std::uint32_t kSlObjectSlotGetState = 2;
constexpr std::uint32_t kSlObjectSlotGetInterface = 3;
constexpr std::uint32_t kSlObjectSlotRegisterCallback = 4;
constexpr std::uint32_t kSlObjectSlotAbortAsyncOperation = 5;
constexpr std::uint32_t kSlObjectSlotDestroy = 6;
constexpr std::uint32_t kSlObjectSlotSetPriority = 7;
constexpr std::uint32_t kSlObjectSlotGetPriority = 8;
constexpr std::uint32_t kSlObjectSlotSetLossOfControlInterfaces = 9;

constexpr std::uint32_t kSlEngineSlotCreateAudioPlayer = 2;
constexpr std::uint32_t kSlEngineSlotCreateAudioRecorder = 3;
constexpr std::uint32_t kSlEngineSlotCreateOutputMix = 7;

constexpr std::uint32_t kSlPlaySlotSetPlayState = 0;
constexpr std::uint32_t kSlPlaySlotGetPlayState = 1;
constexpr std::uint32_t kSlPlaySlotRegisterCallback = 4;

constexpr std::uint32_t kSlQueueSlotEnqueue = 0;
constexpr std::uint32_t kSlQueueSlotClear = 1;
constexpr std::uint32_t kSlQueueSlotGetState = 2;
constexpr std::uint32_t kSlQueueSlotRegisterCallback = 3;

constexpr std::uint32_t kSlConfigSlotSetConfiguration = 0;

constexpr char kSlAndroidKeyStreamType[] = "androidPlaybackStreamType";

// Call census for the OpenSL path (HostOpenSl::note_call). The names are static literals, so the
// ring below can keep pointers to them.
constexpr unsigned kCensusNames = 24;   // distinct method names, bounded like the GLES census
constexpr unsigned kRecentCalls = 6;    // control calls kept, oldest first

constexpr unsigned kPublishedNames = 10;  // census entries kept in the report

struct CallCensus {
    std::mutex mutex;
    const char* names[kCensusNames] = {};
    std::uint64_t succeeded[kCensusNames] = {};
    std::uint64_t failed[kCensusNames] = {};
    unsigned used = 0;
    std::uint64_t calls = 0;      // every call, including the ones past kCensusNames
    std::uint64_t unlisted = 0;   // calls whose name had no census slot left
    struct Recent {
        const char* name = nullptr;
        std::uint32_t result = 0;
        // The one argument that distinguishes two otherwise identical control calls: for
        // SetPlayState that is the requested state, so a route change shows whether the guest
        // asked to play again or left the stream paused.
        std::uint32_t detail = 0;
    };
    Recent recent[kRecentCalls];
    unsigned next_recent = 0;
    // Lock-free fast counter for steady-state successful Enqueue calls on the RT audio thread.
    std::atomic<std::uint64_t> bulk_enqueue_ok{0};
};

CallCensus& call_census() {
    static CallCensus census;
    return census;
}

// Enqueue is the only method on the refill path (tens of calls a second while sound plays), so it
// is counted but kept out of the ring unless it fails: the ring has to describe the control
// sequence a route change produces, not the steady state.
bool is_bulk_call(const char* name) { return std::strcmp(name, "enqueue") == 0; }

const char* object_call_name(unsigned slot) {
    switch (slot) {
    case kSlObjectSlotRealize: return "realize";
    case kSlObjectSlotResume: return "resume";
    case kSlObjectSlotGetState: return "state";
    case kSlObjectSlotGetInterface: return "iface";
    case kSlObjectSlotRegisterCallback: return "registerCb";
    case kSlObjectSlotAbortAsyncOperation: return "abort";
    case kSlObjectSlotDestroy: return "destroy";
    case kSlObjectSlotSetPriority: return "setPrio";
    case kSlObjectSlotGetPriority: return "getPrio";
    case kSlObjectSlotSetLossOfControlInterfaces: return "setLoss";
    default: return "object?";
    }
}

const char* engine_call_name(unsigned slot) {
    switch (slot) {
    case kSlEngineSlotCreateAudioPlayer: return "createPlayer";
    case kSlEngineSlotCreateAudioRecorder: return "createRecorder";
    case kSlEngineSlotCreateOutputMix: return "createMix";
    default: return "engine?";
    }
}

const char* play_call_name(unsigned slot) {
    switch (slot) {
    case kSlPlaySlotSetPlayState: return "setPlay";
    case kSlPlaySlotGetPlayState: return "getPlay";
    case kSlPlaySlotRegisterCallback: return "playCb";
    default: return "play?";
    }
}

const char* queue_call_name(unsigned slot) {
    switch (slot) {
    case kSlQueueSlotEnqueue: return "enqueue";
    case kSlQueueSlotClear: return "clear";
    case kSlQueueSlotGetState: return "queueState";
    case kSlQueueSlotRegisterCallback: return "queueCb";
    default: return "queue?";
    }
}

const char* config_call_name(unsigned slot) {
    switch (slot) {
    case kSlConfigSlotSetConfiguration: return "config";
    default: return "config?";
    }
}

}  // namespace

HostOpenSl::Call::Call(HostOpenSl& host, GuestThread& thread, std::uint32_t index)
    : host_(host), thread_(thread), index_(index),
      regs_{thread.regs()[0], thread.regs()[1], thread.regs()[2], thread.regs()[3]},
      sp_(thread.regs()[13]) {
    // Capture arguments before installing the default result.
    thread_.regs()[0] = 0;
    thread_.regs()[1] = 0;
}

std::uint32_t HostOpenSl::Call::arg(unsigned position) {
    if (position < 4) return regs_[position];
    const std::uint64_t address = static_cast<std::uint64_t>(sp_) + 4u * (position - 4u);
    if (address + 4 > kGuestSpaceSize) {
        valid_ = false;
        return 0;
    }
    const std::uint8_t* source =
        host_.runtime_.memory().host_ptr(static_cast<std::uint32_t>(address), 4, kPageRead);
    if (source == nullptr) {
        valid_ = false;
        return 0;
    }
    std::uint32_t value = 0;
    std::memcpy(&value, source, 4);
    return value;
}

HostOpenSl::HostOpenSl(LibraryRuntime& runtime, OpenSlBackend& backend, GuestAllocator allocator,
                       GuestInvoker invoker)
    : runtime_(runtime), backend_(backend), allocator_(std::move(allocator)),
      invoker_(std::move(invoker)) {}

bool HostOpenSl::ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return vtable_count_ != 0;
}

std::uint32_t HostOpenSl::read_word(std::uint32_t address, bool& ok) {
    std::uint32_t value = 0;
    ok = read_words(address, &value, 1);
    return value;
}

bool HostOpenSl::write_word(std::uint32_t address, std::uint32_t value) {
    const std::uint8_t* bytes = runtime_.memory().host_ptr(address, 4, kPageRead | kPageWrite);
    if (bytes == nullptr) return false;
    std::memcpy(const_cast<std::uint8_t*>(bytes), &value, 4);
    return true;
}

bool HostOpenSl::read_words(std::uint32_t address, std::uint32_t* out, std::size_t words) {
    if (words == 0) return true;
    const std::uint64_t length = static_cast<std::uint64_t>(words) * 4;
    if (static_cast<std::uint64_t>(address) + length > kGuestSpaceSize) return false;
    const std::uint8_t* source = runtime_.memory().host_ptr(address, length, kPageRead);
    if (source == nullptr) return false;
    std::memcpy(out, source, static_cast<std::size_t>(length));
    return true;
}

std::optional<std::uint32_t> HostOpenSl::allocate_guest(std::size_t size) {
    if (size == 0 || size > UINT32_MAX) return std::nullopt;
    if (allocator_) return allocator_(size);
    GuestCall args;
    args.regs = {static_cast<std::uint32_t>(size), 0, 0, 0};
    const auto result = runtime_.call_on_current(runtime_.service_api().malloc_fn, args);
    if (!result || result->r0 == 0) return std::nullopt;
    return result->r0;
}

void HostOpenSl::free_guest(std::uint32_t address) {
    if (address == 0 || allocator_) return;
    GuestCall args;
    args.regs = {address, 0, 0, 0};
    (void)runtime_.call_on_current(runtime_.service_api().free_fn, args);
}

bool HostOpenSl::read_guest_struct(std::uint32_t address, std::uint32_t& handle, Kind& kind) {
    std::uint32_t words[kGuestStructWords] = {};
    if (!read_words(address, words, kGuestStructWords)) return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool known = false;
        for (std::size_t i = 0; i < vtable_count_; ++i) {
            if (vtables_[i] == words[0]) {
                known = true;
                break;
            }
        }
        if (!known) return false;
    }
    if (words[2] < static_cast<std::uint32_t>(Kind::EngineObject) ||
        words[2] > static_cast<std::uint32_t>(Kind::ObjectInterface)) {
        return false;
    }
    handle = words[1];
    kind = static_cast<Kind>(words[2]);
    return true;
}

std::optional<std::uint32_t> HostOpenSl::make_guest_struct(std::uint32_t handle, Kind kind) {
    const auto address = allocate_guest(kGuestStructBytes);
    if (!address) return std::nullopt;
    const std::uint8_t* bytes =
        runtime_.memory().host_ptr(*address, kGuestStructBytes, kPageRead | kPageWrite);
    if (bytes == nullptr) return std::nullopt;
    std::uint32_t words[kGuestStructWords] = {};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        words[0] = vtable_;
        words[1] = handle;
        words[2] = static_cast<std::uint32_t>(kind);
        objects_[handle].guest_structs.push_back(*address);
    }
    std::memcpy(const_cast<std::uint8_t*>(bytes), words, sizeof(words));
    return address;
}

bool HostOpenSl::handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (index == ZB_OPENSL_HC_slCreateEngine) {
        Call call(*this, thread, index);
        const bool handled = dispatch_engine_create(call);
        note_call("createEngine", thread.regs()[0]);
        return handled;
    }
    if (index < ZB_OPENSL_HC_first_slot || index > ZB_OPENSL_HC_last) return false;
    Call call(*this, thread, index);
    return dispatch_slot(call);
}

bool HostOpenSl::dispatch_engine_create(Call& call) {
    // The stub puts the vtable's guest address in r12 (see guest/stubs/libOpenSLES.S).
    const std::uint32_t vtable = call.thread().regs()[12];
    if (runtime_.memory().host_ptr(vtable, 15 * 4, kPageRead) == nullptr) {
        note_create_reject("unreadable-table", vtable);
        call.set_result(kSlResultPreconditionsViolated);
        return true;
    }
    {
        bool accepted = false;
        bool limited = false;
        std::size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bool known = false;
            for (std::size_t i = 0; i < vtable_count_; ++i) {
                if (vtables_[i] == vtable) {
                    known = true;
                    break;
                }
            }
            if (known) {
                accepted = true;
            } else if (vtable_count_ == kMaxVtables) {
                // More distinct tables than any real teardown/reload cycle produces. Refuse
                // instead of silently aliasing a table the guest may not be able to call.
                limited = true;
            } else {
                vtables_[vtable_count_++] = vtable;
                accepted = true;
            }
            if (accepted) vtable_ = vtable;
            count = vtable_count_;
        }
        if (limited) {
            note_create_reject("table-limit", vtable);
            call.set_result(kSlResultPreconditionsViolated);
            return true;
        }
        if (count > 1) {
            char text[64];
            std::snprintf(text, sizeof text, "count=%zu current=0x%x", count, vtable);
            runtime_report().note_audio_detail("vtables", text, true);
        }
    }

    // slCreateEngine(SLObjectItf *pEngine, numOptions, pOptions, numInterfaces, pIds, pRequired)
    // receives no options from FMOD, and every interface it can ask for here is implicit.
    const std::uint32_t p_engine = call.arg(0);
    void* engine = nullptr;
    std::uint32_t result = backend_.create_engine(&engine);
    if (result == OpenSlBackend::kSuccess && engine != nullptr) {
        std::uint32_t handle = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handle = next_handle_++;
            objects_[handle].object = engine;
        }
        const auto address = make_guest_struct(handle, Kind::EngineObject);
        if (!address) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                objects_.erase(handle);
            }
            backend_.destroy(engine);
            result = kSlResultMemoryFailure;
        } else if (!write_word(p_engine, *address)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                objects_.erase(handle);
            }
            backend_.destroy(engine);
            free_guest(*address);
            result = OpenSlBackend::kParameterInvalid;
        }
    } else if (result == OpenSlBackend::kSuccess) {
        result = kSlResultMemoryFailure;
    }
    if (result != OpenSlBackend::kSuccess || control_logs_.fetch_add(1) < kMaxControlLogs) {
        // Control calls are rare enough that a device log line each is affordable, and the log's
        // own timestamps are what place them against the platform's route-change lines.
        log("OpenSL: slCreateEngine result=0x%x engine=%p", result, engine);
    }
    {
        char text[64];
        std::snprintf(text, sizeof text, "result=0x%x", result);
        runtime_report().note_audio_detail("engine", text, true);
    }
    call.set_result(result);
    return true;
}

bool HostOpenSl::dispatch_slot(Call& call) {
    const std::uint32_t object_address = call.arg(0);
    std::uint32_t handle = 0;
    Kind kind = Kind::EngineObject;
    if (!read_guest_struct(object_address, handle, kind)) {
        call.set_result(OpenSlBackend::kParameterInvalid);
        note_call("badobject", OpenSlBackend::kParameterInvalid);
        return true;
    }
    const std::uint32_t index = call.index();
    const unsigned slot = static_cast<unsigned>(index - ZB_OPENSL_HC_first_slot);
    const char* name = "unsupported";
    bool handled = true;
    switch (kind) {
    case Kind::EngineObject:
    case Kind::MixObject:
    case Kind::PlayerObject:
    case Kind::ObjectInterface:
        name = object_call_name(slot);
        handled = object_slot(call, handle, slot);
        break;
    case Kind::EngineInterface:
        name = engine_call_name(slot);
        handled = engine_slot(call, handle, slot);
        break;
    case Kind::PlayInterface:
        name = play_call_name(slot);
        handled = play_slot(call, handle, slot);
        break;
    case Kind::QueueInterface:
        name = queue_call_name(slot);
        handled = queue_slot(call, handle, slot);
        break;
    case Kind::ConfigInterface:
        name = config_call_name(slot);
        handled = config_slot(call, handle, slot);
        break;
    case Kind::RecordInterface:
    case Kind::VolumeInterface:
    default:
        call.set_result(OpenSlBackend::kFeatureUnsupported);
        break;
    }
    // Every path above leaves the result in r0, so one place can record what the guest asked
    // for and what it got back. SetPlayState also records the requested state.
    const bool set_play = kind == Kind::PlayInterface && slot == kSlPlaySlotSetPlayState;
    note_call(name, call.thread().regs()[0], set_play ? call.arg(1) : 0);
    return handled;
}

bool HostOpenSl::object_slot(Call& call, std::uint32_t handle, unsigned slot) {
    void* object = nullptr;
    void* engine_interface = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = objects_.find(handle);
        if (entry == objects_.end()) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        object = entry->second.object;
        const auto engine = entry->second.interfaces.find(static_cast<std::uint32_t>(Kind::EngineInterface));
        if (engine != entry->second.interfaces.end()) engine_interface = engine->second;
    }

    switch (slot) {
    case kSlObjectSlotRealize: {
        const std::uint32_t result = backend_.realize(object, call.arg(1) != 0);
        note_failure("Realize", result);
        call.set_result(result);
        return true;
    }
    case kSlObjectSlotResume:
        call.set_result(OpenSlBackend::kSuccess);
        return true;
    case kSlObjectSlotGetState: {
        std::uint32_t state = 0;
        const std::uint32_t result = backend_.get_state(object, state);
        if (result == OpenSlBackend::kSuccess && !write_word(call.arg(1), state)) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        call.set_result(result);
        return true;
    }
    case kSlObjectSlotGetInterface: {
        const std::uint32_t iid = call.arg(1);
        const std::uint32_t output = call.arg(2);
        OpenSlBackend::Interface interface{};
        Kind interface_kind = Kind::ObjectInterface;
        switch (iid) {
        case kSlIidObject:
            interface = OpenSlBackend::Interface::Object;
            interface_kind = Kind::ObjectInterface;
            break;
        case kSlIidEngine:
            interface = OpenSlBackend::Interface::Engine;
            interface_kind = Kind::EngineInterface;
            break;
        case kSlIidPlay:
            interface = OpenSlBackend::Interface::Play;
            interface_kind = Kind::PlayInterface;
            break;
        case kSlIidRecord:
            interface = OpenSlBackend::Interface::Record;
            interface_kind = Kind::RecordInterface;
            break;
        case kSlIidOutputMix:
        case kSlIidVolume:
            // Legal interface ids, but neither is part of the playback path this bridge serves
            // (FMOD never asks for them). Their vtable layouts differ from the object layout, so
            // they must not be answered with an object struct.
            call.set_result(OpenSlBackend::kFeatureUnsupported);
            return true;
        case kSlIidBufferQueue:
            interface = OpenSlBackend::Interface::BufferQueue;
            interface_kind = Kind::QueueInterface;
            break;
        case kSlIidAndroidSimpleBufferQueue:
            interface = OpenSlBackend::Interface::AndroidSimpleBufferQueue;
            interface_kind = Kind::QueueInterface;
            break;
        case kSlIidAndroidConfiguration:
            interface = OpenSlBackend::Interface::AndroidConfiguration;
            interface_kind = Kind::ConfigInterface;
            break;
        default:
            // A real SL_IID_* this bridge does not serve (an effect send, a 3D interface, ...)
            // must answer "unsupported" rather than "invalid parameter": callers such as Punch
            // Quest's libgame.so ask for SL_IID_EFFECTSEND and are expected to carry on without
            // it. Anything that is not one of our own ids is still a caller bug.
            if ((iid & kSlIidSpaceMask) == kSlIidSpaceBase) {
                call.set_result(OpenSlBackend::kFeatureUnsupported);
                return true;
            }
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        void* real = nullptr;
        const std::uint32_t result = backend_.get_interface(object, interface, &real);
        note_failure("GetInterface", result);
        if (result != OpenSlBackend::kSuccess) {
            call.set_result(result);
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Entry& entry = objects_[handle];
            const auto kind_key = static_cast<std::uint32_t>(interface_kind);
            const bool had_interface = entry.interfaces.find(kind_key) != entry.interfaces.end();
            entry.interfaces[kind_key] = real;
            if (interface_kind == Kind::QueueInterface && !had_interface) {
                if (active_queue_objects_ == 0) {
                    fast_queue_ptr_.store(real, std::memory_order_relaxed);
                    fast_queue_handle_.store(handle, std::memory_order_release);
                } else {
                    fast_queue_handle_.store(0, std::memory_order_release);
                }
                ++active_queue_objects_;
            }
        }
        const auto address = make_guest_struct(handle, interface_kind);
        if (!address) {
            call.set_result(kSlResultMemoryFailure);
            return true;
        }
        if (!write_word(output, *address)) {
            free_guest(*address);
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        call.set_result(OpenSlBackend::kSuccess);
        return true;
    }
    case kSlObjectSlotRegisterCallback:
        // Object-level callbacks report realization state; FMOD does not install one.
        call.set_result(OpenSlBackend::kSuccess);
        return true;
    case kSlObjectSlotAbortAsyncOperation:
        call.set_result(OpenSlBackend::kSuccess);
        return true;
    case kSlObjectSlotDestroy: {
        (void)engine_interface;
        std::vector<std::uint32_t> structures;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto entry = objects_.find(handle);
            if (entry == objects_.end()) {
                call.set_result(OpenSlBackend::kParameterInvalid);
                return true;
            }
            if (entry->second.interfaces.find(static_cast<std::uint32_t>(Kind::QueueInterface)) !=
                entry->second.interfaces.end()) {
                if (fast_queue_handle_.load(std::memory_order_relaxed) == handle) {
                    fast_queue_handle_.store(0, std::memory_order_release);
                }
                if (active_queue_objects_ > 0) --active_queue_objects_;
            }
            structures = entry->second.guest_structs;
            objects_.erase(entry);
        }
        // Mark callbacks inactive and destroy the host OpenSL object FIRST so Android's audio
        // mixer thread drains any in-flight callbacks before guest interface structs are freed.
        for (const std::uint32_t address : structures) drop_queue_callback(address);
        backend_.destroy(object);
        for (const std::uint32_t address : structures) free_guest(address);
        if (control_logs_.fetch_add(1) < kMaxControlLogs) {
            log("OpenSL: Destroy object");
        }
        call.set_result(OpenSlBackend::kSuccess);
        return true;
    }
    case kSlObjectSlotSetPriority:
        call.set_result(OpenSlBackend::kSuccess);
        return true;
    case kSlObjectSlotGetPriority:
        if (!write_word(call.arg(1), 0)) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        call.set_result(OpenSlBackend::kSuccess);
        return true;
    case kSlObjectSlotSetLossOfControlInterfaces:
        call.set_result(OpenSlBackend::kSuccess);
        return true;
    default:
        call.set_result(OpenSlBackend::kFeatureUnsupported);
        return true;
    }
}

bool HostOpenSl::engine_slot(Call& call, std::uint32_t handle, unsigned slot) {
    void* engine_interface = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = objects_.find(handle);
        if (entry == objects_.end()) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        const auto found = entry->second.interfaces.find(static_cast<std::uint32_t>(Kind::EngineInterface));
        if (found == entry->second.interfaces.end()) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        engine_interface = found->second;
    }

    switch (slot) {
    case kSlEngineSlotCreateOutputMix: {
        // CreateOutputMix(SLEngineItf self, SLObjectItf *pMix, numInterfaces, pIds, pRequired)
        const std::uint32_t p_mix = call.arg(1);
        void* mix = nullptr;
        std::uint32_t result = backend_.create_output_mix(engine_interface, &mix);
        if (result == OpenSlBackend::kSuccess && mix != nullptr) {
            std::uint32_t new_handle = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                new_handle = next_handle_++;
                objects_[new_handle].object = mix;
            }
            const auto address = make_guest_struct(new_handle, Kind::MixObject);
            if (!address) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    objects_.erase(new_handle);
                }
                backend_.destroy(mix);
                result = kSlResultMemoryFailure;
            } else if (!write_word(p_mix, *address)) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    objects_.erase(new_handle);
                }
                backend_.destroy(mix);
                free_guest(*address);
                result = OpenSlBackend::kParameterInvalid;
            }
        } else if (result == OpenSlBackend::kSuccess) {
            result = kSlResultMemoryFailure;
        }
        note_failure("CreateOutputMix", result);
        {
            char text[64];
            std::snprintf(text, sizeof text, "result=0x%x", result);
            runtime_report().note_audio_detail("output-mix", text, true);
        }
        call.set_result(result);
        return true;
    }
    case kSlEngineSlotCreateAudioPlayer: {
        // CreateAudioPlayer(self, SLObjectItf *pPlayer, SLDataSource *pSrc, SLDataSink *pSink,
        //                    numInterfaces, pIds, pRequired)
        const std::uint32_t p_player = call.arg(1);
        std::uint32_t source[2] = {};
        std::uint32_t sink[2] = {};
        if (!read_words(call.arg(2), source, 2) || !read_words(call.arg(3), sink, 2)) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        std::uint32_t locator[2] = {};
        if (!read_words(source[0], locator, 2)) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        if (locator[0] != kSlDataLocatorAndroidSimpleBufferQueue || locator[1] == 0) {
            call.set_result(kSlResultContentUnsupported);
            return true;
        }
        std::uint32_t format[7] = {};
        if (!read_words(source[1], format, 7)) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        if (format[0] != kSlDataFormatPcm) {
            call.set_result(kSlResultContentUnsupported);
            return true;
        }
        std::uint32_t sink_locator[2] = {};
        if (!read_words(sink[0], sink_locator, 2)) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        if (sink_locator[0] != kSlDataLocatorOutputMix) {
            call.set_result(kSlResultContentUnsupported);
            return true;
        }
        OpenSlBackend::PcmFormat pcm;
        pcm.format_type = format[0];
        pcm.num_channels = format[1];
        pcm.samples_per_sec = format[2];
        pcm.bits_per_sample = format[3];
        pcm.container_size = format[4];
        pcm.channel_mask = format[5];
        pcm.endianness = format[6];

        // The sink names the output mix the guest already holds; resolve it to our backend object.
        std::uint32_t mix_handle = 0;
        Kind mix_kind = Kind::MixObject;
        void* mix = nullptr;
        if (read_guest_struct(sink_locator[1], mix_handle, mix_kind)) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto mix_entry = objects_.find(mix_handle);
            if (mix_entry != objects_.end()) mix = mix_entry->second.object;
        }
        if (mix == nullptr || mix_kind != Kind::MixObject) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }

        void* player = nullptr;
        std::uint32_t result =
            backend_.create_player(engine_interface, mix, pcm, locator[1], &player);
        if (result == OpenSlBackend::kSuccess && player != nullptr) {
            std::uint32_t new_handle = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                new_handle = next_handle_++;
                objects_[new_handle].object = player;
            }
            const auto address = make_guest_struct(new_handle, Kind::PlayerObject);
            if (!address) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    objects_.erase(new_handle);
                }
                backend_.destroy(player);
                result = kSlResultMemoryFailure;
            } else if (!write_word(p_player, *address)) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    objects_.erase(new_handle);
                }
                backend_.destroy(player);
                free_guest(*address);
                result = OpenSlBackend::kParameterInvalid;
            }
        } else if (result == OpenSlBackend::kSuccess) {
            result = kSlResultMemoryFailure;
        }
        if (!logged_player_.exchange(true)) {
            log("OpenSL: CreateAudioPlayer result=0x%x channels=%u rate(mHz)=%u bits=%u container=%u "
                "mask=0x%x buffers=%u",
                result, pcm.num_channels, pcm.samples_per_sec, pcm.bits_per_sample,
                pcm.container_size, pcm.channel_mask, locator[1]);
        } else if (control_logs_.fetch_add(1) < kMaxControlLogs) {
            log("OpenSL: CreateAudioPlayer result=0x%x buffers=%u", result, locator[1]);
        }
        {
            char text[160];
            std::snprintf(text, sizeof text,
                          "result=0x%x channels=%u rate=%u bits=%u container=%u mask=0x%x "
                          "buffers=%u",
                          result, pcm.num_channels, pcm.samples_per_sec, pcm.bits_per_sample,
                          pcm.container_size, pcm.channel_mask, locator[1]);
            runtime_report().note_audio_detail("player", text, true);
        }
        call.set_result(result);
        return true;
    }
    case kSlEngineSlotCreateAudioRecorder:
        call.set_result(kSlResultContentUnsupported);
        return true;
    default:
        call.set_result(OpenSlBackend::kFeatureUnsupported);
        return true;
    }
}

bool HostOpenSl::play_slot(Call& call, std::uint32_t handle, unsigned slot) {
    void* play = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = objects_.find(handle);
        if (entry == objects_.end()) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        const auto found = entry->second.interfaces.find(static_cast<std::uint32_t>(Kind::PlayInterface));
        if (found == entry->second.interfaces.end()) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        play = found->second;
    }

    switch (slot) {
    case kSlPlaySlotSetPlayState: {
        const std::uint32_t result = backend_.set_play_state(play, call.arg(1));
        note_failure("SetPlayState", result);
        if (control_logs_.fetch_add(1) < kMaxControlLogs) {
            log("OpenSL: SetPlayState state=%u result=0x%x", call.arg(1), result);
        }
        {
            char text[48];
            std::snprintf(text, sizeof text, "state=%u result=0x%x", call.arg(1), result);
            runtime_report().note_audio_detail("play-state", text, true);
        }
        call.set_result(result);
        return true;
    }
    case kSlPlaySlotGetPlayState: {
        std::uint32_t state = 0;
        const std::uint32_t result = backend_.get_play_state(play, state);
        if (result == OpenSlBackend::kSuccess && !write_word(call.arg(1), state)) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        call.set_result(result);
        return true;
    }
    case kSlPlaySlotRegisterCallback:
        // Play-state callbacks are not needed by the FMOD backend; it polls GetPlayState.
        call.set_result(OpenSlBackend::kSuccess);
        return true;
    default:
        call.set_result(OpenSlBackend::kFeatureUnsupported);
        return true;
    }
}

bool HostOpenSl::queue_slot(Call& call, std::uint32_t handle, unsigned slot) {
    void* queue = nullptr;
    if (handle != 0 && fast_queue_handle_.load(std::memory_order_acquire) == handle) {
        queue = fast_queue_ptr_.load(std::memory_order_relaxed);
    } else {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = objects_.find(handle);
        if (entry == objects_.end()) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        const auto found =
            entry->second.interfaces.find(static_cast<std::uint32_t>(Kind::QueueInterface));
        if (found == entry->second.interfaces.end()) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        queue = found->second;
    }

    switch (slot) {
    case kSlQueueSlotEnqueue: {
        const std::uint32_t buffer = call.arg(1);
        const std::uint32_t size = call.arg(2);
        if (size == 0) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        // Guest memory is host memory at base() + address, so the backend reads the guest's
        // buffer directly. The guest keeps it alive until the callback returns it.
        const std::uint8_t* data = runtime_.memory().host_ptr(buffer, size, kPageRead);
        if (data == nullptr) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        const std::uint32_t result = backend_.enqueue(queue, data, size);
        const std::uint64_t count = ++enqueue_count_;
        std::uint32_t cur_min = enqueue_min_bytes_.load(std::memory_order_relaxed);
        while ((cur_min == 0 || size < cur_min) &&
               !enqueue_min_bytes_.compare_exchange_weak(cur_min, size, std::memory_order_relaxed)) {}
        std::uint32_t cur_max = enqueue_max_bytes_.load(std::memory_order_relaxed);
        while (size > cur_max &&
               !enqueue_max_bytes_.compare_exchange_weak(cur_max, size, std::memory_order_relaxed)) {}
        if (!logged_enqueue_.exchange(true)) {
            log("OpenSL: first Enqueue size=%u result=0x%x", size, result);
        } else {
            note_failure("Enqueue", result);
        }
        // Coarse on purpose: publish startup immediately and steady-state periodically off the
        // hot refill path.
        if (count <= 4 || (count % 2048) == 0) note_stream();
        note_samples(data, size, count);
        call.set_result(result);
        return true;
    }
    case kSlQueueSlotClear:
        call.set_result(backend_.clear(queue));
        return true;
    case kSlQueueSlotGetState: {
        // SLAndroidSimpleBufferQueueState { SLuint32 count; SLuint32 index; }
        const std::uint32_t output = call.arg(1);
        std::uint8_t* bytes = runtime_.memory().host_ptr(output, 8, kPageRead | kPageWrite);
        if (bytes == nullptr) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        std::uint32_t count = 0;
        std::uint32_t index = 0;
        const std::uint32_t rc = backend_.get_queue_state(queue, count, index);
        if (rc == OpenSlBackend::kSuccess) {
            const std::uint32_t words[2] = {count, index};
            std::memcpy(bytes, words, sizeof(words));
        }
        call.set_result(rc);
        return true;
    }
    case kSlQueueSlotRegisterCallback:
        return register_queue_callback(call, handle, call.arg(0));
    default:
        call.set_result(OpenSlBackend::kFeatureUnsupported);
        return true;
    }
}

bool HostOpenSl::config_slot(Call& call, std::uint32_t handle, unsigned slot) {
    void* config = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = objects_.find(handle);
        if (entry == objects_.end()) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        const auto found = entry->second.interfaces.find(static_cast<std::uint32_t>(Kind::ConfigInterface));
        if (found == entry->second.interfaces.end()) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        config = found->second;
    }

    if (slot != kSlConfigSlotSetConfiguration) {
        call.set_result(OpenSlBackend::kFeatureUnsupported);
        return true;
    }

    // SetConfiguration(self, const SLchar *key, const void *value, SLuint32 valueSize). The only
    // key FMOD sends is SL_ANDROID_KEY_STREAM_TYPE, a NUL-terminated string.
    const std::uint32_t key = call.arg(1);
    const std::uint32_t value = call.arg(2);
    const std::uint32_t size = call.arg(3);
    const std::size_t length = sizeof(kSlAndroidKeyStreamType);  // includes the NUL
    const std::uint8_t* key_bytes = runtime_.memory().host_ptr(key, length, kPageRead);
    if (key_bytes == nullptr) {
        call.set_result(OpenSlBackend::kParameterInvalid);
        return true;
    }
    if (std::memcmp(key_bytes, kSlAndroidKeyStreamType, length) != 0) {
        call.set_result(OpenSlBackend::kFeatureUnsupported);
        return true;
    }
    if (size != 4) {
        call.set_result(OpenSlBackend::kParameterInvalid);
        return true;
    }
    bool ok = false;
    const std::uint32_t stream_type = read_word(value, ok);
    if (!ok) {
        call.set_result(OpenSlBackend::kParameterInvalid);
        return true;
    }
    const std::uint32_t result = backend_.set_stream_type(config, static_cast<std::int32_t>(stream_type));
    note_failure("SetConfiguration(stream type)", result);
    call.set_result(result);
    return true;
}

bool HostOpenSl::register_queue_callback(Call& call, std::uint32_t handle, std::uint32_t queue_struct) {
    const std::uint32_t guest_callback = call.arg(1);
    const std::uint32_t guest_context = call.arg(2);
    void* queue = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto entry = objects_.find(handle);
        if (entry == objects_.end()) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        const auto found = entry->second.interfaces.find(static_cast<std::uint32_t>(Kind::QueueInterface));
        if (found == entry->second.interfaces.end()) {
            call.set_result(OpenSlBackend::kParameterInvalid);
            return true;
        }
        queue = found->second;
    }

    drop_queue_callback(queue_struct);
    if (guest_callback == 0) {
        call.set_result(backend_.set_buffer_callback(queue, nullptr, nullptr));
        return true;
    }

    auto context = std::make_shared<CallbackContext>();
    context->host = this;
    context->queue_struct = queue_struct;
    context->guest_callback = guest_callback;
    context->guest_context = guest_context;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_[queue_struct] = context;
    }
    const std::uint32_t result =
        backend_.set_buffer_callback(queue, &HostOpenSl::buffer_callback, context.get());
    note_failure("RegisterCallback(buffer queue)", result);
    if (result != OpenSlBackend::kSuccess) {
        drop_queue_callback(queue_struct);
    }
    call.set_result(result);
    return true;
}

void HostOpenSl::drop_queue_callback(std::uint32_t queue_struct) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = callbacks_.find(queue_struct);
    if (found == callbacks_.end()) return;
    found->second->active.store(false, std::memory_order_release);
    // Keep retired CallbackContext alive rather than dropping past a fixed cap while the RT audio
    // thread could still dereference its raw pointer.
    retired_callbacks_.push_back(std::move(found->second));
    callbacks_.erase(found);
}

void HostOpenSl::buffer_callback(void* context) {
    auto* raw = static_cast<CallbackContext*>(context);
    if (raw == nullptr || !raw->active.load(std::memory_order_acquire)) return;
    HostOpenSl* host = raw->host;
    if (host == nullptr) return;
    const auto now_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const std::uint64_t prev_ns = host->last_callback_ns_.exchange(now_ns, std::memory_order_relaxed);
    // The counter is lock-free and the report is not written from here: this runs on the real
    // OpenSL mixer thread, where a filesystem write would be an audible glitch. The Enqueue path
    // publishes both counters instead.
    if (!host->logged_callback_.exchange(true, std::memory_order_relaxed)) {
        log("OpenSL: first buffer callback");
    }
    const std::uint64_t cb_index = host->callback_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (prev_ns != 0 && now_ns > prev_ns) {
        const std::uint64_t gap_us = (now_ns - prev_ns) / 1000ULL;
        // Ignore long pause/background gaps (> 1s) when tracking active playback jitter.
        if (gap_us < 1'000'000ULL) {
            std::uint64_t max_gap = host->callback_gap_max_us_.load(std::memory_order_relaxed);
            while (gap_us > max_gap &&
                   !host->callback_gap_max_us_.compare_exchange_weak(
                       max_gap, gap_us, std::memory_order_relaxed)) {}
            if (cb_index > 16 && gap_us > 25000ULL) {
                host->callback_gap_slow_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    (void)host->invoke_guest(raw->guest_callback, raw->queue_struct, raw->guest_context);
    const auto end_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (end_ns > now_ns) {
        const std::uint64_t dur_us = (end_ns - now_ns) / 1000ULL;
        std::uint64_t max_dur = host->callback_max_us_.load(std::memory_order_relaxed);
        while (dur_us > max_dur &&
               !host->callback_max_us_.compare_exchange_weak(
                   max_dur, dur_us, std::memory_order_relaxed)) {}
        if (dur_us > 2000ULL) host->callback_slow_.fetch_add(1, std::memory_order_relaxed);
    }
}

void HostOpenSl::note_stream() {
#if defined(ZB_NO_TELEMETRY)
    return;
#else
    char text[192];
    std::snprintf(text, sizeof text,
                  "enqueued=%llu returned=%llu bytes=%u..%u cb-max-us=%llu gap-max-us=%llu gap25=%llu slow=%llu",
                  static_cast<unsigned long long>(enqueue_count_.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(callback_count_.load(std::memory_order_relaxed)),
                  enqueue_min_bytes_.load(std::memory_order_relaxed),
                  enqueue_max_bytes_.load(std::memory_order_relaxed),
                  static_cast<unsigned long long>(callback_max_us_.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(callback_gap_max_us_.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(callback_gap_slow_.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(callback_slow_.load(std::memory_order_relaxed)));
    runtime_report().note_audio_detail("stream", text, true);
#endif
}

void HostOpenSl::note_samples(const std::uint8_t* data, std::uint32_t size, std::uint64_t count) {
#if defined(ZB_NO_TELEMETRY)
    (void)data;
    (void)size;
    (void)count;
    return;
#else
    // 16-bit PCM only: that is what the bridge accepts (see CreateAudioPlayer), so every sample
    // here is one signed 16-bit word. Scan fully during startup (first 8 buffers), then subsample
    // cleanly so the refill path never pays an 8192-iteration loop per buffer.
    const std::size_t samples = std::min<std::size_t>(size / 2, 8192);
    const std::size_t stride = count <= 8 ? 1 : 8;
    unsigned peak = 0;
    unsigned zero_streak = 0;
    unsigned max_zero_streak = 0;
    for (std::size_t i = 0; i < samples; i += stride) {
        std::int16_t value = 0;
        std::memcpy(&value, data + i * 2, sizeof value);
        if (value == 0) {
            if (++zero_streak > max_zero_streak) max_zero_streak = zero_streak;
        } else {
            zero_streak = 0;
        }
        const unsigned magnitude = value < 0 ? static_cast<unsigned>(-static_cast<int>(value))
                                             : static_cast<unsigned>(value);
        if (magnitude > peak) peak = magnitude;
    }

    const std::uint64_t buffers = ++sample_buffers_;
    if (peak != 0) {
        ++sample_nonzero_;
        silent_run_.store(0, std::memory_order_relaxed);
        if (max_zero_streak >= 16) {
            sample_dropouts_.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        silent_run_.fetch_add(1, std::memory_order_relaxed);
    }
    sample_peak_.store(peak, std::memory_order_relaxed);
    unsigned previous_max = sample_max_peak_.load(std::memory_order_relaxed);
    while (peak > previous_max &&
           !sample_max_peak_.compare_exchange_weak(previous_max, peak, std::memory_order_relaxed)) {
    }

    // Only acquire sample_mutex_ on an actual loud/silent state edge.
    const bool loud = peak != 0;
    bool transition = false;
    bool became_loud = false;
    if (loud != sample_loud_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(sample_mutex_);
        const bool prev = sample_loud_.load(std::memory_order_relaxed);
        if (loud != prev) {
            sample_loud_.store(loud, std::memory_order_relaxed);
            became_loud = loud;
            transition = true;
        }
    }
    if (transition) {
        const unsigned slot = became_loud ? transitions_.fetch_add(1) : silent_transitions_.fetch_add(1);
        if (slot < kMaxTransitions) {
            char key[32];
            std::snprintf(key, sizeof key, became_loud ? "sound-%u" : "silence-%u", slot + 1);
            char text[96];
            std::snprintf(text, sizeof text, "at-enqueue=%llu silent-run=%llu peak=%u",
                          static_cast<unsigned long long>(count),
                          static_cast<unsigned long long>(silent_run_.load(std::memory_order_relaxed)),
                          peak);
            runtime_report().note_audio_detail(key, text, false);
            log("OpenSL: stream became %s at enqueue %llu",
                became_loud ? "audible" : "silent",
                static_cast<unsigned long long>(count));
        }
    }
    if (count <= 4 || (count % 2048) == 0) {
        char text[160];
        std::snprintf(text, sizeof text,
                      "buffers=%llu nonzero=%llu dropouts=%llu silent-run=%llu peak=%u max-peak=%u",
                      static_cast<unsigned long long>(buffers),
                      static_cast<unsigned long long>(sample_nonzero_.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(sample_dropouts_.load(std::memory_order_relaxed)),
                      static_cast<unsigned long long>(silent_run_.load(std::memory_order_relaxed)),
                      peak, sample_max_peak_.load(std::memory_order_relaxed));
        runtime_report().note_audio_detail("samples", text, true);
    }
#endif
}

void HostOpenSl::note_failure(const char* operation, std::uint32_t result) {
    if (result == OpenSlBackend::kSuccess) return;
    static std::atomic<unsigned> reported{0};
    if (reported.fetch_add(1) >= 8) return;
    log("OpenSL: %s failed with 0x%x", operation, result);
}

// A refused slCreateEngine is the difference between "the route change hiccuped" and "audio is
// gone until the process restarts", and both cases return the same SLresult, so the reason has to
// be recorded explicitly.
void HostOpenSl::note_create_reject(const char* reason, std::uint32_t vtable) {
    static std::atomic<unsigned> reported{0};
    const unsigned slot = reported.fetch_add(1);
    if (slot >= 4) return;
    char text[96];
    std::snprintf(text, sizeof text, "reason=%s vtable=0x%x", reason, vtable);
    char key[32];
    std::snprintf(key, sizeof key, "create-reject-%u", slot + 1);
    runtime_report().note_audio_detail(key, text, false);
    log("OpenSL: slCreateEngine refused: %s", text);
}

void HostOpenSl::note_call(const char* name, std::uint32_t result, std::uint32_t detail) {
#if defined(ZB_NO_TELEMETRY)
    (void)name;
    (void)result;
    (void)detail;
    return;
#else
    CallCensus& census = call_census();
    if (result == OpenSlBackend::kSuccess && detail == 0 && is_bulk_call(name)) {
        const std::uint64_t ok =
            census.bulk_enqueue_ok.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ok > 8 && (ok % 2048) != 0) return;
        publish_call_census();
        return;
    }
    bool publish = false;
    {
        std::lock_guard<std::mutex> lock(census.mutex);
        unsigned index = 0;
        while (index < census.used && std::strcmp(census.names[index], name) != 0) ++index;
        if (index == census.used) {
            if (census.used < kCensusNames) {
                census.names[census.used++] = name;
            } else {
                ++census.unlisted;
                index = kCensusNames;
            }
        }
        if (index < kCensusNames) {
            if (result == OpenSlBackend::kSuccess) {
                ++census.succeeded[index];
            } else {
                ++census.failed[index];
            }
        }
        // The ring describes the control sequence a route change produces. Enqueue runs on the
        // refill path, so it only enters the ring when it fails.
        if (!is_bulk_call(name) || result != OpenSlBackend::kSuccess) {
            census.recent[census.next_recent].name = name;
            census.recent[census.next_recent].result = result;
            census.recent[census.next_recent].detail = detail;
            census.next_recent = (census.next_recent + 1) % kRecentCalls;
        }
        ++census.calls;
        // A failure is published at once, and so is a control call that carries a detail: the play
        // state is the one fact that must already be on disk when the very next call kills the
        // process. The steady state is published on a coarse interval, because every publish can
        // rewrite the report file.
        publish = result != OpenSlBackend::kSuccess || detail != 0 || census.calls <= 8 ||
                  (census.calls % 2048) == 0;
    }
    if (publish) publish_call_census();
#endif
}

void HostOpenSl::publish_call_census() {
    CallCensus& census = call_census();
    struct Snapshot {
        const char* name;
        std::uint64_t succeeded;
        std::uint64_t failed;
    } entries[kPublishedNames];
    unsigned used = 0;
    std::uint64_t calls = 0;
    std::uint64_t unlisted = 0;
    const char* recent[kRecentCalls] = {};
    std::uint32_t recent_result[kRecentCalls] = {};
    std::uint32_t recent_detail[kRecentCalls] = {};
    unsigned recent_used = 0;
    unsigned next_recent = 0;
    {
        std::lock_guard<std::mutex> lock(census.mutex);
        const std::uint64_t bulk_ok = census.bulk_enqueue_ok.load(std::memory_order_relaxed);
        if (bulk_ok > 0) {
            unsigned enqueue_index = 0;
            while (enqueue_index < census.used &&
                   std::strcmp(census.names[enqueue_index], "enqueue") != 0) {
                ++enqueue_index;
            }
            if (enqueue_index == census.used && census.used < kCensusNames) {
                census.names[census.used++] = "enqueue";
            }
        }
        used = census.used < kPublishedNames ? census.used : kPublishedNames;
        calls = census.calls + bulk_ok;
        unlisted = census.unlisted;
        bool bulk_accounted = false;
        for (unsigned i = 0; i < used; ++i) {
            std::uint64_t ok_count = census.succeeded[i];
            if (!bulk_accounted && std::strcmp(census.names[i], "enqueue") == 0) {
                ok_count += bulk_ok;
                bulk_accounted = true;
            }
            entries[i] = {census.names[i], ok_count, census.failed[i]};
        }
        if (bulk_ok > 0 && !bulk_accounted) unlisted += bulk_ok;
        next_recent = census.next_recent;
        recent_used = 0;
        for (unsigned i = 0; i < kRecentCalls; ++i) {
            const unsigned at = (next_recent + i) % kRecentCalls;
            if (census.recent[at].name == nullptr) continue;
            recent[recent_used] = census.recent[at].name;
            recent_result[recent_used] = census.recent[at].result;
            recent_detail[recent_used] = census.recent[at].detail;
            ++recent_used;
        }
    }

    char text[160];
    std::snprintf(text, sizeof text, "total=%llu distinct=%u unlisted=%llu",
                  static_cast<unsigned long long>(calls), used,
                  static_cast<unsigned long long>(unlisted));
    runtime_report().note_audio_detail("calls", text, true);
    for (unsigned i = 0; i < used; ++i) {
        char key[32];
        std::snprintf(key, sizeof key, "call-%u", i + 1);
        std::snprintf(text, sizeof text, "%s ok=%llu fail=%llu", entries[i].name,
                      static_cast<unsigned long long>(entries[i].succeeded),
                      static_cast<unsigned long long>(entries[i].failed));
        runtime_report().note_audio_detail(key, text, true);
    }
    for (unsigned i = 0; i < recent_used; ++i) {
        char key[32];
        std::snprintf(key, sizeof key, "recent-%u", i + 1);
        if (recent_detail[i] != 0) {
            std::snprintf(text, sizeof text, "%s=0x%x arg=%u", recent[i], recent_result[i],
                          recent_detail[i]);
        } else {
            std::snprintf(text, sizeof text, "%s=0x%x", recent[i], recent_result[i]);
        }
        runtime_report().note_audio_detail(key, text, true);
    }
}

HostOpenSl::~HostOpenSl() {
    if (alive_) alive_->store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : callbacks_) {
        if (entry.second) entry.second->active.store(false, std::memory_order_release);
    }
    callbacks_.clear();
    retired_callbacks_.clear();
}

GuestResult HostOpenSl::invoke_guest(std::uint32_t function, std::uint32_t a0, std::uint32_t a1) {
    GuestCall args;
    args.regs = {a0, a1, 0, 0};
    if (invoker_) {
        const auto result = invoker_(function, args);
        return result ? *result : GuestResult{};
    }
    if (Process::current_thread() != nullptr) {
        const auto result = runtime_.call_on_current(function, args);
        return result ? *result : GuestResult{};
    }
    // Execute OpenSL buffer callbacks directly on Android's real-time audio mixer thread using a
    // lazily leased per-thread Carrier instead of hopping through the single normal-priority
    // service thread.
    struct ThreadCarrier {
        const HostOpenSl* owner = nullptr;
        std::shared_ptr<std::atomic<bool>> alive;
        std::unique_ptr<LibraryRuntime::Carrier> carrier;
        ~ThreadCarrier() {
            if (carrier && (!alive || !alive->load(std::memory_order_acquire))) {
                (void)carrier.release();
            }
        }
    };
    thread_local ThreadCarrier slot;
    if (slot.owner != this || !slot.alive || !slot.alive->load(std::memory_order_acquire)) {
        slot.carrier.reset();
        slot.owner = this;
        slot.alive = alive_;
    }
    if (!slot.carrier) {
        std::string error;
        slot.carrier = runtime_.borrow(error);
        if (!slot.carrier) {
            static std::atomic<bool> logged_borrow_fail{false};
            if (!logged_borrow_fail.exchange(true, std::memory_order_relaxed)) {
                log("OpenSL: cannot borrow a guest carrier (%s); falling back to service thread",
                    error.c_str());
            }
            const auto result = runtime_.call_on_service(function, args);
            return result ? *result : GuestResult{};
        }
    }
    const auto result = slot.carrier->call(function, args);
    return result ? *result : GuestResult{};
}

}  // namespace zb
