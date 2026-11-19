#include "zb/runtime_report.h"

#include <algorithm>
#include <condition_variable>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>

#include "zb/log.h"

namespace zb {

namespace {

// One line per record: newlines and other control characters would break the "key: value"
// layout that makes the report diff-friendly.
std::string one_line(const std::string& text, std::size_t limit) {
    std::string out;
    out.reserve(text.size() < limit ? text.size() : limit);
    for (const char c : text) {
        if (out.size() >= limit) {
            out += "...";
            break;
        }
        out += (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) ? ' ' : c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string hex_version(std::int32_t version) {
    char text[11];
    std::snprintf(text, sizeof text, "0x%08x", static_cast<std::uint32_t>(version));
    return text;
}

std::string hex32(std::uint32_t value) {
    char text[11];
    std::snprintf(text, sizeof text, "0x%08x", value);
    return text;
}

void append_count(std::string& out, const char* key, std::uint64_t value) {
    out += key;
    out += ": ";
    out += std::to_string(value);
    out += '\n';
}

// Set by write_runtime_report_to(), read by runtime_report_path() and by the guest probe loader.
std::mutex g_report_path_mutex;
std::string g_report_path;

}  // namespace

std::string runtime_report_path() {
    std::lock_guard<std::mutex> lock(g_report_path_mutex);
    return g_report_path;
}

RuntimeReport::~RuntimeReport() {
    set_observer(nullptr);
}

RuntimeReport::Observers RuntimeReport::take_observers() const {
    return Observers{observer_, persist_observer_};
}

void RuntimeReport::notify_observers(const Observers& observers, bool structural, bool urgent) {
    if (observers.observer) (*observers.observer)(structural);
    if (observers.persist) (*observers.persist)(structural, urgent);
}

void RuntimeReport::set_observer(Observer observer) {
    Observers retired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        retired.observer = std::move(observer_);
        retired.persist = std::move(persist_observer_);
        observer_ = observer ? std::make_shared<Observer>(std::move(observer)) : nullptr;
    }
}

void RuntimeReport::note_plugin(const std::string& plugin_root, std::uint32_t target_sdk) {
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        plugin_root_ = one_line(plugin_root, kMaxDetail);
        target_sdk_ = target_sdk;
        observers = take_observers();
    }
    notify_observers(observers, true, true);
}

void RuntimeReport::note_unimplemented_host_call(std::uint32_t index, const char* library, const char* function) {
    bool structural = false;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++host_call_total_;
        HostCall* found = nullptr;
        for (HostCall& entry : host_calls_) {
            if (entry.index == index) {
                found = &entry;
                break;
            }
        }
        if (found != nullptr) {
            ++found->count;
        } else {
            ++distinct_host_calls_;
            structural = true;
            if (host_calls_.size() < kMaxDistinctHostCalls) {
                host_calls_.push_back(HostCall{index, library != nullptr ? library : "?",
                                               function != nullptr ? function : "?", 1});
            }
        }
        observers = take_observers();
    }
    notify_observers(observers, structural, true);
}

void RuntimeReport::note_proxy_loaded(const std::string& library, std::int32_t jni_version) {
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++proxy_load_total_;
        if (proxy_loads_.size() < kMaxLibraries) {
            proxy_loads_.push_back(Load{one_line(library, kMaxDetail), true, jni_version, {}});
        }
        observers = take_observers();
    }
    notify_observers(observers, true, true);
}

void RuntimeReport::note_proxy_failed(const std::string& library, const std::string& error) {
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++proxy_failure_total_;
        if (proxy_loads_.size() < kMaxLibraries) {
            proxy_loads_.push_back(Load{one_line(library, kMaxDetail), false, 0, one_line(error, kMaxDetail)});
        }
        observers = take_observers();
    }
    notify_observers(observers, true, true);
}

void RuntimeReport::note_jni_onload(const std::string& library, bool ok, std::int32_t jni_version) {
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++onload_total_;
        if (onloads_.size() < kMaxLibraries) {
            onloads_.push_back(Load{one_line(library, kMaxDetail), ok, jni_version, {}});
        }
        observers = take_observers();
    }
    notify_observers(observers, true, true);
}

void RuntimeReport::note_registered_native() {
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++registered_natives_;
        observers = take_observers();
    }
    notify_observers(observers, false, false);
}

NativeCallCounter& RuntimeReport::native_call_counter(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string label = one_line(name, 128);
    for (NativeCallEntry& entry : native_calls_) {
        if (entry.name == label) return *entry.counter;
    }
    if (native_calls_.size() >= kMaxNativeCalls) return native_calls_overflow_;
    native_calls_.push_back(NativeCallEntry{label, std::make_unique<NativeCallCounter>()});
    return *native_calls_.back().counter;
}

