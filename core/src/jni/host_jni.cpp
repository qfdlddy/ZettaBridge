// HostJni core: API registration, per-thread JNI state, handle and id resolution, guest memory
// access, native calls, and the host-call switch.
#include "host_jni_internal.h"

#include <atomic>
#include <string>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "zb/log.h"
#include "zb/process.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

struct JniHostCallName {
    std::uint32_t index;
    const char* name;
};

constexpr JniHostCallName kJniHostCallNames[] = {
#include "gen/jni_hostcalls.inc"
};

// Longest guest string read by a host call (class names, signatures, UTF text).
constexpr std::uint64_t kMaxGuestString = 64u << 20;

thread_local JniThread t_thread;

class ThreadEnvScope {
public:
    ThreadEnvScope(JniThread& state, JniBackend::Env env, std::string& error)
        : state_(state), previous_(state.env) {
        if (env == 0) {
            error = "JNI loader received a null JNIEnv";
            return;
        }
        if (previous_ != 0 && previous_ != env) {
            error = "JNI loader received a JNIEnv from another call";
            return;
        }
        state_.env = env;
        active_ = true;
    }

    ~ThreadEnvScope() {
        if (active_) state_.env = previous_;
    }

    bool active() const { return active_; }

private:
    JniThread& state_;
    JniBackend::Env previous_;
    bool active_ = false;
};

std::string loader_error(HostJni::Impl& jni, JniThread& state) {
    const auto result = jni.invoke(state, jni.runtime.service_api().dlerror_fn, GuestCall{});
    if (!result || result->r0 == 0) return "guest dlerror returned no message";
    std::string text;
    for (std::uint64_t i = 0; i < ZB_SERVICE_SCRATCH_SIZE; ++i) {
        const std::uint64_t address = static_cast<std::uint64_t>(result->r0) + i;
        if (address >= kGuestSpaceSize) return "guest dlerror returned an unreadable string";
        const std::uint8_t* byte =
            jni.runtime.memory().host_ptr(static_cast<std::uint32_t>(address), 1, kPageRead);
        if (byte == nullptr) return "guest dlerror returned an unreadable string";
        if (*byte == 0) return text;
        text.push_back(static_cast<char>(*byte));
    }
    return "guest dlerror string is not terminated";
}

std::uint32_t loader_operation(HostJni::Impl& jni, JniBackend::Env env, bool symbol,
                               std::uint32_t value, const std::string& text, std::string& error) {
    error.clear();
    if (!jni.ready.load()) {
        error = "JNI bridge is not ready";
        return 0;
    }
    if (text.find('\0') != std::string::npos || text.size() >= ZB_SERVICE_SCRATCH_SIZE) {
        error = "guest loader string does not fit the bounded buffer";
        return 0;
    }
    JniThread& state = jni.thread();
    ThreadEnvScope env_scope(state, env, error);
    if (!env_scope.active()) return 0;

    bool pushed_frame = false;
    if (!symbol) {
        if (!jni.ensure_guest_env(state)) {
            error = "cannot allocate guest JNIEnv for dlopen";
            return 0;
        }
        if (env != 0 && jni.backend.push_local_frame(env, 16) == 0) {
            state.locals.push_frame();
            pushed_frame = true;
        }
    }

    GuestCall allocate;
    allocate.regs = {static_cast<std::uint32_t>(text.size() + 1), 0, 0, 0};
    const auto allocated = jni.invoke(state, jni.runtime.service_api().malloc_fn, allocate);
    if (!allocated || allocated->r0 == 0) {
        if (pushed_frame) {
            state.locals.pop_frame();
            (void)jni.backend.pop_local_frame(env, 0);
        }
        error = "guest malloc for the loader string failed";
        return 0;
    }
    const std::uint32_t scratch = allocated->r0;
    std::uint8_t* destination = jni.runtime.memory().host_ptr(scratch, text.size() + 1, kPageWrite);
    if (destination == nullptr) {
        error = "guest loader string buffer is not writable";
    } else {
        std::memcpy(destination, text.c_str(), text.size() + 1);
    }

    std::optional<GuestResult> result;
    if (destination != nullptr) {
        GuestCall call;
        call.regs = symbol ? std::array<std::uint32_t, 4>{value, scratch, 0, 0}
                           : std::array<std::uint32_t, 4>{scratch, value, 0, 0};
        const std::uint32_t function = symbol ? jni.runtime.service_api().dlsym_fn
                                              : jni.runtime.service_api().dlopen_fn;
        result = jni.invoke(state, function, call);
        if (!result) {
            error = symbol ? "guest dlsym call failed" : "guest dlopen call failed";
        } else if (result->r0 == 0) {
            error = loader_error(jni, state);
        }
    }

    GuestCall release;
    release.regs = {scratch, 0, 0, 0};
    (void)jni.invoke(state, jni.runtime.service_api().free_fn, release);
    if (pushed_frame) {
        state.locals.pop_frame();
        (void)jni.backend.pop_local_frame(env, 0);
    }
    return result ? result->r0 : 0;
}

}  // namespace