void RuntimeReport::note_guest_open(const std::string& path) {
    bool structural = false;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string line = one_line(path, kMaxDetail);
        auto found = std::find(opened_paths_.begin(), opened_paths_.end(), line);
        if (found != opened_paths_.end()) opened_paths_.erase(found);
        else structural = true;
        opened_paths_.push_back(line);
        if (opened_paths_.size() > kMaxOpenedPaths) opened_paths_.erase(opened_paths_.begin());
        observers = take_observers();
    }
    notify_observers(observers, structural, false);
}

void RuntimeReport::note_guest_open_failed(const std::string& path, int error) {
    Observers observers;
    bool structural = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++failed_open_count_;
        failed_opens_.emplace_back(one_line(path, kMaxDetail), error);
        if (failed_opens_.size() > kMaxFailedOpens) failed_opens_.erase(failed_opens_.begin());
        // Only the first few failures of a run are structural: a guest that retries a missing file
        // in a loop must not rewrite the whole report on every attempt.
        structural = failed_open_count_ <= kMaxFailedOpens;
        observers = take_observers();
    }
    notify_observers(observers, structural, false);
}

void RuntimeReport::note_guest_exit(const std::string& reason) {
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!exit_reason_.empty()) return;
        exit_reason_ = one_line(reason, kMaxDetail);
        if (exit_reason_.empty()) exit_reason_ = "unknown";
        observers = take_observers();
    }
    notify_observers(observers, true, true);
}

void RuntimeReport::note_syscall_eintr(std::uint32_t nr, const char* name,
                                       std::uint64_t guest_tid, bool restartable) {
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++signal_eintr_total_;
        ++(restartable ? signal_restartable_total_ : signal_nonrestartable_total_);
        signal_last_eintr_ = one_line(std::string(name != nullptr ? name : "?") + "(" +
                                          std::to_string(nr) + ") tid=" +
                                          std::to_string(guest_tid) + " restartable=" +
                                          (restartable ? "yes" : "no"),
                                      kMaxDetail);
        observers = take_observers();
    }
    notify_observers(observers, false, false);
}

void RuntimeReport::note_signal_restart(std::uint32_t nr, const char* name,
                                        std::uint64_t guest_tid, bool sa_restart, bool rewound) {
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++(sa_restart ? signal_delivered_sa_restart_total_ : signal_delivered_no_restart_total_);
        if (rewound) ++signal_rewound_total_;
        signal_last_delivery_ = one_line(std::string(name != nullptr ? name : "?") + "(" +
                                             std::to_string(nr) + ") tid=" +
                                             std::to_string(guest_tid) + " sa-restart=" +
                                             (sa_restart ? "yes" : "no") + " rewound=" +
                                             (rewound ? "yes" : "no"),
                                         kMaxDetail);
        observers = take_observers();
    }
    notify_observers(observers, false, false);
}

RuntimeReport::FutexWord* RuntimeReport::futex_word_locked(std::uint32_t address) {
    for (FutexWord& word : futex_words_) {
        if (word.address == address) return &word;
    }
    if (futex_words_.size() < kMaxFutexAddresses) {
        futex_words_.push_back(FutexWord{});
        futex_words_.back().address = address;
        return &futex_words_.back();
    }
    // Parked words are what the report exists to show, so an idle entry gives its slot up. Its
    // history folds into the overflow entry rather than disappearing, which keeps the per-word
    // counts summing to the totals.
    for (FutexWord& word : futex_words_) {
        if (word.live != 0) continue;
        futex_overflow_.entered += word.entered;
        futex_overflow_.returned += word.returned;
        futex_overflow_.timeouts += word.timeouts;
        futex_overflow_.wakes += word.wakes;
        futex_overflow_.woken += word.woken;
        futex_overflow_.empty_wakes += word.empty_wakes;
        futex_overflow_.live += word.live;
        word = FutexWord{};
        word.address = address;
        return &word;
    }
    return nullptr;
}

void RuntimeReport::note_futex_wait(std::uint32_t guest_address, std::uint32_t expected,
                                    std::uint64_t guest_tid) {
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(futex_mutex_);
        FutexWord* word = futex_word_locked(guest_address);
        if (word == nullptr) word = &futex_overflow_;
        ++word->entered;
        ++word->live;
        word->last_expected = expected;
        word->last_tid = guest_tid;
        ++futex_entered_total_;
        ++futex_live_total_;
        notify = (futex_entered_total_ & 4095u) == 0;
    }
    if (!notify) return;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        observers = take_observers();
    }
    notify_observers(observers, false, false);
}

void RuntimeReport::note_futex_returned(std::uint32_t guest_address, std::int32_t result) {
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(futex_mutex_);
        FutexWord* word = futex_word_locked(guest_address);
        if (word == nullptr) word = &futex_overflow_;
        ++word->returned;
        ++futex_returned_total_;
        if (result == -ETIMEDOUT) {
            ++word->timeouts;
            ++futex_timeout_total_;
        }
        // A wait that returned is no longer parked. Clamping keeps the count honest if a caller
        // ever reports a return it did not report a wait for.
        if (word->live > 0) {
            --word->live;
            --futex_live_total_;
        }
        notify = (futex_returned_total_ & 4095u) == 0;
    }
    if (!notify) return;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        observers = take_observers();
    }
    notify_observers(observers, false, false);
}

void RuntimeReport::note_futex_wake(std::uint32_t guest_address, std::int32_t woken) {
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(futex_mutex_);
        FutexWord* word = futex_word_locked(guest_address);
        if (word == nullptr) word = &futex_overflow_;
        ++word->wakes;
        ++futex_wake_total_;
        if (woken > 0) {
            const std::uint64_t count = static_cast<std::uint64_t>(woken);
            word->woken += count;
            futex_woken_total_ += count;
        } else {
            ++word->empty_wakes;
            ++futex_empty_wake_total_;
        }
        notify = (futex_wake_total_ & 4095u) == 0;
    }
    if (!notify) return;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        observers = take_observers();
    }
    notify_observers(observers, false, false);
}

void RuntimeReport::note_gl_call(const char* function, std::uint64_t host_tid) {
    // Hot path: count every GL call with one relaxed atomic increment and zero mutexes.
    // Only the very first call needs the lock to record gl-first-call structurally.
    const std::uint64_t prev = gl_call_total_.fetch_add(1, std::memory_order_relaxed);
    if (prev != 0) return;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gl_first_call_function_.empty()) {
            gl_first_call_function_ = function != nullptr ? function : "?";
            gl_first_call_tid_ = host_tid;
        }
        observers = take_observers();
    }
    notify_observers(observers, true, false);
}

void RuntimeReport::note_gl_egl_context(bool current) {
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gl_egl_context_known_) return;
        gl_egl_context_known_ = true;
        gl_egl_context_current_ = current;
        observers = take_observers();
    }
    notify_observers(observers, true, false);
}

void RuntimeReport::note_gl_error(const char* function, std::uint32_t error) {
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gl_error_known_) return;
        gl_error_known_ = true;
        gl_error_function_ = function != nullptr ? function : "?";
        gl_error_value_ = error;
        observers = take_observers();
    }
    notify_observers(observers, true, true);
}

void RuntimeReport::note_gl_detail(const std::string& key, const std::string& value, bool overwrite) {
    bool structural = false;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string line = one_line(value, 600);
        auto found = std::find_if(gl_details_.begin(), gl_details_.end(),
                                  [&](const auto& entry) { return entry.first == key; });
        if (found != gl_details_.end()) {
            if (!overwrite || found->second == line) return;
            found->second = line;
        } else {
            if (gl_details_.size() >= kMaxGlDetails) return;
            gl_details_.emplace_back(one_line(key, 64), line);
            structural = true;
        }
        observers = take_observers();
    }
    notify_observers(observers, structural, false);
}

void RuntimeReport::note_crash_detail(const std::string& key, const std::string& value) {
    bool structural = false;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string line = one_line(value, 600);
        auto found = std::find_if(crash_details_.begin(), crash_details_.end(),
                                  [&](const auto& entry) { return entry.first == key; });
        if (found != crash_details_.end()) {
            if (found->second == line) return;
            found->second = line;
        } else {
            if (crash_details_.size() >= kMaxCrashDetails) return;
            crash_details_.emplace_back(one_line(key, 64), line);
            structural = true;
        }
        observers = take_observers();
    }
    notify_observers(observers, structural, true);
}

void RuntimeReport::note_jni_detail(const std::string& key, const std::string& value, bool overwrite) {
    bool structural = false;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string line = one_line(value, 600);
        auto found = std::find_if(jni_details_.begin(), jni_details_.end(),
                                  [&](const auto& entry) { return entry.first == key; });
        if (found != jni_details_.end()) {
            if (!overwrite || found->second == line) return;
            found->second = line;
        } else {
            if (jni_details_.size() >= kMaxJniDetails) return;
            jni_details_.emplace_back(one_line(key, 64), line);
            structural = true;
        }
        observers = take_observers();
    }
    notify_observers(observers, structural, false);
}

void RuntimeReport::note_slow_host_call(const std::string& name, std::uint64_t guest_tid,
                                        std::uint64_t ms) {
    bool structural = false;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++slow_host_call_total_;
        if (slow_host_calls_.size() < kMaxSlowHostCalls) {
            slow_host_calls_.push_back(one_line(name, 96) + " tid=" + std::to_string(guest_tid) +
                                       " ms=" + std::to_string(ms));
            structural = true;
        }
        observers = take_observers();
    }
    notify_observers(observers, structural, false);
}