const char* jni_host_call_name(std::uint32_t index) {
    for (const auto& entry : kJniHostCallNames) {
        if (entry.index == index) return entry.name;
    }
    return "?";
}

JniThread::~JniThread() {
    // A Java thread that ran guest natives frees its guest JNIEnv on its carrier before the lease
    // ends. Guest threads free theirs in DetachCurrentThread.
    if (owner != nullptr && carrier && guest_env != 0) {
        GuestCall args;
        args.regs = {guest_env, 0, 0, 0};
        (void)carrier->call(owner->api.free_env_fn, args);
        guest_env = 0;
    }
}

std::uint32_t JniCall::arg(unsigned position) const {
    if (position < 4) return regs_[position];
    const std::uint32_t address = thread_.regs()[13] + 4 * (position - 4);
    const std::uint8_t* word = jni_.runtime.memory().host_ptr(address, 4, kPageRead);
    if (word == nullptr) {
        jni_.fatal(state_.env, "JNI %s: argument %u is not on a readable guest stack", jni_host_call_name(index_),
                   position);
    }
    std::uint32_t value;
    std::memcpy(&value, word, sizeof value);
    return value;
}

JniBackend::Env JniCall::env() {
    if (state_.env == 0) {
        const char* name = jni_host_call_name(index_);
        log("JNI %s called on a thread without a JNIEnv", name);
        runtime_report().note_jni_detail("fatal", std::string("JNI ") + name + " called without JNIEnv", true);
        runtime_report().note_jni_detail("recent-calls", jni_recent_calls(), true);
        std::abort();
    }
    return state_.env;
}

JniThread& HostJni::Impl::thread() {
    JniThread& state = t_thread;
    state.owner = this;
    return state;
}

void HostJni::Impl::fatal(JniBackend::Env env, const char* fmt, ...) {
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    log("JNI fatal error: %s", text);
    runtime_report().note_jni_detail("fatal", text, true);
    runtime_report().note_jni_detail("recent-calls", jni_recent_calls(), true);
    runtime_report().note_jni_detail("recent-names", jni_recent_call_names(), true);
    if (env != 0) backend.fatal_error(env, text);
    std::abort();
}

JniBackend::Ref HostJni::Impl::resolve(JniThread& state, std::uint32_t handle, const char* function) {
    if (handle == 0) return 0;
    std::optional<std::uint64_t> ref;
    switch (handle & 3u) {
    case static_cast<std::uint32_t>(HandleKind::Local):
        ref = state.locals.get(handle);
        break;
    case static_cast<std::uint32_t>(HandleKind::Global):
        ref = globals.get(handle);
        break;
    case static_cast<std::uint32_t>(HandleKind::WeakGlobal):
        ref = weaks.get(handle);
        break;
    default:
        break;
    }
    if (!ref) fatal(state.env, "%s: invalid JNI reference 0x%08x", function, handle);
    return *ref;
}