void RuntimeReport::note_audio_detail(const std::string& key, const std::string& value,
                                      bool overwrite) {
    bool structural = false;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string line = one_line(value, 240);
        auto found = std::find_if(audio_details_.begin(), audio_details_.end(),
                                  [&](const auto& entry) { return entry.first == key; });
        if (found != audio_details_.end()) {
            if (!overwrite || found->second == line) return;
            found->second = line;
        } else {
            if (audio_details_.size() >= kMaxAudioDetails) return;
            audio_details_.emplace_back(one_line(key, 64), line);
            structural = true;
        }
        observers = take_observers();
    }
    notify_observers(observers, structural, false);
}

void RuntimeReport::note_looper_detail(const std::string& key, const std::string& value, bool overwrite) {
    bool structural = false;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string line = one_line(value, 600);
        auto found = std::find_if(looper_details_.begin(), looper_details_.end(),
                                  [&](const auto& entry) { return entry.first == key; });
        if (found != looper_details_.end()) {
            if (!overwrite || found->second == line) return;
            found->second = line;
        } else {
            if (looper_details_.size() >= kMaxLooperDetails) return;
            looper_details_.emplace_back(one_line(key, 64), line);
            structural = true;
        }
        observers = take_observers();
    }
    notify_observers(observers, structural, false);
}

void RuntimeReport::note_watch_detail(const std::string& key, const std::string& value) {
    bool structural = false;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Longer than the other detail sections: the thread census and the per-thread stack chains
        // are meant to be read, not parsed, and one line holds a whole thread's evidence. The stuck
        // list shows up to twelve threads, each with its state, name and PC.
        const std::string line = one_line(value, 1600);
        auto found = std::find_if(watch_details_.begin(), watch_details_.end(),
                                  [&](const auto& entry) { return entry.first == key; });
        if (found != watch_details_.end()) {
            if (found->second == line) return;
            found->second = line;
        } else {
            if (watch_details_.size() >= kMaxWatchDetails) return;
            watch_details_.emplace_back(one_line(key, 64), line);
            structural = true;
        }
        observers = take_observers();
    }
    notify_observers(observers, structural, false);
}

void RuntimeReport::note_guest_log_captured() {
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++guest_log_total_;
        observers = take_observers();
    }
    notify_observers(observers, false, false);
}

void RuntimeReport::note_guest_log_line(const std::string& line) {
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        guest_log_.push_back(one_line(line, 220));
        if (guest_log_.size() > kMaxGuestLog) guest_log_.erase(guest_log_.begin());
        observers = take_observers();
    }
    notify_observers(observers, false, false);
}

void RuntimeReport::note_io_detail(const std::string& key, const std::string& value) {
    bool structural = false;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string line = one_line(value, 240);
        auto found = std::find_if(io_details_.begin(), io_details_.end(),
                                  [&](const auto& entry) { return entry.first == key; });
        if (found != io_details_.end()) {
            if (found->second == line) return;
            found->second = line;
        } else {
            if (io_details_.size() >= kMaxIoDetails) return;
            io_details_.emplace_back(one_line(key, 64), line);
            structural = true;
        }
        observers = take_observers();
    }
    notify_observers(observers, structural, false);
}

std::uint64_t RuntimeReport::guest_log_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return guest_log_total_;
}

std::uint64_t RuntimeReport::egl_swaps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return egl_swap_total_;
}

void RuntimeReport::note_egl_object(const std::string& key, const std::string& value) {
    bool structural = false;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string line = one_line(value, 600);
        auto found = std::find_if(egl_objects_.begin(), egl_objects_.end(),
                                  [&](const auto& entry) { return entry.first == key; });
        if (found != egl_objects_.end()) {
            found->second = line;
        } else {
            if (egl_objects_.size() >= kMaxGlDetails) return;
            egl_objects_.emplace_back(one_line(key, 64), line);
            structural = true;
        }
        observers = take_observers();
    }
    notify_observers(observers, structural, false);
}

void RuntimeReport::note_egl_current(std::uint64_t host_tid) {
    egl_current_generation_.fetch_add(1, std::memory_order_relaxed);
    bool structural = false;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(egl_current_tids_.begin(), egl_current_tids_.end(), host_tid) == egl_current_tids_.end()) {
            if (egl_current_tids_.size() < kMaxEglThreads) egl_current_tids_.push_back(host_tid);
            structural = true;
        }
        observers = take_observers();
    }
    notify_observers(observers, structural, false);
}

void RuntimeReport::note_gl_thread(std::uint64_t host_tid) {
    bool structural = false;
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(gl_thread_tids_.begin(), gl_thread_tids_.end(), host_tid) == gl_thread_tids_.end()) {
            if (gl_thread_tids_.size() < kMaxEglThreads) gl_thread_tids_.push_back(host_tid);
            structural = true;
        }
        observers = take_observers();
    }
    notify_observers(observers, structural, false);
}