std::uint32_t HostJni::Impl::intern_method(JniBackend::Id id, const std::string& shorty,
                                           MethodHook hook, const std::string& java_name,
                                           const std::string& java_signature) {
    const std::uint32_t guest = methods.intern(id);
    if (guest == 0) return 0;
    std::lock_guard<std::mutex> lock(shorty_mutex);
    const std::size_t page_idx = guest >> kMethodPageBits;
    if (page_idx < kMethodPages) {
        MethodPage* page = method_pages_[page_idx].load(std::memory_order_relaxed);
        if (page == nullptr) {
            auto owned_page = std::make_unique<MethodPage>();
            page = owned_page.get();
            owned_method_pages_.push_back(std::move(owned_page));
            method_pages_[page_idx].store(page, std::memory_order_release);
        }
        const std::size_t slot = guest & (kMethodPageSize - 1);
        const MethodRecord* existing = page->entries[slot].load(std::memory_order_relaxed);
        const bool hook_changed = existing != nullptr && hook != MethodHook::None &&
                                  existing->hook != hook;
        const bool detail_missing = existing != nullptr &&
                                    ((existing->name.empty() && !java_name.empty()) ||
                                     (existing->signature.empty() && !java_signature.empty()));
        if (existing == nullptr || hook_changed || detail_missing) {
            // Name and signature are diagnostic only, so re-interning the same id must not lose
            // the shorty or hook that were already recorded for it (and vice versa).
            MethodRecord record = existing != nullptr
                                      ? *existing
                                      : MethodRecord{id, shorty, java_name, java_signature, hook};
            if (hook_changed) record.hook = hook;
            if (record.name.empty()) record.name = java_name;
            if (record.signature.empty()) record.signature = java_signature;
            auto owned_record = std::make_unique<MethodRecord>(std::move(record));
            const MethodRecord* raw = owned_record.get();
            owned_methods_.push_back(std::move(owned_record));
            page->entries[slot].store(raw, std::memory_order_release);
        }
        return guest;
    }
    const std::size_t overflow_idx = guest - kMethodPages * kMethodPageSize;
    if (method_overflow_.size() <= overflow_idx) method_overflow_.resize(overflow_idx + 1);
    method_overflow_[overflow_idx] = MethodRecord{id, shorty, java_name, java_signature, hook};
    return guest;
}

const HostJni::Impl::MethodRecord* HostJni::Impl::method_record_fast(std::uint32_t id) const {
    if (id == 0) return nullptr;
    const std::size_t page_idx = id >> kMethodPageBits;
    if (page_idx >= kMethodPages) return nullptr;
    const MethodPage* page = method_pages_[page_idx].load(std::memory_order_acquire);
    if (page == nullptr) return nullptr;
    return page->entries[id & (kMethodPageSize - 1)].load(std::memory_order_acquire);
}

std::optional<std::string> HostJni::Impl::method_shorty(std::uint32_t id) {
    if (const MethodRecord* fast = method_record_fast(id)) return fast->shorty;
    if (id < kMethodPages * kMethodPageSize) return std::nullopt;
    std::lock_guard<std::mutex> lock(shorty_mutex);
    const std::size_t overflow_idx = id - kMethodPages * kMethodPageSize;
    if (overflow_idx >= method_overflow_.size() || method_overflow_[overflow_idx].shorty.empty()) {
        return std::nullopt;
    }
    return method_overflow_[overflow_idx].shorty;
}

std::optional<HostJni::Impl::MethodRecord> HostJni::Impl::method_record(std::uint32_t id) {
    if (const MethodRecord* fast = method_record_fast(id)) return *fast;
    if (id < kMethodPages * kMethodPageSize) return std::nullopt;
    std::lock_guard<std::mutex> lock(shorty_mutex);
    const std::size_t overflow_idx = id - kMethodPages * kMethodPageSize;
    if (overflow_idx >= method_overflow_.size() || method_overflow_[overflow_idx].shorty.empty()) {
        return std::nullopt;
    }
    return method_overflow_[overflow_idx];
}

JniBackend::Id HostJni::Impl::method_id(JniBackend::Env env, std::uint32_t id, const char* function) {
    const std::optional<std::uint64_t> host = methods.get(id);
    if (id == 0 || !host) fatal(env, "%s: invalid jmethodID 0x%08x", function, id);
    return *host;
}

JniBackend::Id HostJni::Impl::field_id(JniBackend::Env env, std::uint32_t id, const char* function) {
    const std::optional<std::uint64_t> host = fields.get(id);
    if (id == 0 || !host) fatal(env, "%s: invalid jfieldID 0x%08x", function, id);
    return *host;
}

void HostJni::Impl::note_null_result(const std::string& what) {
    const std::uint64_t total = null_result_total_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::string key;
    {
        std::lock_guard<std::mutex> lock(null_result_mutex_);
        if (null_results_.size() < kMaxNullResults &&
            std::find(null_results_.begin(), null_results_.end(), what) == null_results_.end()) {
            null_results_.push_back(what);
            key = "null-" + std::to_string(null_results_.size());
        }
    }
    runtime_report().note_jni_detail("null-results", std::to_string(total), true);
    if (!key.empty()) runtime_report().note_jni_detail(key, what, true);
}