void RuntimeReport::note_egl_swap() {
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++egl_swap_total_;
        observers = take_observers();
    }
    notify_observers(observers, false, false);
}

void RuntimeReport::note_egl_error(const char* function, std::uint32_t error) {
    Observers observers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (egl_error_known_) return;
        egl_error_known_ = true;
        egl_error_function_ = function != nullptr ? function : "?";
        egl_error_value_ = error;
        observers = take_observers();
    }
    notify_observers(observers, true, true);
}

std::uint64_t RuntimeReport::egl_current_generation() const {
    return egl_current_generation_.load(std::memory_order_relaxed);
}

std::size_t RuntimeReport::unimplemented_host_calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(host_call_total_);
}

std::size_t RuntimeReport::proxy_loads() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return proxy_load_total_;
}

std::size_t RuntimeReport::jni_onload_calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return onload_total_;
}

std::size_t RuntimeReport::registered_natives() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(registered_natives_);
}

std::string RuntimeReport::first_unimplemented_host_call() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (host_calls_.empty()) return {};
    return std::string(host_calls_.front().library) + " " + host_calls_.front().function;
}

std::uint64_t RuntimeReport::gl_calls() const {
    return gl_call_total_.load(std::memory_order_relaxed);
}

std::string RuntimeReport::text() const {
    std::vector<FutexWord> futex_words_snapshot;
    FutexWord futex_overflow_snapshot;
    std::uint64_t futex_entered_snapshot = 0;
    std::uint64_t futex_returned_snapshot = 0;
    std::uint64_t futex_timeout_snapshot = 0;
    std::uint64_t futex_live_snapshot = 0;
    std::uint64_t futex_wake_snapshot = 0;
    std::uint64_t futex_woken_snapshot = 0;
    std::uint64_t futex_empty_wake_snapshot = 0;
    {
        std::lock_guard<std::mutex> futex_lock(futex_mutex_);
        futex_words_snapshot = futex_words_;
        futex_overflow_snapshot = futex_overflow_;
        futex_entered_snapshot = futex_entered_total_;
        futex_returned_snapshot = futex_returned_total_;
        futex_timeout_snapshot = futex_timeout_total_;
        futex_live_snapshot = futex_live_total_;
        futex_wake_snapshot = futex_wake_total_;
        futex_woken_snapshot = futex_woken_total_;
        futex_empty_wake_snapshot = futex_empty_wake_total_;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out = "zettabridge-runtime-report 1\n";
    out += "plugin: ";
    out += plugin_root_.empty() ? "(none)" : plugin_root_;
    if (!plugin_root_.empty()) out += " targetSdk " + std::to_string(target_sdk_);
    out += '\n';

    append_count(out, "proxy-loads", proxy_load_total_);
    append_count(out, "proxy-failures", proxy_failure_total_);
    for (const Load& load : proxy_loads_) {
        out += load.ok ? "proxy-loaded: " : "proxy-failed: ";
        out += load.library;
        if (load.ok) {
            out += " jni=" + hex_version(load.jni_version);
        } else {
            out += ' ';
            out += load.error;
        }
        out += '\n';
    }
    if (proxy_load_total_ + proxy_failure_total_ > proxy_loads_.size()) {
        append_count(out, "proxy-more", proxy_load_total_ + proxy_failure_total_ - proxy_loads_.size());
    }

    append_count(out, "jni-onload-calls", onload_total_);
    for (const Load& load : onloads_) {
        out += "jni-onload: ";
        out += load.library;
        out += load.ok ? " ok jni=" + hex_version(load.jni_version) : std::string(" failed");
        out += '\n';
    }
    if (onload_total_ > onloads_.size()) append_count(out, "jni-onload-more", onload_total_ - onloads_.size());

    append_count(out, "registered-natives", registered_natives_);
    append_count(out, "unimplemented-host-calls", host_call_total_);
    append_count(out, "unimplemented-distinct", distinct_host_calls_);
    out += "first-unimplemented: ";
    if (host_calls_.empty()) {
        out += "(none)";
    } else {
        out += host_calls_.front().library;
        out += ' ';
        out += host_calls_.front().function;
    }
    out += '\n';
    for (const HostCall& entry : host_calls_) {
        out += "unimplemented: ";
        out += entry.library;
        out += ' ';
        out += entry.function;
        out += " x" + std::to_string(entry.count);
        out += '\n';
    }
    if (distinct_host_calls_ > host_calls_.size()) {
        append_count(out, "unimplemented-more", distinct_host_calls_ - host_calls_.size());
    }

    out += "host-slow-calls: " + std::to_string(slow_host_call_total_) + '\n';
    for (std::size_t i = 0; i < slow_host_calls_.size(); ++i) {
        out += "host-slow-" + std::to_string(i + 1) + ": " + slow_host_calls_[i] + '\n';
    }

    out += "guest-exit: ";
    out += exit_reason_.empty() ? "(none)" : exit_reason_;
    out += '\n';

    out += "signal-restart: eintr=" + std::to_string(signal_eintr_total_) +
           " restartable=" + std::to_string(signal_restartable_total_) +
           " nonrestartable=" + std::to_string(signal_nonrestartable_total_) +
           " delivered-sa-restart=" + std::to_string(signal_delivered_sa_restart_total_) +
           " delivered-no-restart=" + std::to_string(signal_delivered_no_restart_total_) +
           " rewound=" + std::to_string(signal_rewound_total_) + '\n';
    if (!signal_last_eintr_.empty()) out += "signal-restart-last-eintr: " + signal_last_eintr_ + '\n';
    if (!signal_last_delivery_.empty()) {
        out += "signal-restart-last-delivery: " + signal_last_delivery_ + '\n';
    }

    out += "futex-waits: entered=" + std::to_string(futex_entered_snapshot) +
           " returned=" + std::to_string(futex_returned_snapshot) +
           " live=" + std::to_string(futex_live_snapshot) +
           " timeouts=" + std::to_string(futex_timeout_snapshot) + '\n';
    out += "futex-wakes: called=" + std::to_string(futex_wake_snapshot) +
           " woken=" + std::to_string(futex_woken_snapshot) +
           " empty=" + std::to_string(futex_empty_wake_snapshot) + '\n';
    {
        const auto append_futex_word = [&out](const std::string& address, const FutexWord& word) {
            out += "futex: addr=" + address + " live=" + std::to_string(word.live) +
                   " entered=" + std::to_string(word.entered) +
                   " returned=" + std::to_string(word.returned) +
                   " timeouts=" + std::to_string(word.timeouts) +
                   " wakes=" + std::to_string(word.wakes) +
                   " woken=" + std::to_string(word.woken) +
                   " empty=" + std::to_string(word.empty_wakes) +
                   " expect=" + hex32(word.last_expected) +
                   " tid=" + std::to_string(word.last_tid) + '\n';
        };
        for (const FutexWord& word : futex_words_snapshot) {
            append_futex_word(hex32(word.address), word);
        }
        if (futex_overflow_snapshot.entered != 0 || futex_overflow_snapshot.wakes != 0) {
            append_futex_word("overflow", futex_overflow_snapshot);
        }
    }

    {
        std::vector<std::pair<const std::string*, std::uint64_t>> counts;
        std::uint64_t total = 0;
        for (const NativeCallEntry& entry : native_calls_) {
            const std::uint64_t n = entry.counter->count.load(std::memory_order_relaxed);
            total += n;
            counts.emplace_back(&entry.name, n);
        }
        total += native_calls_overflow_.count.load(std::memory_order_relaxed);
        std::sort(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        out += "native-calls:";
        std::size_t shown = 0;
        for (const auto& [name, count] : counts) {
            if (shown >= 8 || count == 0) break;
            out += " " + *name + "=" + std::to_string(count);
            ++shown;
        }
        if (shown == 0) out += " (none)";
        out += " total=" + std::to_string(total) + '\n';
    }

    if (opened_paths_.empty()) {
        out += "opened-1: (none)\n";
    } else {
        std::size_t n = 1;
        for (const std::string& path : opened_paths_) out += "opened-" + std::to_string(n++) + ": " + path + '\n';
    }
    {
        std::size_t n = 1;
        for (const auto& [path, error] : failed_opens_) {
            out += "open-failed-" + std::to_string(n++) + ": " + path + " errno=" + std::to_string(error) + '\n';
        }
    }
    append_count(out, "open-failures-total", failed_open_count_);

    for (const auto& [key, value] : io_details_) out += "io-" + key + ": " + value + '\n';
    append_count(out, "guest-log-records", guest_log_total_);
    {
        std::size_t n = 1;
        for (const std::string& line : guest_log_) {
            out += "guest-log-" + std::to_string(n++) + ": " + line + '\n';
        }
    }

    const std::uint64_t gl_calls_count = gl_call_total_.load(std::memory_order_relaxed);
    append_count(out, "gl-calls", gl_calls_count);
    out += "gl-first-call: ";
    if (gl_calls_count == 0 || gl_first_call_function_.empty()) {
        out += "(none)";
    } else {
        out += gl_first_call_function_;
        out += " tid=" + std::to_string(gl_first_call_tid_);
    }
    out += '\n';
    out += "gl-egl-context-current: ";
    out += !gl_egl_context_known_ ? "(unknown)" : (gl_egl_context_current_ ? "yes" : "no");
    out += '\n';
    out += "gl-first-error: ";
    if (!gl_error_known_) {
        out += "(none)";
    } else {
        char hex[11];
        std::snprintf(hex, sizeof hex, "0x%04x", gl_error_value_);
        out += gl_error_function_ + " " + hex;
    }
    out += '\n';
    for (const auto& [key, value] : gl_details_) out += "gl-" + key + ": " + value + '\n';

    for (const auto& [key, value] : egl_objects_) out += "egl-" + key + ": " + value + '\n';
    append_count(out, "egl-swaps", egl_swap_total_);
    // A mismatch is a thread that issued a gl* call having never made an EGL context current on
    // itself. A raster thread and a resource thread each with their own (correct) context both
    // appear in egl_current_tids_, so this does not fire for that normal multi-context case.
    for (const std::uint64_t tid : gl_thread_tids_) {
        if (std::find(egl_current_tids_.begin(), egl_current_tids_.end(), tid) != egl_current_tids_.end()) continue;
        out += "egl-thread-mismatch: current=" +
               std::to_string(egl_current_tids_.empty() ? 0 : egl_current_tids_.back()) +
               " gl=" + std::to_string(tid) + '\n';
        break;
    }
    out += "egl-first-error: ";
    if (!egl_error_known_) {
        out += "(none)";
    } else {
        char hex[11];
        std::snprintf(hex, sizeof hex, "0x%04x", egl_error_value_);
        out += egl_error_function_ + " " + hex;
    }
    out += '\n';
    for (const auto& [key, value] : jni_details_) out += "jni-" + key + ": " + value + '\n';
    for (const auto& [key, value] : crash_details_) out += "crash-" + key + ": " + value + '\n';
    for (const auto& [key, value] : watch_details_) out += "watch-" + key + ": " + value + '\n';
    for (const auto& [key, value] : looper_details_) out += "looper-" + key + ": " + value + '\n';
    for (const auto& [key, value] : audio_details_) out += "audio-" + key + ": " + value + '\n';
    return out;
}

void RuntimeReport::clear() {
    Observers retired;
    {
        std::scoped_lock lock(mutex_, futex_mutex_);
        retired.observer = std::move(observer_);
        retired.persist = std::move(persist_observer_);
    plugin_root_.clear();
    target_sdk_ = 0;
    host_calls_.clear();
    distinct_host_calls_ = 0;
    host_call_total_ = 0;
    proxy_loads_.clear();
    proxy_load_total_ = 0;
    proxy_failure_total_ = 0;
    onloads_.clear();
    onload_total_ = 0;
    registered_natives_ = 0;
    exit_reason_.clear();
    signal_eintr_total_ = 0;
    signal_restartable_total_ = 0;
    signal_nonrestartable_total_ = 0;
    signal_delivered_sa_restart_total_ = 0;
    signal_delivered_no_restart_total_ = 0;
    signal_rewound_total_ = 0;
    signal_last_eintr_.clear();
    signal_last_delivery_.clear();
    futex_words_.clear();
    futex_overflow_ = FutexWord{};
    futex_entered_total_ = 0;
    futex_returned_total_ = 0;
    futex_timeout_total_ = 0;
    futex_live_total_ = 0;
    futex_wake_total_ = 0;
    futex_woken_total_ = 0;
    futex_empty_wake_total_ = 0;
    native_calls_.clear();
    native_calls_overflow_.count.store(0, std::memory_order_relaxed);
    opened_paths_.clear();
    failed_opens_.clear();
    failed_open_count_ = 0;
    gl_call_total_.store(0, std::memory_order_relaxed);
    gl_first_call_function_.clear();
    gl_first_call_tid_ = 0;
    gl_egl_context_known_ = false;
    gl_egl_context_current_ = false;
    gl_error_known_ = false;
    gl_error_function_.clear();
    gl_error_value_ = 0;
    gl_details_.clear();
    egl_objects_.clear();
    egl_current_tids_.clear();
    egl_current_generation_.store(0, std::memory_order_relaxed);
    gl_thread_tids_.clear();
    egl_swap_total_ = 0;
    egl_error_known_ = false;
    egl_error_function_.clear();
    egl_error_value_ = 0;
    crash_details_.clear();
    jni_details_.clear();
    watch_details_.clear();
    looper_details_.clear();
    audio_details_.clear();
    io_details_.clear();
    guest_log_.clear();
    guest_log_total_ = 0;
    slow_host_calls_.clear();
    slow_host_call_total_ = 0;
    }
}

RuntimeReport& runtime_report() {
    // Process-lifetime: notes arrive from guest threads that are never torn down, so this must
    // outlive every static destructor.
    static RuntimeReport* report = new RuntimeReport();
    return *report;
}

namespace {

// Rewrites one file from a RuntimeReport. Urgent lifecycle/crash notes flush synchronously on the
// caller, while high-frequency telemetry (audio/GL/JNI/futex/watchdog) wakes a background writer
// thread so ::fsync() and ::rename() never stall real-time audio or rendering.
class ReportWriter {
public:
    using Clock = std::chrono::steady_clock;

    ReportWriter(RuntimeReport& report, std::string path, std::chrono::milliseconds min_interval)
        : report_(report), path_(std::move(path)), temporary_(path_ + ".tmp"), min_interval_(min_interval) {}

    ~ReportWriter() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (pending_on_stop_.load(std::memory_order_relaxed)) write_now();
    }

    void start_async() {
        worker_ = std::thread([this]() { run_async(); });
    }

    bool write_now() {
        std::lock_guard<std::mutex> write_lock(write_mutex_);
        const std::string text = report_.text();
        const int fd = ::open(temporary_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) return false;
        std::size_t written = 0;
        while (written < text.size()) {
            const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
            if (n <= 0) {
                if (errno == EINTR) continue;
                ::close(fd);
                ::unlink(temporary_.c_str());
                return false;
            }
            written += static_cast<std::size_t>(n);
        }
        ::fsync(fd);
        ::close(fd);
        if (::rename(temporary_.c_str(), path_.c_str()) != 0) {
            ::unlink(temporary_.c_str());
            return false;
        }
        return true;
    }

    void operator()(bool structural, bool urgent) {
        if (!urgent) {
            // Fast skip when a non-urgent write is already queued: avoids taking mutex_ on
            // every 60fps eglSwapBuffers or futex pulse once a flush is pending.
            if (!structural && queued_non_structural_.load(std::memory_order_acquire)) return;
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = true;
            if (structural) {
                pending_structural_ = true;
                queued_non_structural_.store(false, std::memory_order_release);
            } else {
                queued_non_structural_.store(true, std::memory_order_release);
            }
            pending_on_stop_.store(true, std::memory_order_release);
            cv_.notify_one();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const Clock::time_point now = Clock::now();
            if (!structural && now - last_ < min_interval_) {
                // Queue a deferred async flush instead of silently dropping the update.
                pending_ = true;
                queued_non_structural_.store(true, std::memory_order_release);
                pending_on_stop_.store(true, std::memory_order_release);
                cv_.notify_one();
                return;
            }
            last_ = now;
            pending_ = false;
            pending_structural_ = false;
            queued_non_structural_.store(false, std::memory_order_release);
            pending_on_stop_.store(false, std::memory_order_release);
        }
        write_now();
    }

private:
    void run_async() {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return stop_ || pending_; });
                if (stop_) break;
                if (pending_structural_) {
                    // Brief coalesce window so bursts (e.g., OpenSL call census) become one disk write.
                    const auto batch = std::min(min_interval_, std::chrono::milliseconds(15));
                    if (batch.count() > 0) cv_.wait_for(lock, batch, [&] { return stop_; });
                } else if (last_ != Clock::time_point::min()) {
                    const Clock::time_point due = last_ + min_interval_;
                    cv_.wait_until(lock, due, [&] { return stop_ || pending_structural_; });
                }
                if (stop_) break;
                pending_ = false;
                pending_structural_ = false;
                queued_non_structural_.store(false, std::memory_order_relaxed);
                pending_on_stop_.store(false, std::memory_order_relaxed);
                last_ = Clock::now();
            }
            write_now();
        }
    }

    RuntimeReport& report_;
    std::string path_;
    std::string temporary_;
    std::chrono::milliseconds min_interval_;
    std::mutex mutex_;
    std::mutex write_mutex_;
    std::condition_variable cv_;
    Clock::time_point last_ = Clock::time_point::min();
    bool pending_ = false;
    bool pending_structural_ = false;
    bool stop_ = false;
    std::atomic<bool> queued_non_structural_{false};
    std::atomic<bool> pending_on_stop_{false};
    std::thread worker_;
};

}  // namespace

bool write_runtime_report_to(RuntimeReport& report, const std::string& path,
                             std::chrono::milliseconds min_interval) {
#if defined(ZB_NO_TELEMETRY)
    (void)report;
    (void)path;
    (void)min_interval;
    return true;
#else
    {
        std::lock_guard<std::mutex> lock(g_report_path_mutex);
        g_report_path = path;
    }
    auto writer = std::make_shared<ReportWriter>(report, path, min_interval);
    if (!writer->write_now()) {
        log("cannot write the runtime report to %s: %s", path.c_str(), std::strerror(errno));
        return false;
    }
    writer->start_async();
    RuntimeReport::Observers retired;
    {
        std::lock_guard<std::mutex> lock(report.mutex_);
        retired.persist = std::move(report.persist_observer_);
        report.persist_observer_ = std::make_shared<RuntimeReport::PersistObserver>(
            [writer](bool structural, bool urgent) { (*writer)(structural, urgent); });
    }
    return true;
#endif
}

}  // namespace zb