std::string HostJni::Impl::read_string(JniBackend::Env env, std::uint32_t address, const char* function) {
    std::string text;
    std::uint64_t cursor = address;
    for (;;) {
        // Read up to the end of the current page, which is mapped as a whole or not at all.
        const std::uint64_t chunk = kPageSize - (cursor & kPageMask);
        if (cursor >= kGuestSpaceSize || text.size() > kMaxGuestString) {
            fatal(env, "%s: unterminated guest string at 0x%08x", function, address);
        }
        const std::uint8_t* bytes =
            runtime.memory().host_ptr(static_cast<std::uint32_t>(cursor), chunk, kPageRead);
        if (bytes == nullptr) fatal(env, "%s: unreadable guest string at 0x%08x", function, address);
        const auto* end = static_cast<const std::uint8_t*>(std::memchr(bytes, 0, chunk));
        if (end != nullptr) {
            text.append(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(end - bytes));
            return text;
        }
        text.append(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(chunk));
        cursor += chunk;
    }
}

const std::uint8_t* HostJni::Impl::readable(JniBackend::Env env, std::uint32_t address, std::uint64_t size,
                                            const char* function) {
    const std::uint8_t* bytes = runtime.memory().host_ptr(address, size, kPageRead);
    if (bytes == nullptr || static_cast<std::uint64_t>(address) + size > kGuestSpaceSize) {
        fatal(env, "%s: unreadable guest buffer 0x%08x (%llu bytes)", function, address,
              static_cast<unsigned long long>(size));
    }
    return bytes;
}

std::uint8_t* HostJni::Impl::writable(JniBackend::Env env, std::uint32_t address, std::uint64_t size,
                                      const char* function) {
    std::uint8_t* bytes = runtime.memory().host_ptr(address, size, kPageWrite);
    if (bytes == nullptr || static_cast<std::uint64_t>(address) + size > kGuestSpaceSize) {
        fatal(env, "%s: unwritable guest buffer 0x%08x (%llu bytes)", function, address,
              static_cast<unsigned long long>(size));
    }
    return bytes;
}

void HostJni::Impl::write_shorty(JniBackend::Env env, std::uint32_t address, const std::string& shorty,
                                 const char* function) {
    if (shorty.size() + 1 > ZB_JNI_SHORTY_SIZE) fatal(env, "%s: shorty longer than 256 letters", function);
    std::memcpy(writable(env, address, shorty.size() + 1, function), shorty.c_str(), shorty.size() + 1);
}

std::optional<GuestResult> HostJni::Impl::invoke(JniThread& state, std::uint32_t function, const GuestCall& args) {
    if (Process::current_thread() != nullptr) return runtime.call_on_current(function, args);
    if (!state.carrier) {
        std::string error;
        state.carrier = runtime.borrow(error);
        if (!state.carrier) {
            log("JNI: cannot borrow a guest carrier: %s", error.c_str());
            runtime_report().note_jni_detail("borrow-failed", error, true);
            return std::nullopt;
        }
    }
    return state.carrier->call(function, args);
}

bool HostJni::Impl::ensure_guest_env(JniThread& state) {
    if (state.guest_env != 0) return true;
    const std::optional<GuestResult> env = invoke(state, api.new_env_fn, GuestCall{});
    if (!env || env->r0 == 0) {
        log("JNI: guest JNIEnv allocation failed");
        return false;
    }
    state.guest_env = env->r0;
    return true;
}

std::vector<std::string> HostJni::mapped_file_paths() const {
    return impl_->runtime.mapped_file_paths();
}

HostJni::HostJni(LibraryRuntime& runtime, JniBackend& backend, std::size_t slot_capacity)
    : impl_(std::make_unique<Impl>(runtime, backend, slot_capacity)) {
    impl_->owner = this;
    install_native_dispatcher(impl_.get());
}

HostJni::~HostJni() {
    log("HostJni destroyed; the JNI bridge is process-lifetime");
    std::abort();
}

std::optional<GuestResult> HostJni::call_on_host_thread(std::uint32_t function, const GuestCall& args) {
    return impl_->invoke(impl_->thread(), function, args);
}

bool HostJni::ready() const {
    return impl_->ready.load();
}

std::uint32_t HostJni::guest_java_vm() const {
    return impl_->api.java_vm;
}

JniBackend::Env HostJni::current_env() {
    return impl_->thread().env;
}

JniBackend::Ref HostJni::resolve_ref(std::uint32_t handle, const char* function) {
    return impl_->resolve(impl_->thread(), handle, function);
}

std::uint32_t HostJni::new_global_handle(JniBackend::Ref ref) {
    if (ref == 0) return 0;
    return impl_->globals.add(ref);
}

JniBackend::Ref HostJni::remove_global_handle(std::uint32_t handle) {
    if (handle == 0) return 0;
    const std::optional<std::uint64_t> ref = impl_->globals.remove(handle);
    return ref.value_or(0);
}

JniBackend::Ref HostJni::retain_global_ref(JniBackend::Env env, JniBackend::Ref ref) {
    if (env == 0 || ref == 0) return 0;
    return impl_->backend.new_global_ref(env, ref);
}

void HostJni::release_global_ref(JniBackend::Env env, JniBackend::Ref ref) {
    if (env == 0 || ref == 0) return;
    impl_->backend.delete_global_ref(env, ref);
}

std::uint32_t HostJni::ensure_guest_env_for_current(JniBackend::Env env) {
    Impl& jni = *impl_;
    if (!jni.ready.load()) return 0;
    JniThread& state = jni.thread();
    std::string error;
    ThreadEnvScope env_scope(state, env, error);
    if (!env_scope.active()) return 0;
    if (!jni.ensure_guest_env(state)) return 0;
    return state.guest_env;
}

std::uint32_t HostJni::new_local_handle(JniBackend::Ref ref) {
    if (ref == 0) return 0;
    return impl_->local(impl_->thread(), ref);
}

std::uint32_t HostJni::load_library_on_current(JniBackend::Env env, const std::string& path,
                                               std::uint32_t guest_flags, std::string& error) {
    return loader_operation(*impl_, env, false, guest_flags, path, error);
}

std::uint32_t HostJni::find_symbol_on_current(JniBackend::Env env, std::uint32_t handle,
                                              const std::string& name, std::string& error) {
    return loader_operation(*impl_, env, true, handle, name, error);
}

namespace {

// The last JNI host calls, for a crash report. A guest that dies inside its own code usually died
// because of what the previous call answered, and there is no logcat on the device.
constexpr std::size_t kRecentJniCalls = 24;
std::atomic<std::uint32_t> g_recent[kRecentJniCalls];
std::atomic<std::uint64_t> g_recent_next{0};

void note_recent(std::uint32_t index) {
#if defined(ZB_NO_TELEMETRY)
    (void)index;
#else
    const std::uint64_t slot = g_recent_next.fetch_add(1, std::memory_order_relaxed);
    g_recent[slot % kRecentJniCalls].store(index, std::memory_order_relaxed);
#endif
}

// The method name of the call a CallMethodA performed, when it was looked up through GetMethodID.
// The index alone ("CallMethodA") hides the difference between getObbDir, getAbsolutePath and a
// Bundle.getString, which is exactly what a crash caused by one of them has to distinguish.
constexpr std::size_t kRecentCalls = 24;
std::mutex g_recent_names_mutex;
std::array<std::string, kRecentCalls> g_recent_names;
std::uint64_t g_recent_names_next = 0;

void note_recent_name(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_recent_names_mutex);
    g_recent_names[g_recent_names_next % kRecentCalls] = name;
    ++g_recent_names_next;
}

}  // namespace

void jni_note_call_name(const std::string& name) {
#if defined(ZB_NO_TELEMETRY)
    (void)name;
#else
    note_recent_name(name);
#endif
}

// Actual string values returned by CallMethodA, for the handful of getters whose result a guest
// turns into a path or an identifier. Bounded and truncated so a hot getter cannot grow the report.
constexpr std::size_t kRecentStrings = 16;
std::mutex g_recent_strings_mutex;
std::array<std::string, kRecentStrings> g_recent_strings;
std::uint64_t g_recent_strings_next = 0;

void jni_note_string_result(const std::string& name, const std::string& value) {
#if defined(ZB_NO_TELEMETRY)
    (void)name;
    (void)value;
#else
    if (value.size() > 512) return;
    // A toString() that produced a throwable's text is the single most valuable string here; give
    // it its own report line so the bounded combined "recent-jni-strings" line cannot truncate it.
    if (value.find("Exception") != std::string::npos || value.find("Error") != std::string::npos) {
        runtime_report().note_crash_detail("jni-toString-throwable", name + "=" + value);
    }
    std::lock_guard<std::mutex> lock(g_recent_strings_mutex);
    g_recent_strings[g_recent_strings_next % kRecentStrings] = name + "=" + value;
    ++g_recent_strings_next;
#endif
}

std::string jni_recent_string_results() {
    std::lock_guard<std::mutex> lock(g_recent_strings_mutex);
    if (g_recent_strings_next == 0) return "(none)";
    const std::uint64_t first = g_recent_strings_next > kRecentStrings ? g_recent_strings_next - kRecentStrings : 0;
    std::string out;
    for (std::uint64_t i = first; i < g_recent_strings_next; ++i) {
        const std::string& item = g_recent_strings[i % kRecentStrings];
        if (item.empty()) continue;
        if (!out.empty()) out += " | ";
        out += item;
    }
    return out.empty() ? "(none)" : out;
}

std::string jni_recent_call_names() {
    std::lock_guard<std::mutex> lock(g_recent_names_mutex);
    if (g_recent_names_next == 0) return "(none)";
    const std::uint64_t first = g_recent_names_next > kRecentCalls ? g_recent_names_next - kRecentCalls : 0;
    std::string out;
    for (std::uint64_t i = first; i < g_recent_names_next; ++i) {
        const std::string& name = g_recent_names[i % kRecentCalls];
        if (name.empty()) continue;
        if (!out.empty()) out += " ";
        out += name;
    }
    return out.empty() ? "(none)" : out;
}

std::string jni_recent_calls() {
    const std::uint64_t next = g_recent_next.load(std::memory_order_relaxed);
    if (next == 0) return "(none)";
    const std::uint64_t first = next > kRecentJniCalls ? next - kRecentJniCalls : 0;
    std::string out;
    for (std::uint64_t i = first; i < next; ++i) {
        const std::uint32_t index = g_recent[i % kRecentJniCalls].load(std::memory_order_relaxed);
        if (!out.empty()) out += " ";
        out += jni_host_call_name(index);
    }
    return out;
}

namespace {

[[noreturn]] void abort_jni(const std::string& detail) {
    runtime_report().note_jni_detail("fatal", detail, true);
    runtime_report().note_jni_detail("recent-calls", jni_recent_calls(), true);
    std::abort();
}

// The JNI calls whose whole subject is the pending exception's state. Holding the exception aside
// for one of these would answer it wrongly, so they always run against the real state.
bool jni_call_inspects_exception(std::uint32_t index) {
    switch (index) {
    case ZB_JNI_HC_ExceptionOccurred:
    case ZB_JNI_HC_ExceptionDescribe:
    case ZB_JNI_HC_ExceptionClear:
    case ZB_JNI_HC_ExceptionCheck:
        return true;
    default:
        return false;
    }
}

bool jni_call_is_fast_leaf(std::uint32_t index, GuestThread& thread) {
    // Strict allowlist of genuinely leaf JNI calls that never invoke Java bytecode, class
    // initializers (<clinit>), constructors, or guest code re-entry.
    switch (index) {
    case ZB_JNI_HC_GetField:
    case ZB_JNI_HC_SetField:
        // Instance field accesses (arg0 == 0) operate on an already-constructed object and never
        // trigger <clinit> or Java bytecode execution.
        return thread.regs()[0] == 0;
    case ZB_JNI_HC_GetEnv:
    case ZB_JNI_HC_GetSuperclass:
    case ZB_JNI_HC_IsAssignableFrom:
    case ZB_JNI_HC_GetObjectClass:
    case ZB_JNI_HC_IsInstanceOf:
    case ZB_JNI_HC_IsSameObject:
    case ZB_JNI_HC_GetObjectRefType:
    case ZB_JNI_HC_GetMethodShorty:
    case ZB_JNI_HC_NewLocalRef:
    case ZB_JNI_HC_DeleteLocalRef:
    case ZB_JNI_HC_NewGlobalRef:
    case ZB_JNI_HC_DeleteGlobalRef:
    case ZB_JNI_HC_NewWeakGlobalRef:
    case ZB_JNI_HC_DeleteWeakGlobalRef:
    case ZB_JNI_HC_EnsureLocalCapacity:
    case ZB_JNI_HC_PushLocalFrame:
    case ZB_JNI_HC_PopLocalFrame:
    case ZB_JNI_HC_Throw:
    case ZB_JNI_HC_ExceptionOccurred:
    case ZB_JNI_HC_ExceptionClear:
    case ZB_JNI_HC_ExceptionCheck:
    case ZB_JNI_HC_NewString:
    case ZB_JNI_HC_GetStringLength:
    case ZB_JNI_HC_GetStringRegion:
    case ZB_JNI_HC_NewStringUTF:
    case ZB_JNI_HC_GetStringUTFLength:
    case ZB_JNI_HC_GetStringUTFRegion:
    case ZB_JNI_HC_GetArrayLength:
    case ZB_JNI_HC_NewObjectArray:
    case ZB_JNI_HC_GetObjectArrayElement:
    case ZB_JNI_HC_SetObjectArrayElement:
    case ZB_JNI_HC_NewPrimitiveArray:
    case ZB_JNI_HC_GetPrimitiveArrayRegion:
    case ZB_JNI_HC_SetPrimitiveArrayRegion:
    case ZB_JNI_HC_GetDirectBufferCapacity:
        return true;
    default:
        return false;
    }
}

}  // namespace

bool HostJni::handle_fast_host_call(std::uint32_t index, GuestThread& thread) {
    if (!jni_call_is_fast_leaf(index, thread)) return false;
    Impl& jni = *impl_;
    if (__builtin_expect(!jni.ready.load(std::memory_order_acquire), 0)) return false;
    // A pending Java exception has to go through the slow path, which holds it aside around this
    // call (see handle_host_call). CheckJNI aborts on a JNI call made while one is pending, and
    // guests built for releases without CheckJNI do exactly that all the time.
    const JniBackend::Env fast_env = jni.thread().env;
    if (fast_env != 0 && jni.backend.exception_check(fast_env)) return false;
    JniCall call(jni, thread, jni.thread(), index);
    return jni.serve_vm(call) || jni.serve_objects(call) || jni.serve_values(call) || jni.serve_data(call);
}

bool HostJni::handle_host_call(std::uint32_t index, GuestThread& thread) {
    if (index < ZB_JNI_SLOT_STUB_FIRST || index > ZB_JNI_HOST_CALL_LAST) return false;
    note_recent(index);
    Impl& jni = *impl_;
    if (index == ZB_JNI_HC_Register) {
        zb_jni_guest_api api{};
        const std::uint8_t* source = jni.runtime.memory().host_ptr(thread.regs()[0], sizeof api, kPageRead);
        if (source != nullptr) std::memcpy(&api, source, sizeof api);
        const bool valid = source != nullptr && api.size == sizeof api && api.version == ZB_JNI_PROTOCOL_VERSION &&
                           api.new_env_fn != 0 && api.free_env_fn != 0 && api.java_vm != 0;
        if (valid && !jni.ready.load()) {
            jni.api = api;
            jni.ready.store(true);
        } else if (!valid) {
            log("libzbjni.so registered an invalid or mismatched API");
        }
        thread.regs()[0] = valid ? 1 : 0;
        return true;
    }
    if (!jni.ready.load()) {
        const char* name = jni_host_call_name(index);
        log("JNI host call %s before libzbjni.so registered", name);
        abort_jni(std::string("host call ") + name + " before libzbjni.so registered");
    }
    JniCall call(jni, thread, jni.thread(), index);
    // A Java exception raised inside one guest call stays pending for the guest afterwards, exactly
    // as on a device: ART only delivers it back to managed code. Calling into ART while one is
    // pending is what CheckJNI aborts on - "JNI NewStringUTF called with pending exception" ended
    // Topsoil's run - while release ART quietly tolerates it. So the bridge holds the exception
    // aside for the duration of this call and puts it back afterwards: the guest still observes it
    // and one Java exception in an old game can no longer kill the process.
    const JniBackend::Env env = jni.thread().env;
    JniBackend::Ref held = 0;
    if (env != 0 && !jni_call_inspects_exception(index) && jni.backend.exception_check(env)) {
        held = jni.backend.exception_occurred(env);
        if (held != 0) {
            jni.backend.exception_clear(env);
            static std::atomic<unsigned> deferred{0};
            if (deferred.fetch_add(1) + 1 == 1) {
                std::string pending_class;
                if (!jni.backend.object_class_name(env, held, pending_class)) pending_class.clear();
                runtime_report().note_jni_detail(
                    "exception-deferred",
                    std::string(jni_host_call_name(index)) +
                        (pending_class.empty() ? " with an exception pending"
                                               : " while " + pending_class + " was pending"),
                    true);
            }
        }
    }
    const bool served = jni.serve_vm(call) || jni.serve_objects(call) || jni.serve_values(call) ||
                        jni.serve_data(call) || jni.serve_natives(call);
    if (held != 0) {
        // A call that throws its own exception wins: the guest is meant to see that one.
        if (!jni.backend.exception_check(env)) jni.backend.throw_exception(env, held);
        jni.backend.delete_local_ref(env, held);
    }
    if (served) return true;
    const char* name = jni_host_call_name(index);
    log("JNI host call 0x%x (%s) is not implemented", index, name);
    abort_jni(std::string("unimplemented JNI host call ") + name);
}

std::optional<HostJni::NativeResult> HostJni::call_native(JniBackend::Env env, char return_type,
                                                          std::uint32_t function, const BuildCall& build,
                                                          NativeCallCounter* call_counter) {
    // The single funnel for every Java -> guest native invocation: one relaxed atomic add per
    // call, no lock, no string work. call_counter is null for calls that are not dispatching a
    // bound native method (the loader's own JNI_OnLoad call), which are not counted.
    if (call_counter != nullptr) call_counter->count.fetch_add(1, std::memory_order_relaxed);
    Impl& jni = *impl_;
    JniThread& state = jni.thread();
    const JniBackend::Env outer_env = state.env;
    if (outer_env != 0 && outer_env != env) {
        log("JNI native call with a JNIEnv that differs from this thread's JNIEnv");
        abort_jni("native call JNIEnv mismatch");
    }
    if (!jni.ready.load() || !jni.ensure_guest_env(state)) {
        runtime_report().note_jni_detail(
            "native-call-aborted", !jni.ready.load() ? "host jni not ready" : "ensure_guest_env failed", true);
        return std::nullopt;
    }
    // The backend frame frees every host local created during the call in one step and keeps
    // the caller's (argument) references untouched.
    if (jni.backend.push_local_frame(env, 16) != 0) return std::nullopt;
    state.env = env;
    ++state.native_depth;
    const int outer_user_frames = state.user_frames;
    state.user_frames = 0;
    state.locals.push_frame();
    const std::size_t frames = state.locals.frame_count();

    const GuestCall args =
        build(state.guest_env, [&](std::uint64_t ref) { return state.locals.add(ref); });
    std::optional<GuestResult> guest = jni.invoke(state, function, args);

    // The one guest -> host sync point of the direct-buffer mirrors (host_jni_data.cpp): whatever
    // the guest wrote into a mirrored Java buffer becomes visible to Java here, when the native
    // method returns. A guest that writes to a mirror outside a Java -> guest native call (from a
    // guest thread of its own, say) is not seen by Java until the next native call returns, and a
    // later GetDirectBufferAddress for the same buffer overwrites those writes with Java's bytes.
    const bool exception_pending = jni.backend.exception_check(env);
    if (!exception_pending) jni.flush_buffer_mirrors(env);

    std::optional<NativeResult> result;
    JniBackend::Ref kept = 0;
    if (guest) {
        result = NativeResult{*guest, 0};
        if (return_type == 'L' && !exception_pending) {
            kept = jni.resolve(state, guest->r0, "native method result");
        }
    }
    if (state.locals.frame_count() != frames || state.user_frames != 0) {
        log("JNI: a native method returned with %d unpopped PushLocalFrame frames", state.user_frames);
        while (state.user_frames > 0) {
            state.locals.pop_frame();
            kept = jni.backend.pop_local_frame(env, kept);
            --state.user_frames;
        }
    }
    state.locals.pop_frame();
    const JniBackend::Ref outer = jni.backend.pop_local_frame(env, kept);
    if (result && return_type == 'L') result->ref = outer;
    state.user_frames = outer_user_frames;
    --state.native_depth;
    state.env = outer_env;
    return result;
}

}  // namespace zb
