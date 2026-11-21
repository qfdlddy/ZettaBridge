#include "zb/process.h"

#include <elf.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <exception>
#include <mutex>

#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iterator>
#include <string_view>
#include <thread>

#include <dynarmic/interface/exclusive_monitor.h>

#include "zb/elf_loader.h"
#include "zb/hang_watchdog.h"
#include "zb/initial_stack.h"
#include "zb/jni_protocol.h"
#include "zb/library_protocol.h"
#include "zb/log.h"
#include "zb/host_egl.h"
#include "zb/host_jni.h"

namespace {

// Fast monotonic milliseconds for >=1000ms slow-host-call detection: CLOCK_MONOTONIC_COARSE reads
// the kernel-updated vDSO page without hardware counter barriers on every GL/JNI/OpenSL host call.
inline std::uint64_t coarse_now_ms() {
#ifdef CLOCK_MONOTONIC_COARSE
    struct timespec ts {};
    if (::clock_gettime(CLOCK_MONOTONIC_COARSE, &ts) == 0) {
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000ULL +
               static_cast<std::uint64_t>(ts.tv_nsec) / 1'000'000ULL;
    }
#endif
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

#if defined(__aarch64__)
inline std::uint64_t host_call_counter_freq_hz() {
    static const std::uint64_t freq = [] {
        std::uint64_t val = 0;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
        return val;
    }();
    return freq;
}

inline std::uint64_t host_call_now_ticks() {
    std::uint64_t val = 0;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}
#endif

struct HostFpcrGuard {
#if defined(__aarch64__)
    std::uint64_t saved = 0;
    bool changed = false;
    HostFpcrGuard() {
        asm volatile("mrs %0, fpcr" : "=r"(saved));
        if (__builtin_expect(saved != 0, 0)) {
            asm volatile("msr fpcr, xzr" ::: "memory");
            changed = true;
        }
    }
    ~HostFpcrGuard() {
        if (__builtin_expect(changed, 0)) {
            asm volatile("msr fpcr, %0" :: "r"(saved) : "memory");
        }
    }
#endif
};

std::size_t worker_code_cache_size() {
    static const std::size_t size = [] {
        const char* v = std::getenv("ZB_WORKER_CODE_CACHE_MB");
        if (v != nullptr && v[0] != '\0') {
            char* end = nullptr;
            const unsigned long mb = std::strtoul(v, &end, 10);
            if (end != v && *end == '\0' && mb >= 8 && mb <= 128) {
                return static_cast<std::size_t>(mb) * 1024u * 1024u;
            }
        }
        return zb::kWorkerCodeCacheSize;
    }();
    return size;
}

}  // namespace
#include "zb/runtime_report.h"
#include "zb/syscalls.h"

namespace zb {

namespace {

// arch/arm/include/uapi/asm/hwcap.h
constexpr std::uint32_t kHwcapHalf = 1u << 1;
constexpr std::uint32_t kHwcapThumb = 1u << 2;
constexpr std::uint32_t kHwcapFastMult = 1u << 4;
constexpr std::uint32_t kHwcapVfp = 1u << 6;
constexpr std::uint32_t kHwcapEdsp = 1u << 7;
constexpr std::uint32_t kHwcapNeon = 1u << 12;
constexpr std::uint32_t kHwcapVfpv3 = 1u << 13;
constexpr std::uint32_t kHwcapTls = 1u << 15;
constexpr std::uint32_t kHwcapVfpv4 = 1u << 16;
constexpr std::uint32_t kHwcapIdiva = 1u << 17;
constexpr std::uint32_t kHwcapIdivt = 1u << 18;
constexpr std::uint32_t kHwcapVfpd32 = 1u << 19;
constexpr std::uint32_t kHwcapLpae = 1u << 20;
constexpr std::uint32_t kGuestHwcap = kHwcapHalf | kHwcapThumb | kHwcapFastMult | kHwcapVfp | kHwcapEdsp | kHwcapNeon |
                                      kHwcapVfpv3 | kHwcapTls | kHwcapVfpv4 | kHwcapIdiva | kHwcapIdivt |
                                      kHwcapVfpd32 | kHwcapLpae;

constexpr std::uint32_t kCpsrUserMode = 0x10;
constexpr std::uint32_t kCpsrThumb = 0x20;

// Linux clone flags (identical on arm and arm64).
constexpr std::uint32_t kCloneSetTls = 0x00080000;
constexpr std::uint32_t kCloneParentSetTid = 0x00100000;
constexpr std::uint32_t kCloneChildClearTid = 0x00200000;
constexpr std::uint32_t kCloneChildSetTid = 0x01000000;

constexpr std::uint64_t kSeenUnexpectedSvc = 1ULL << 40;
constexpr std::uint64_t kSeenHostCall = 1ULL << 41;

// svc #(0x5A0000 | index) from the generated stub libraries (tools/gen_stubs.py).
constexpr std::uint32_t kHostCallBase = 0x5A0000;

// svc #(0x5B0000 | index) from a guest code probe: the first instruction of a probed function
// entry is replaced by it. Diagnostics only, see Process::load_probe_requests().
constexpr std::uint32_t kProbeSwiBase = 0x5B0000;
// The IL2CPP arm32 prologue a probe replaces: push {..., lr}.
constexpr std::uint32_t kArmPushMask = 0xFFFF0000u;
constexpr std::uint32_t kArmPush = 0xE92D0000u;

struct HostCallName {
    std::uint32_t index;
    const char* library;
    const char* name;
};

constexpr HostCallName kHostCallNames[] = {
#include "gen/hostcalls.inc"
};

struct PathMapping {
    std::string_view guest_prefix;
    std::string_view sysroot_prefix;
};

// The runtime APEX paths collapse onto the bootstrap copies extracted into system/.
constexpr PathMapping kPathMappings[] = {
    {"/apex/com.android.runtime/lib/bionic/", "/system/lib/"},
    {"/apex/com.android.runtime/bin/", "/system/bin/"},
    {"/system/", "/system/"},
    {"/vendor/", "/vendor/"},
    {"/odm/", "/odm/"},
    {"/product/", "/product/"},
    {"/system_ext/", "/system_ext/"},
    {"/apex/", "/apex/"},
    {"/linkerconfig/", "/linkerconfig/"},
};

const char* exception_name(Dynarmic::A32::Exception e) {
    using E = Dynarmic::A32::Exception;
    switch (e) {
    case E::UndefinedInstruction: return "undefined instruction";
    case E::UnpredictableInstruction: return "unpredictable instruction";
    case E::DecodeError: return "decode error";
    case E::SendEvent: return "SEV";
    case E::SendEventLocal: return "SEVL";
    case E::WaitForInterrupt: return "WFI";
    case E::WaitForEvent: return "WFE";
    case E::Yield: return "YIELD";
    case E::Breakpoint: return "breakpoint";
    case E::PreloadData: return "PLD";
    case E::PreloadDataWithIntentToWrite: return "PLDW";
    case E::PreloadInstruction: return "PLI";
    case E::NoExecuteFault: return "jump to non-executable memory";
    }
    return "unknown exception";
}

void write_guest_u32(GuestMemory& mem, std::uint32_t addr, std::uint32_t value) {
    if (std::uint8_t* p = mem.host_ptr(addr, 4, kPageWrite)) std::memcpy(p, &value, 4);
}

// Linux ARM kernel user helpers (arch/arm/kernel/entry-armv.S, SMP variants), ARM mode and
// position independent. Encodings from the NDK assembler. Real ARMv5 (armeabi) code calls
// these for atomics and TLS. Version 3 advertises get_tls, cmpxchg and memory_barrier;
// cmpxchg64 (version 5) is not provided.
constexpr std::uint32_t kKuserPage = 0xFFFF0000;
constexpr std::uint32_t kKuserHostReturn[] = {
    0xef5affff,  // svc #0x5affff
};
constexpr std::uint32_t kKuserSigreturnCode[] = {
    0xe3a07077,  // mov r7, #119 (__NR_sigreturn)
    0xef000000,  // svc #0
};
constexpr std::uint32_t kKuserRtSigreturnCode[] = {
    0xe3a070ad,  // mov r7, #173 (__NR_rt_sigreturn)
    0xef000000,  // svc #0
};
constexpr std::uint32_t kKuserCmpxchgBody[] = {
    0xf57ff05b,  //    dmb ish
    0xe1923f9f,  // 1: ldrex r3, [r2]
    0xe0533000,  //    subs r3, r3, r0
    0x01823f91,  //    strexeq r3, r1, [r2]
    0x03330001,  //    teqeq r3, #1
    0x0afffffa,  //    beq 1b
    0xf57ff05b,  //    dmb ish
    0xe2730000,  //    rsbs r0, r3, #0
    0xe12fff1e,  //    bx lr
};
constexpr std::uint32_t kKuserMemoryBarrier[] = {
    0xf57ff05b,  // dmb ish
    0xe12fff1e,  // bx lr
};
constexpr std::uint32_t kKuserCmpxchg[] = {
    0xeaffffde,  // b 0xffff0f40 (kKuserCmpxchgBody)
};
constexpr std::uint32_t kKuserGetTls[] = {
    0xee1d0f70,  // mrc p15, 0, r0, c13, c0, 3
    0xe12fff1e,  // bx lr
};
constexpr std::uint32_t kKuserHelperVersion = 3;

bool map_kuser_page(GuestMemory& mem) {
    if (!mem.map_anon(kKuserPage, kPageSize, PROT_READ | PROT_WRITE)) return false;
    const auto put = [&](std::uint32_t offset, const std::uint32_t* words, std::size_t count) {
        std::memcpy(mem.base() + kKuserPage + offset, words, count * sizeof(std::uint32_t));
    };
    put(0xf00, kKuserHostReturn, std::size(kKuserHostReturn));
    put(0xf20, kKuserSigreturnCode, std::size(kKuserSigreturnCode));
    put(0xf30, kKuserRtSigreturnCode, std::size(kKuserRtSigreturnCode));
    put(0xf40, kKuserCmpxchgBody, std::size(kKuserCmpxchgBody));
    put(0xfa0, kKuserMemoryBarrier, std::size(kKuserMemoryBarrier));
    put(0xfc0, kKuserCmpxchg, std::size(kKuserCmpxchg));
    put(0xfe0, kKuserGetTls, std::size(kKuserGetTls));
    put(0xffc, &kKuserHelperVersion, 1);
    return mem.protect(kKuserPage, kPageSize, PROT_READ | PROT_EXEC);
}

}  // namespace

// Public so the watchdog can name a stuck host call instead of printing its index: a frozen-device
// report is read without the generated table at hand.
std::pair<const char*, const char*> host_call_name(std::uint32_t index) {
    for (const auto& host_call : kHostCallNames) {
        if (host_call.index == index) return {host_call.library, host_call.name};
    }
    // The library runtime serves 0xFE00-0xFEFF inside the runtime itself, so those calls are in
    // neither generated table. A carrier park lasts exactly as long as the parked carrier's guest
    // identity is borrowed by a Java thread - minutes, not milliseconds - so it must not read as an
    // anonymous multi-minute stall in the report.
    if (index == ZB_SERVICE_READY_INDEX) return {"guest runtime", "ready"};
    if (index == ZB_CARRIER_PARK_INDEX) return {"guest runtime", "carrier-park"};
    // The flat JNI host calls (0xFB00+) are generated into their own table, so without this
    // fallback every one of them would print as an unhelpful "? ?". A JNI call is not a stub
    // library call, but naming its guest library keeps the report's two fields meaningful.
    if (index >= ZB_JNI_SLOT_STUB_FIRST) {
        const char* jni_name = jni_host_call_name(index);
        if (std::strcmp(jni_name, "?") != 0) return {"libzbjni.so", jni_name};
    }
    return {"?", "?"};
}

bool host_call_is_lease(std::uint32_t index) {
    // The parked carrier runs no guest code itself; it sleeps until the Java thread that borrowed
    // its identity finishes with it, which can be the whole run. Its duration therefore says how
    // long that Java thread lived, not how long a guest waited.
    return index == ZB_CARRIER_PARK_INDEX;
}

namespace {
std::atomic<int> g_precise_faults_override{-1};
}  // namespace

void set_precise_faults_override(bool enabled) {
    g_precise_faults_override.store(enabled ? 1 : 0, std::memory_order_release);
}

Process::Process() : monitor_(std::make_unique<Dynarmic::ExclusiveMonitor>(kMaxThreads)) {
    const int override_val = g_precise_faults_override.load(std::memory_order_acquire);
    if (override_val >= 0) {
        precise_faults_ = override_val != 0;
    } else if (const char* precise = std::getenv("ZB_PRECISE_FAULTS")) {
        precise_faults_ = precise[0] == '1';
    }
}

Process::~Process() {
    if (reporter_process == this) reporter_process = nullptr;
}

void Process::request_exit(int status) {
    exit_status_ = status;
    exiting_ = true;
    runtime_report().note_guest_exit("guest exited with status " + std::to_string(status));
}

void Process::invalidate(std::uint32_t addr, std::uint64_t len) {
    if (len == 0) return;
    record_jit_invalidate();
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (GuestThread* t : threads_) t->invalidate(addr, len);
    for (GuestThread* t : borrowers_) t->invalidate(addr, len);
}

void Process::post_process_signal(GuestThread& fallback, const g::siginfo32& info) {
    const int sig = info.si_signo;
    if (sig < 1 || sig > 64) return;
    const std::uint64_t bit = 1ULL << (sig - 1);
    if ((fallback.sigmask & bit) == 0) {
        fallback.post_signal(info);
        return;
    }
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (GuestThread* t : borrowers_) {
        if ((t->sigmask & bit) == 0) {
            t->post_signal(info);
            return;
        }
    }
    for (GuestThread* t : threads_) {
        if ((t->sigmask & bit) == 0) {
            t->post_signal(info);
            return;
        }
    }
    fallback.post_signal(info);
}

bool Process::first_time(std::uint64_t key) {
    std::lock_guard<std::mutex> lock(seen_mutex_);
    return seen_.insert(key).second;
}

// A guest probe answers one question the runtime report otherwise cannot: did a specific function
// run? Some of the most important ones are reached only through a dynamic dispatch (an animation
// event, a UnityEvent, an iTween callback by method name), so nothing in a log or in the report
// shows whether they executed. Each probe replaces the function's prologue with a private svc, and
// the stop handler emulates the replaced instruction before the function continues.
void Process::load_probe_requests() {
    if (probes_loaded_) return;
    probes_loaded_ = true;
    std::string text;
    const char* const inline_list = std::getenv("ZB_GUEST_PROBES");
    if (inline_list != nullptr && *inline_list != '\0') {
        text = inline_list;
    } else {
        // Beside the runtime report, so a device run can change the list without a rebuild.
        const std::string& report = runtime_report_path();
        const std::size_t slash = report.find_last_of('/');
        if (slash == std::string::npos) return;
        const std::string path = report.substr(0, slash + 1) + "zb-probes.txt";
        std::FILE* const file = std::fopen(path.c_str(), "rb");
        if (file == nullptr) return;
        char line[256];
        while (std::fgets(line, sizeof line, file) != nullptr) {
            text += line;
            text += ';';
        }
        std::fclose(file);
    }
    // Entries are separated by ';' or newlines and look like "libil2cpp.so+0x94d858 StartScene.Update".
    std::size_t begin = 0;
    while (begin < text.size() && probes_.size() < 32) {
        std::size_t end = text.find_first_of(";\r\n", begin);
        if (end == std::string::npos) end = text.size();
        const std::string entry = text.substr(begin, end - begin);
        begin = end + 1;
        std::size_t at = entry.find_first_not_of(" \t");
        if (at == std::string::npos || entry[at] == '#') continue;
        const std::size_t space = entry.find_first_of(" \t", at);
        const std::string spec = entry.substr(at, space == std::string::npos ? std::string::npos : space - at);
        std::string name;
        bool read_method_name = false;
        if (space != std::string::npos) {
            const std::size_t name_at = entry.find_first_not_of(" \t", space);
            if (name_at != std::string::npos) {
                const std::size_t name_end = entry.find_first_of(" \t", name_at);
                name = entry.substr(name_at, name_end == std::string::npos ? std::string::npos : name_end - name_at);
                // An optional trailing flag. "method" reports the managed method name of an IL2CPP
                // MethodInfo passed in r0, which is how the generic invoke worker is identified.
                if (name_end != std::string::npos) {
                    const std::size_t flag_at = entry.find_first_not_of(" \t", name_end);
                    if (flag_at != std::string::npos &&
                        entry.compare(flag_at, std::string::npos, "method") == 0) {
                        read_method_name = true;
                    }
                }
            }
        }
        const std::size_t plus = spec.rfind('+');
        if (plus == std::string::npos) continue;
        const char* const hex = spec.c_str() + plus + 1;
        char* hex_end = nullptr;
        const unsigned long offset = std::strtoul(hex, &hex_end, 16);
        if (hex_end == hex) continue;
        GuestProbe probe;
        probe.module = spec.substr(0, plus);
        probe.module_offset = static_cast<std::uint32_t>(offset);
        probe.name = name.empty() ? spec : name;
        probe.read_method_name = read_method_name;
        probes_.push_back(std::move(probe));
    }
}

void Process::install_probes() {
    if (probes_resolved_.load(std::memory_order_acquire) >= probes_.size()) return;
    if (probes_installing_.exchange(true)) return;
    std::size_t resolved = 0;
    for (std::size_t index = 0; index < probes_.size(); ++index) {
        GuestProbe& probe = probes_[index];
        {
            std::lock_guard<std::mutex> plock(probe_mutex_);
            if (probe.installed) {
                ++resolved;
                continue;
            }
        }
        std::uint32_t address = 0;
        std::uint32_t map_start = 0;
        std::uint32_t map_offset = 0;
        std::uint32_t map_length = 0;
        std::uint32_t original = 0;
        bool patched_ok = false;
        std::string skip_reason;
        {
            std::lock_guard<std::mutex> lock(mm_mutex_);
            for (const FileMapping& mapping : file_mappings_) {
                const std::size_t slash = mapping.path.find_last_of('/');
                const std::string base =
                    slash == std::string::npos ? mapping.path : mapping.path.substr(slash + 1);
                if (base != probe.module) continue;
                // An image placed by our own loader is named by ELF virtual address, one mapped by
                // the guest's linker by file offset; describe_code_address() uses the same pair.
                if (probe.module_offset < mapping.offset) continue;
                if (static_cast<std::uint64_t>(probe.module_offset) - mapping.offset >= mapping.length) continue;
                address = mapping.start + (probe.module_offset - static_cast<std::uint32_t>(mapping.offset));
                map_start = mapping.start;
                map_offset = static_cast<std::uint32_t>(mapping.offset);
                map_length = static_cast<std::uint32_t>(mapping.length);
                break;
            }
            if (address != 0) {
                const std::uint32_t page = address & ~kPageMask;
                const std::uint8_t flags = mem_.page_flags(page);
                const int guest_prot = ((flags & kPageRead) ? PROT_READ : 0) |
                                       ((flags & kPageWrite) ? PROT_WRITE : 0) |
                                       ((flags & kPageExec) ? PROT_EXEC : 0);
                const std::uint8_t* const source = mem_.host_ptr(address, sizeof(std::uint32_t), kPageRead);
                if ((guest_prot & PROT_EXEC) == 0 || source == nullptr) {
                    skip_reason = "not executable code";
                } else {
                    std::memcpy(&original, source, sizeof original);
                    if ((original & kArmPushMask) != kArmPush) {
                        char text[96];
                        std::snprintf(text, sizeof text, "not a push prologue: 0x%08x", original);
                        skip_reason = text;
                    } else {
                        const std::uint32_t patched = 0xEF000000u | (kProbeSwiBase | static_cast<std::uint32_t>(index));
                        // Keep PROT_EXEC set while patching so concurrent guest threads never hit NoExecuteFault.
                        if (!mem_.protect(page, kPageSize, guest_prot | PROT_WRITE)) {
                            skip_reason = "page not writable";
                        } else {
                            std::uint8_t* const target = mem_.host_ptr(address, sizeof patched, kPageRead | kPageWrite);
                            if (target != nullptr) std::memcpy(target, &patched, sizeof patched);
                            mem_.protect(page, kPageSize, guest_prot);
                            patched_ok = target != nullptr;
                        }
                    }
                }
            }
        }
        if (address == 0) continue;  // the image is not mapped yet: try again on a later stop
        if (std::find(probe_map_reported_.begin(), probe_map_reported_.end(), probe.module) ==
            probe_map_reported_.end()) {
            probe_map_reported_.push_back(probe.module);
            char map[160];
            std::snprintf(map, sizeof map, "start=0x%08x len=0x%x file-off=0x%x request=0x%x -> 0x%08x",
                          map_start, map_length, map_offset, probe.module_offset, address);
            runtime_report().note_jni_detail("probe-map-" + probe.module, map, false);
        }
        {
            std::lock_guard<std::mutex> plock(probe_mutex_);
            probe.installed = true;
            probe.address = address;
            if (patched_ok) probe.original = original;
        }
        ++resolved;
        if (!skip_reason.empty()) {
            runtime_report().note_jni_detail("guest-probe-skip", probe.name + ": " + skip_reason, false);
            continue;
        }
        if (patched_ok) invalidate(address, sizeof(std::uint32_t));
    }
    probes_resolved_.store(resolved, std::memory_order_release);
    probes_installing_.store(false);
    if (resolved != probes_reported_) {
        probes_reported_ = resolved;
        runtime_report().note_jni_detail(
            "guest-probes",
            "requested=" + std::to_string(probes_.size()) + " installed=" + std::to_string(resolved), true);
    }
}

// Reads a NUL-terminated guest string of at most `limit` printable ASCII bytes. Diagnostics
// only: an unreadable page or a non-printable byte yields an empty string, never a fault.
static std::string probe_guest_string(GuestMemory& mem, std::uint32_t address, std::uint32_t limit) {
    if (address == 0) return {};
    std::string text;
    for (std::uint32_t offset = 0; offset < limit; ++offset) {
        if (static_cast<std::uint64_t>(address) + offset >= kGuestSpaceSize) break;
        const std::uint8_t* const p = mem.host_ptr(address + offset, 1, kPageRead);
        if (p == nullptr || *p == 0) break;
        if (*p < 0x20 || *p > 0x7e) return {};
        text.push_back(static_cast<char>(*p));
    }
    return text;
}

static std::uint32_t probe_guest_word(GuestMemory& mem, std::uint32_t address) {
    const std::uint8_t* const p = mem.host_ptr(address, 4, kPageRead);
    if (p == nullptr) return 0;
    std::uint32_t value = 0;
    std::memcpy(&value, p, sizeof value);
    return value;
}

// Reads the managed method name an IL2CPP MethodInfo describes (methodPointer, invoker_method,
// name, klass in il2cpp 24.x, Unity 2019), qualified by its declaring class so two behaviours
// whose message is `Update` can be told apart. Diagnostics only, and empty when the words do not
// lead to short printable strings.
static std::string probe_method_name(GuestMemory& mem, std::uint32_t method_info) {
    if (method_info == 0) return {};
    const std::string method = probe_guest_string(mem, probe_guest_word(mem, method_info + 8), 48);
    if (method.size() < 2) return {};
    const std::uint32_t klass = probe_guest_word(mem, method_info + 12);
    const std::string type = klass != 0 ? probe_guest_string(mem, probe_guest_word(mem, klass + 8), 32) : std::string();
    return type.empty() ? method : type + "." + method;
}

bool Process::handle_probe(GuestThread& thread, const Stop& stop) {
    const std::uint32_t index = stop.swi & 0xFFFFu;
    std::lock_guard<std::mutex> plock(probe_mutex_);
    if (index >= probes_.size()) return false;
    GuestProbe& probe = probes_[index];
    if (!probe.installed || probe.original == 0) return false;
    ++probe.hits;
    // Self-verification: a hit proves the patch was in place at this call, and re-reading the word
    // now says whether it still is. libil2cpp.so carries text relocations, so a rewrite of its text
    // could restore the prologue and silently stop every later hit.
    if (!probe.reverted_reported) {
        const std::uint32_t expected = 0xEF000000u | (kProbeSwiBase | static_cast<std::uint32_t>(index));
        std::uint32_t current = 0;
        const std::uint8_t* const word = mem_.host_ptr(probe.address, sizeof current, kPageRead);
        if (word != nullptr) std::memcpy(&current, word, sizeof current);
        if (word == nullptr || current != expected) {
            probe.reverted_reported = true;
            char text[160];
            std::snprintf(text, sizeof text, "expected 0x%08x found 0x%08x hits=%u", expected, current,
                          probe.hits);
            runtime_report().note_jni_detail("guest-probe-reverted", probe.name + ": " + text, true);
        }
    }
    // The replaced instruction is `push {register list}`: sp -= 4 * count, then the named registers
    // are stored at ascending addresses, lowest register first.
    const std::uint32_t list = probe.original & 0xFFFFu;
    std::uint32_t count = 0;
    for (unsigned reg = 0; reg < 16; ++reg) {
        if ((list & (1u << reg)) != 0) ++count;
    }
    const std::uint32_t sp = thread.regs()[13] - 4u * count;
    std::uint32_t cursor = sp;
    for (unsigned reg = 0; reg < 16; ++reg) {
        if ((list & (1u << reg)) == 0) continue;
        thread.MemoryWrite32(cursor, thread.regs()[reg]);
        cursor += 4;
    }
    thread.regs()[13] = sp;
    if (!probe.hit) {
        probe.hit = true;
        char text[200];
        std::snprintf(text, sizeof text,
                      "first-hit tid=%d at=0x%08x lr=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x", thread.tid,
                      probe.address, thread.regs()[14], thread.regs()[0], thread.regs()[1], thread.regs()[2],
                      thread.regs()[3]);
        std::string line = text;
        if (probe.read_method_name) {
            const std::string method = probe_method_name(mem_, thread.regs()[0]);
            if (!method.empty()) line += " method=" + method;
        }
        runtime_report().note_jni_detail(probe.name, line, false);
    } else if (probe.read_method_name && probe.hits <= 65536) {
        // A bounded tally instead of one line per hit: note_jni_detail keys on the name, so per-hit
        // lines would overwrite each other and only the second call's callee would survive.
        const std::string method = probe_method_name(mem_, thread.regs()[0]);
        if (!method.empty()) {
            auto found = std::find_if(probe.methods.begin(), probe.methods.end(),
                                      [&](const auto& entry) { return entry.first == method; });
            if (found != probe.methods.end()) {
                ++found->second;
            } else if (probe.methods.size() < 24) {
                probe.methods.emplace_back(method, 1u);
            }
            probe.recent[probe.recent_next] = method;
            probe.recent_next = (probe.recent_next + 1) % GuestProbe::kRecentNames;
            const std::size_t dot = method.rfind('.');
            const std::string bare = dot == std::string::npos ? method : method.substr(dot + 1);
            for (std::size_t i = 0; i < probe.lifecycle.size(); ++i) {
                if (bare == GuestProbe::kLifecycleNames[i]) ++probe.lifecycle[i];
            }
            // The entry point the script runtime will call, so a code probe that reports no hit can
            // be compared with the address actually used. Only per-frame messages and Rebuild are
            // kept: the startup Awake/OnEnable burst would fill the list before any frame runs.
            const bool interesting = bare == "Update" || bare == "Rebuild" || bare == "Start";
            if (interesting && probe.code_entries.size() < 8 &&
                std::none_of(probe.code_entries.begin(), probe.code_entries.end(),
                             [&](const auto& entry) { return entry.first == method; })) {
                probe.code_entries.emplace_back(
                    method, std::make_pair(probe_guest_word(mem_, thread.regs()[0]),
                                           probe_guest_word(mem_, thread.regs()[0] + 32)));
            }
        }
        // Powers of two only, so a hot function cannot grow the report. Three lines: the names in
        // first-seen order (the startup sequence), the most frequent ones, and the steady-state
        // window of recent calls with the lifecycle message counts.
        if ((probe.hits & (probe.hits - 1)) == 0) {
            std::string seen = "hits=" + std::to_string(probe.hits);
            for (std::size_t i = 0; i < probe.methods.size() && i < 6; ++i) {
                seen += " " + probe.methods[i].first + "=" + std::to_string(probe.methods[i].second);
            }
            if (probe.methods.empty()) seen += " method=(unknown)";
            if (probe.methods.size() > 6) seen += " more=yes";
            runtime_report().note_jni_detail(probe.name + ".methods", seen, true);

            std::vector<std::pair<std::string, std::uint32_t>> hot = probe.methods;
            std::stable_sort(hot.begin(), hot.end(),
                             [](const auto& a, const auto& b) { return a.second > b.second; });
            std::string frequent = "hits=" + std::to_string(probe.hits);
            for (std::size_t i = 0; i < hot.size() && i < 6; ++i) {
                frequent += " " + hot[i].first + "=" + std::to_string(hot[i].second);
            }
            if (hot.empty()) frequent += " method=(unknown)";
            if (hot.size() > 6) frequent += " more=yes";
            runtime_report().note_jni_detail(probe.name + ".hot", frequent, true);

            if (!probe.code_entries.empty()) {
                std::string code = "hits=" + std::to_string(probe.hits);
                for (const auto& [name, entry] : probe.code_entries) {
                    char hex[40];
                    std::snprintf(hex, sizeof hex, " =0x%08x:0x%08x", entry.first, entry.second);
                    code += " " + name + hex;
                }
                runtime_report().note_jni_detail(probe.name + ".code", code, true);
            }

            std::vector<std::pair<std::string, std::uint32_t>> window;
            for (const std::string& name : probe.recent) {
                if (name.empty()) continue;
                auto found = std::find_if(window.begin(), window.end(),
                                          [&](const auto& entry) { return entry.first == name; });
                if (found != window.end()) {
                    ++found->second;
                } else if (window.size() < 6) {
                    window.emplace_back(name, 1u);
                }
            }
            std::stable_sort(window.begin(), window.end(),
                             [](const auto& a, const auto& b) { return a.second > b.second; });
            std::string recent = "window=" + std::to_string(GuestProbe::kRecentNames);
            for (const auto& [name, count] : window) recent += " " + name + "=" + std::to_string(count);
            if (window.empty()) recent += " (empty)";
            for (std::size_t i = 0; i < probe.lifecycle.size(); ++i) {
                recent += std::string(" | ") + GuestProbe::kLifecycleNames[i] + "=" +
                          std::to_string(probe.lifecycle[i]);
            }
            runtime_report().note_jni_detail(probe.name + ".recent", recent, true);
        }
    } else if ((probe.hits & (probe.hits - 1)) == 0 && probe.hits <= 65536) {
        // Bounded proof of life: powers of two only, so a hot function cannot grow the report.
        runtime_report().note_jni_detail(probe.name + ".hits", "hits=" + std::to_string(probe.hits), false);
    }
    return true;
}

// Re-reads every installed probe's patched word and reports a probe whose svc is gone. Called from
// dispatch_stop at a bounded cadence, so it also covers a function that never reaches its entry.
void Process::verify_probes() {
    std::lock_guard<std::mutex> plock(probe_mutex_);
    for (std::size_t index = 0; index < probes_.size(); ++index) {
        GuestProbe& probe = probes_[index];
        if (!probe.installed || probe.original == 0 || probe.reverted_reported) continue;
        const std::uint32_t expected = 0xEF000000u | (kProbeSwiBase | static_cast<std::uint32_t>(index));
        std::uint32_t current = 0;
        const std::uint8_t* const word = mem_.host_ptr(probe.address, sizeof current, kPageRead);
        if (word != nullptr) std::memcpy(&current, word, sizeof current);
        if (word != nullptr && current == expected) continue;
        probe.reverted_reported = true;
        char text[160];
        std::snprintf(text, sizeof text, "expected 0x%08x found 0x%08x hits=%u", expected, current, probe.hits);
        runtime_report().note_jni_detail("guest-probe-reverted", probe.name + ": " + text, true);
    }
}

void Process::record_file_mapping(std::uint32_t start, std::uint32_t length, std::uint64_t offset, std::string path,
                                  bool offset_is_vaddr) {
    forget_mappings(start, length);
    file_mappings_.push_back({start, length, offset, std::move(path), offset_is_vaddr});
}

std::vector<std::string> Process::mapped_file_paths() const {
    std::lock_guard<std::mutex> lock(mm_mutex_);
    std::vector<std::string> paths;
    for (auto it = file_mappings_.rbegin(); it != file_mappings_.rend(); ++it) {
        if (it->path.empty()) continue;
        if (std::find(paths.begin(), paths.end(), it->path) == paths.end()) paths.push_back(it->path);
    }
    return paths;
}

void Process::forget_mappings(std::uint32_t start, std::uint64_t length) {
    if (length == 0) return;
    const std::uint64_t end = std::min<std::uint64_t>(static_cast<std::uint64_t>(start) + length, kGuestSpaceSize);
    std::vector<FileMapping> remaining;
    remaining.reserve(file_mappings_.size());
    for (auto& m : file_mappings_) {
        const std::uint64_t m_start = m.start;
        const std::uint64_t m_end = m_start + m.length;
        if (m_end <= start || m_start >= end) {
            remaining.push_back(std::move(m));
            continue;
        }
        if (m_start < start) {
            remaining.push_back(FileMapping{
                m.start, static_cast<std::uint32_t>(start - m_start), m.offset, m.path, m.offset_is_vaddr});
        }
        if (m_end > end) {
            const std::uint64_t delta = end - m_start;
            remaining.push_back(FileMapping{
                static_cast<std::uint32_t>(end), static_cast<std::uint32_t>(m_end - end), m.offset + delta,
                std::move(m.path), m.offset_is_vaddr});
        }
    }
    file_mappings_ = std::move(remaining);

    std::vector<std::pair<std::uint32_t, std::uint32_t>> kept_textrel;
    kept_textrel.reserve(textrel_ranges_.size());
    for (const auto& r : textrel_ranges_) {
        const std::uint64_t r_start = r.first;
        const std::uint64_t r_end = r_start + r.second;
        if (r_end <= start || r_start >= end) {
            kept_textrel.push_back(r);
            continue;
        }
        if (r_start < start) {
            kept_textrel.emplace_back(r.first, static_cast<std::uint32_t>(start - r_start));
        }
        if (r_end > end) {
            kept_textrel.emplace_back(static_cast<std::uint32_t>(end), static_cast<std::uint32_t>(r_end - end));
        }
    }
    textrel_ranges_ = std::move(kept_textrel);
}

std::string Process::describe_address_locked(std::uint32_t addr) const {
    for (const auto& m : file_mappings_) {
        if (addr >= m.start && addr - m.start < m.length) {
            char where[64];
            std::snprintf(where, sizeof where, " %s 0x%llx", m.offset_is_vaddr ? "vaddr" : "offset",
                          static_cast<unsigned long long>(m.offset + (addr - m.start)));
            return m.path + where;
        }
    }
    return "?";
}

std::string Process::describe_address(std::uint32_t addr) const {
    std::lock_guard<std::mutex> lock(mm_mutex_);
    return describe_address_locked(addr);
}

std::string Process::describe_code_address_locked(std::uint32_t addr) const {
    for (const auto& m : file_mappings_) {
        if (addr >= m.start && addr - m.start < m.length) {
            const char* slash = std::strrchr(m.path.c_str(), '/');
            char text[96];
            std::snprintf(text, sizeof text, "%s%c0x%llx", slash != nullptr ? slash + 1 : m.path.c_str(),
                          m.offset_is_vaddr ? '+' : '@',
                          static_cast<unsigned long long>(m.offset + (addr - m.start)));
            return text;
        }
    }
    return std::string();
}

std::string Process::describe_code_address(std::uint32_t addr) const {
    std::lock_guard<std::mutex> lock(mm_mutex_);
    return describe_code_address_locked(addr);
}

bool Process::sample_thread_state(std::int32_t tid, ThreadStateSample& out) {
    out = ThreadStateSample{};
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        GuestThread* thread = nullptr;
        for (GuestThread* t : borrowers_) {
            if (t->host_tid.load(std::memory_order_relaxed) == tid) {
                thread = t;
                break;
            }
        }
        if (thread == nullptr) {
            for (GuestThread* t : borrowers_) {
                if (t->host_tid.load(std::memory_order_relaxed) == 0 && t->tid == tid) {
                    thread = t;
                    break;
                }
            }
        }
        if (thread == nullptr) {
            for (GuestThread* t : threads_) {
                if (t->tid == tid) {
                    thread = t;
                    break;
                }
            }
        }
        if (thread == nullptr) return false;
        const std::array<std::uint32_t, 16>& regs = thread->regs();
        out.pc = regs[15];
        out.lr = regs[14];
        out.sp = regs[13];
        out.fp = regs[11];
        for (std::size_t i = 0; i < 4; ++i) out.arg[i] = regs[i];
    }
    if (out.sp == 0) return true;
    std::lock_guard<std::mutex> lock(mm_mutex_);
    // r0 of a thread blocked in futex is the futex word itself; reading it is what tells a
    // contended lock (2) from a plain wait (0).
    if ((out.arg[0] & 3u) == 0) {
        if (const std::uint8_t* word = mem_.host_ptr(out.arg[0], 4, kPageRead)) {
            std::memcpy(&out.arg_word, word, sizeof out.arg_word);
            out.arg_word_valid = true;
        }
    }
    // A Thumb return address is odd and points into a loaded image; data pointers on the stack are
    // word aligned. Scanning upward from sp therefore recovers the call chain of a blocked thread
    // without frame pointers, which a stripped release build does not promise to keep.
    for (std::uint32_t offset = 0; offset < 8192 && out.chain.size() < kMaxChain; offset += 4) {
        const std::uint32_t addr = out.sp + offset;
        if (addr < out.sp) break;  // wrapped the 32-bit space
        const std::uint8_t* word = mem_.host_ptr(addr, 4, kPageRead);
        if (word == nullptr) continue;
        std::uint32_t value = 0;
        std::memcpy(&value, word, sizeof value);
        if ((value & 1u) == 0) continue;
        std::string label = describe_code_address_locked(value & ~1u);
        if (label.empty()) continue;
        out.chain.push_back(std::move(label));
    }
    return true;
}

void Process::add_textrel_range(std::uint32_t start, std::uint32_t length) {
    textrel_ranges_.emplace_back(start, length);
}

bool Process::overlaps_textrel_range(std::uint32_t start, std::uint64_t length) const {
    const std::uint64_t end = static_cast<std::uint64_t>(start) + length;
    for (const auto& [range_start, range_length] : textrel_ranges_) {
        if (range_start < end && static_cast<std::uint64_t>(range_start) + range_length > start) return true;
    }
    return false;
}

void Process::add_path_alias(std::string guest_prefix, std::string host_prefix) {
    if (guest_prefix.empty() || host_prefix.empty()) return;
    std::lock_guard<std::mutex> lock(path_aliases_mutex_);
    for (auto& [from, to] : path_aliases_) {
        if (from == guest_prefix) {
            to = std::move(host_prefix);
            return;
        }
    }
    path_aliases_.emplace_back(std::move(guest_prefix), std::move(host_prefix));
}

void Process::add_library_dir(std::string dir) {
    if (dir.empty()) return;
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    std::lock_guard<std::mutex> lock(path_aliases_mutex_);
    for (const std::string& existing : library_dirs_) {
        if (existing == dir) return;
    }
    library_dirs_.push_back(std::move(dir));
}

// True for a guest path that names a shared object in a system library directory
// (/system/lib/libfoo.so, /vendor/lib64/libfoo.so, /system/lib/arm/libfoo.so). Other kinds of
// file - fonts, configuration, application data - are not looked up in add_library_dir().
static bool is_system_library_path(std::string_view path) {
    if (path.size() < 4 || path.back() == '/') return false;
    if (path.find(".so") == std::string_view::npos) return false;
    const std::string_view dir = path.substr(0, path.find_last_of('/'));
    const std::size_t slash = dir.find_last_of('/');
    const std::string_view component = slash == std::string_view::npos ? dir : dir.substr(slash + 1);
    return component == "lib" || component == "lib64";
}

std::string Process::translate_path(const char* guest_path) const {
    const std::string_view path(guest_path);
    if (path == "/proc/self/exe") return exe_path_;
    {
        std::lock_guard<std::mutex> lock(path_aliases_mutex_);
        const std::string* best_from = nullptr;
        const std::string* best_to = nullptr;
        for (const auto& [from, to] : path_aliases_) {
            if (path.substr(0, from.size()) != from) continue;
            if (best_from == nullptr || from.size() > best_from->size()) {
                best_from = &from;
                best_to = &to;
            }
        }
        if (best_from != nullptr) return *best_to + std::string(path.substr(best_from->size()));
    }
    if (!sysroot_.empty()) {
        for (const auto& m : kPathMappings) {
            if (path.substr(0, m.guest_prefix.size()) == m.guest_prefix) {
                std::string out = sysroot_;
                out += m.sysroot_prefix;
                out += path.substr(m.guest_prefix.size());
                // The sysroot holds the 32-bit libraries and nothing else, but guests also read
                // plain data from the system: fonts (/system/fonts, /system/etc/fonts.xml),
                // timezone tables, configuration. Those are architecture-independent, so a path
                // the sysroot does not have falls through to the device's own file. Flutter drew
                // its icons (a font inside the APK) and no text at all until this fallback
                // existed, because every /system/fonts lookup landed in the sysroot and failed.
                if (::access(out.c_str(), F_OK) == 0) return out;
                // The libraries the bridge implements itself are not in the sysroot either: they
                // live in the runtime's guest library directory. A guest that names one by a
                // system path - Topsoil's OpenAL probe dlopen()s "/system/lib/libOpenSLES.so"
                // directly - would otherwise get the device's file, which is built for the host
                // architecture and cannot be loaded by the guest's linker.
                if (is_system_library_path(path)) {
                    const std::string_view name = path.substr(path.find_last_of('/') + 1);
                    std::lock_guard<std::mutex> lock(path_aliases_mutex_);
                    for (const std::string& dir : library_dirs_) {
                        std::string candidate = dir;
                        candidate += '/';
                        candidate += name;
                        if (::access(candidate.c_str(), F_OK) == 0) return candidate;
                    }
                }
                static std::atomic<unsigned> fallbacks{0};
                const unsigned seen = fallbacks.fetch_add(1) + 1;
                if (seen <= 8) log("sysroot has no %.*s; using the device file",
                                   static_cast<int>(path.size()), path.data());
                return std::string(path);
            }
        }
    }
    return std::string(path);
}

std::size_t Process::thread_count() const {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    return threads_.size();
}

bool Process::is_borrower(const GuestThread& thread) const {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (const GuestThread* t : borrowers_) {
        if (t == &thread) return true;
    }
    return false;
}

std::string Process::pending_signal_summary() const {
    std::unique_lock lock(threads_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return "(thread registry busy)";

    std::string result;
    std::size_t count = 0;
    const auto append = [&](const GuestThread& thread) {
        const auto pending = thread.pending_signals();
        if (pending == 0 || ++count > 5) return;
        // host_tid is the borrowing Java thread while it runs guest code and zero in between,
        // so it names the host thread a signal has to reach, when there is one.
        const std::int32_t owner = thread.host_tid.load(std::memory_order_relaxed);
        const char* state = thread.blocking() ? "syscall" : owner != 0 ? "active" : "idle";
        char entry[96];
        std::snprintf(entry, sizeof entry, "guest=%d host=%d %s mask=0x%016llx", thread.tid,
                      owner != 0 ? owner : thread.tid, state,
                      static_cast<unsigned long long>(pending));
        if (!result.empty()) result += " | ";
        result += entry;
    };
    // Borrowers first: a Java thread keeps its guest identity between native calls.
    for (const GuestThread* thread : borrowers_) append(*thread);
    for (const GuestThread* thread : threads_) append(*thread);
    if (count > 5) result += " | more=" + std::to_string(count - 5);
    return result.empty() ? "(none)" : result;
}

int Process::allocate_processor_id() {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (std::size_t i = 0; i < processor_ids_.size(); ++i) {
        if (!processor_ids_.test(i)) {
            processor_ids_.set(i);
            return static_cast<int>(i);
        }
    }
    return -1;
}

GuestThread* Process::find_thread(std::int32_t tid) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (GuestThread* t : borrowers_) {
        if (t->tid == tid) return t;
    }
    for (GuestThread* t : threads_) {
        if (t->tid == tid) return t;
    }
    return nullptr;
}

bool Process::post_signal_to(std::int32_t tid, const g::siginfo32& info) {
    // post_signal takes no Process lock (atomics and a futex wake), so holding threads_mutex_
    // here cannot deadlock.
    std::lock_guard<std::mutex> lock(threads_mutex_);
    for (GuestThread* t : borrowers_) {
        if (t->tid == tid) {
            t->post_signal(info);
            return true;
        }
    }
    for (GuestThread* t : threads_) {
        if (t->tid == tid) {
            t->post_signal(info);
            return true;
        }
    }
    return false;
}

std::unique_ptr<GuestThread> Process::create_borrower(GuestThread& carrier) {
    const int processor_id = allocate_processor_id();
    if (processor_id < 0) return nullptr;
    auto borrower =
        std::make_unique<GuestThread>(mem_, monitor_.get(), static_cast<std::size_t>(processor_id), precise_faults_);
    configure_thread(*borrower);
    borrower->regs().fill(0);
    borrower->regs()[13] = carrier.regs()[13] & ~7u;
    borrower->ext_regs().fill(0);
    borrower->set_cpsr(kCpsrUserMode);
    borrower->set_fpscr(carrier.fpscr());
    borrower->set_tls(carrier.tls());
    borrower->tid = carrier.tid;
    borrower->sigmask = carrier.sigmask;
    borrower->altstack = carrier.altstack;
    record_guest_borrower();
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        borrowers_.push_back(borrower.get());
    }
    // Drain any signals that arrived on the carrier between parking and lease acquisition.
    g::siginfo32 pending_info;
    while (carrier.take_signal(0, pending_info)) borrower->post_signal(pending_info);
    return borrower;
}

// Borrower counterpart of unregister_thread(), in this order: tid routing is removed first, then
// the exclusive monitor slot is cleared and the JIT freed, and only then is the processor id
// released, so a concurrent create_borrower/clone_thread cannot reuse the id while this JIT still
// exists. A borrower is never published as the process signal target, and only the borrowing host
// thread names it as current (cleared by the caller before this), so no forwarding reader can
// still hold it.
namespace {

void release_gl_slot(GuestMemory& mem, GuestThread& thread) {
    const std::uint32_t batch_addr = thread.gl_batch_addr();
    if (batch_addr != 0) {
        if (auto* hdr = reinterpret_cast<zb_gl_batch_header*>(
                mem.host_ptr(batch_addr, sizeof(zb_gl_batch_header), kPageRead | kPageWrite))) {
            hdr->count = 0;
            hdr->used_words = 0;
            hdr->disabled = 0;
        }
        thread.set_gl_batch_addr(0);
    }
    const std::uint32_t slot_addr = thread.gl_slot_addr();
    if (slot_addr != 0) {
        if (auto* slot = reinterpret_cast<std::uint32_t*>(
                mem.host_ptr(slot_addr, 16, kPageRead | kPageWrite))) {
            slot[1] = 0;  // batch_registered
            slot[2] = 0;  // host_epoch
            slot[3] = 0;  // delete_epoch
            __atomic_store_n(&slot[0], 0u, __ATOMIC_RELEASE);  // tag
        }
        thread.set_gl_slot_addr(0);
    }
}

}  // namespace

void Process::destroy_borrower(std::unique_ptr<GuestThread> borrower, GuestThread& carrier) {
    if (!borrower) return;
    release_gl_slot(mem_, *borrower);
    carrier.sigmask = borrower->sigmask;
    carrier.altstack = borrower->altstack;
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        std::erase(borrowers_, borrower.get());
    }
    g::siginfo32 info;
    while (borrower->take_signal(0, info)) carrier.post_signal(info);
    const std::size_t processor_id = borrower->processor_id();
    monitor_->ClearProcessor(processor_id);
    borrower.reset();
    std::lock_guard<std::mutex> lock(threads_mutex_);
    processor_ids_.reset(processor_id);
}

void Process::register_thread(GuestThread* thread) {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    threads_.push_back(thread);
}

int Process::run(const std::string& path, const std::vector<std::string>& argv, const std::vector<std::string>& envp) {
    if (!mem_.ok()) return 1;

    char resolved[PATH_MAX];
    exe_path_ = realpath(path.c_str(), resolved) != nullptr ? resolved : path;

    LoadedElf exe;
    std::string error;
    if (!load_elf(mem_, path, kExecutableLimit, exe, error)) {
        log("%s", error.c_str());
        return 1;
    }
    record_file_mapping(exe.load_start, exe.load_end - exe.load_start, exe.load_start - exe.bias, exe_path_, true);
    load_probe_requests();

    std::uint32_t start_pc = exe.entry;
    std::uint32_t interp_base = 0;
    if (!exe.interp.empty()) {
        const std::string interp_path = translate_path(exe.interp.c_str());
        LoadedElf interp;
        if (!load_elf(mem_, interp_path, kMmapLimit, interp, error)) {
            log("cannot load the dynamic linker %s: %s", exe.interp.c_str(), error.c_str());
            if (sysroot_.empty()) log("pass --sysroot or set ZB_SYSROOT");
            return 1;
        }
        record_file_mapping(interp.load_start, interp.load_end - interp.load_start, interp.load_start - interp.bias,
                            interp_path, true);
        interp_base = interp.bias;
        start_pc = interp.entry;
        linker_start_ = interp.load_start;
        linker_end_ = interp.load_end;
    }

    if (!mem_.map_anon(kStackTop - kStackSize, kStackSize, PROT_READ | PROT_WRITE)) {
        log("cannot map the guest stack");
        return 1;
    }
    if (!map_kuser_page(mem_)) {
        log("cannot map the kuser helper page");
        return 1;
    }
    brk_start = brk_current = exe.load_end;

    const std::vector<AuxEntry> auxv = {
        {AT_PHDR, exe.phdr},
        {AT_PHENT, sizeof(Elf32_Phdr)},
        {AT_PHNUM, exe.phnum},
        {AT_PAGESZ, kPageSize},
        {AT_BASE, interp_base},
        {AT_FLAGS, 0},
        {AT_ENTRY, exe.entry},
        {AT_UID, static_cast<std::uint32_t>(getuid())},
        {AT_EUID, static_cast<std::uint32_t>(geteuid())},
        {AT_GID, static_cast<std::uint32_t>(getgid())},
        {AT_EGID, static_cast<std::uint32_t>(getegid())},
        {AT_HWCAP, kGuestHwcap},
        {AT_HWCAP2, 0},
        {AT_CLKTCK, 100},
        {AT_SECURE, 0},
    };
    const std::uint32_t sp = build_initial_stack(mem_, kStackTop, argv, envp, auxv, exe_path_);
    if (sp == 0) {
        log("arguments and environment do not fit on the guest stack");
        return 1;
    }
    initial_sp_ = sp;

    main_ = std::make_unique<GuestThread>(mem_, monitor_.get(), static_cast<std::size_t>(allocate_processor_id()),
                                          precise_faults_);
    configure_thread(*main_);
    auto& regs = main_->regs();
    regs.fill(0);
    regs[13] = sp;
    regs[15] = start_pc & ~1u;
    main_->set_cpsr(kCpsrUserMode | ((start_pc & 1) ? kCpsrThumb : 0));
    main_->tid = static_cast<std::int32_t>(::syscall(SYS_gettid));
    main_->host_tid.store(main_->tid, std::memory_order_relaxed);
    register_thread(main_.get());
    set_process_signal_target(main_.get());
    install_host_signal_forwarding();
    install_terminate_reporter();

    thread_loop(*main_);
    // Every return below retires what run() published, so destroying the Process afterwards cannot
    // leave a signal handler with a freed main_. The target is retired while t_current_thread still
    // names main_, then this host thread stops naming it.
    if (exiting_) {
        // exit_group/fatal exit: other live threads would already have ended the host process, so
        // main_ is the only registered thread. Its CLONE_CHILD_CLEARTID is not honoured on a
        // process-wide exit.
        set_current_thread(nullptr);
        unregister_thread(main_);
        return exit_status_;
    }

    // The main thread called exit() while other threads may still run: wait for them.
    const int main_status = main_->exit_status;
    set_current_thread(nullptr);
    finish_thread(main_);
    wait_for_threads();
    return exiting_ ? exit_status_.load() : main_status;
}

void Process::configure_thread(GuestThread& thread) {
    thread.set_fast_svc_handler(&Process::fast_svc_entry, this);
}

bool Process::fast_svc_entry(void* ctx, GuestThread& thread, std::uint32_t swi) {
    return static_cast<Process*>(ctx)->try_fast_svc(thread, swi);
}

bool Process::try_fast_svc(GuestThread& thread, std::uint32_t swi) {
    if (__builtin_expect(exiting_.load(std::memory_order_relaxed), 0)) return false;
    if (__builtin_expect(!probes_.empty(), 0)) {
        if (probes_resolved_.load(std::memory_order_acquire) < probes_.size()) return false;
        if (((probe_verify_ticks_.load(std::memory_order_relaxed) + 1u) & 0x7FFFu) == 0) {
            return false;
        }
    }
    if (swi == 0) {
        // Pure-integer fast syscalls do not invoke external driver code or modify host FPCR.
        return try_fast_syscall(*this, thread);
    }
    if ((swi & 0xFF0000u) == kHostCallBase && swi != kHostReturnSwi && fast_host_call_handler_ != nullptr) {
        HostFpcrGuard fpcr_guard;
        return fast_host_call_handler_(fast_host_call_ctx_, swi & 0xFFFFu, thread);
    }
    return false;
}

bool Process::dispatch_stop(GuestThread& thread, const Stop& stop) {
    if (probes_resolved_.load(std::memory_order_acquire) < probes_.size()) install_probes();
    // Periodic self-check of every installed probe. A rewrite of the image text (this APK carries
    // text relocations) would restore the prologue silently, and then the absence of a hit says
    // nothing about whether the function ran.
    if (!probes_.empty() && ((probe_verify_ticks_.fetch_add(1, std::memory_order_relaxed) + 1u) & 0x7FFFu) == 0) {
        verify_probes();
    }
    switch (stop.kind) {
    case StopKind::Svc:
        if (stop.swi == 0) {
            if (handle_syscall(*this, thread)) return true;
            if (!exiting_ && thread.call_depth > 0) {
                // bionic has already released this thread's TLS and stack; nothing may run on it.
                log("guest thread exited inside a host-to-guest call");
                request_exit(1);
                exit_host_process();
            }
            if (exiting_ && thread_count() > 1) exit_host_process();
            return false;
        }
        if (stop.swi == kHostReturnSwi) {
            // GuestThread::call consumes its own return; any other one is an illegal instruction.
            if (thread.call_depth == 0) log("host return svc outside a host-to-guest call at pc 0x%08x", stop.pc - 4);
            Stop illegal;
            illegal.kind = StopKind::Exception;
            illegal.exception = Dynarmic::A32::Exception::UndefinedInstruction;
            illegal.pc = stop.pc - 4;
            return fault_or_crash(thread, illegal);
        }
        if ((stop.swi & 0xFF0000u) == kProbeSwiBase) {
            if (handle_probe(thread, stop)) return true;
        }
        if ((stop.swi & 0xFF0000u) == kHostCallBase) {
            const std::uint32_t index = stop.swi & 0xFFFFu;
            const std::int32_t diag_tid = thread.diagnostic_tid();
            record_thread_activity(diag_tid, ThreadActivityKind::kHostCall, index);
            // A host call is host C++ called from translated guest code, and an exception that
            // escapes it has no frame above to catch it: the process dies with "terminating" and
            // nothing else. The name of the call and the reason are worth far more than the
            // abort, so they go into the report and the guest is stopped instead.
            bool served = false;
#if !defined(ZB_NO_TELEMETRY)
            // Wall time of the host side of this call. A stalled guest loop is either a host call
            // that never returned or the guest's own spin; only the first shows up here.
#if defined(__aarch64__)
            const std::uint64_t freq_hz = host_call_counter_freq_hz();
            const std::uint64_t host_call_started =
                freq_hz != 0 ? host_call_now_ticks() : coarse_now_ms();
#else
            const std::uint64_t host_call_started_ms = coarse_now_ms();
#endif
#endif  // !ZB_NO_TELEMETRY
            try {
                served = host_call_handler_ && host_call_handler_(index, thread);
            } catch (const std::exception& failure) {
                const auto [failed_library, failed_name] = host_call_name(index);
                std::string detail = std::string(failed_library) + ":" + failed_name + ": " + failure.what();
                log("host call %s threw: %s", failed_name, failure.what());
                runtime_report().note_jni_detail("host-call-threw", detail, true);
                record_thread_activity_done(diag_tid);
                request_exit(70);
                if (thread_count() > 1) exit_host_process();
                return false;
            } catch (...) {
                const auto [failed_library, failed_name] = host_call_name(index);
                log("host call %s threw a non-standard exception", failed_name);
                runtime_report().note_jni_detail("host-call-threw",
                                                 std::string(failed_library) + ":" + failed_name, true);
                record_thread_activity_done(diag_tid);
                request_exit(70);
                if (thread_count() > 1) exit_host_process();
                return false;
            }
            if (served) {
#if !defined(ZB_NO_TELEMETRY)
                std::uint64_t elapsed_ms = 0;
#if defined(__aarch64__)
                if (freq_hz != 0) {
                    const std::uint64_t ended_ticks = host_call_now_ticks();
                    const std::uint64_t delta_ticks =
                        ended_ticks > host_call_started ? ended_ticks - host_call_started : 0;
                    if (__builtin_expect(delta_ticks >= freq_hz, 0)) {
                        elapsed_ms = (delta_ticks * 1000ULL) / freq_hz;
                    }
                } else {
                    const std::uint64_t ended_ms = coarse_now_ms();
                    elapsed_ms = ended_ms > host_call_started ? ended_ms - host_call_started : 0;
                }
#else
                const std::uint64_t ended_ms = coarse_now_ms();
                elapsed_ms =
                    ended_ms > host_call_started_ms ? ended_ms - host_call_started_ms : 0;
#endif
                if (__builtin_expect(elapsed_ms >= 1000, 0) && !host_call_is_lease(index) &&
                    index != ZB_SERVICE_READY_INDEX) {
                    const auto [slow_library, slow_name] = host_call_name(index);
                    // The tid alone identifies nothing: a thread created after the one-time census
                    // was written never appears in it, and a number cannot be read as a thread.
                    char raw_index[16];
                    std::snprintf(raw_index, sizeof raw_index, "0x%x", index);
                    std::string label =
                        std::string(slow_library) + " " + slow_name + " index=" + raw_index;
                    const std::string name = thread_name(diag_tid);
                    if (!name.empty()) label += " thread=\"" + name + "\"";
                    runtime_report().note_slow_host_call(label, diag_tid, elapsed_ms);
                }
                record_thread_activity_done(diag_tid);
#endif  // !ZB_NO_TELEMETRY
                return !exiting_;
            }
            record_thread_activity_done(diag_tid);
            const auto [library, name] = host_call_name(index);
            if (first_time(kSeenHostCall | index)) {
                log("host call %s:%s is not implemented yet", library, name);
            }
            // The guest keeps running with r0 = 0; the report is the only record that survives a
            // device run, because OxygenOS drops our logcat output.
            runtime_report().note_unimplemented_host_call(index, library, name);
            thread.regs()[0] = 0;
            return true;
        }
        if (first_time(kSeenUnexpectedSvc | stop.swi)) {
            log("unexpected svc #0x%x at pc 0x%08x", stop.swi, stop.pc);
        }
        thread.regs()[0] = static_cast<std::uint32_t>(-ENOSYS);
        return true;
    case StopKind::Interrupted:
        return true;
    case StopKind::MemoryFault:
    case StopKind::Exception:
        return fault_or_crash(thread, stop);
    case StopKind::None:
        log("guest stopped without a reason at pc 0x%08x", stop.pc);
        request_exit(1);
        if (thread_count() > 1) exit_host_process();
        return false;
    }
    return false;
}

bool Process::fault_or_crash(GuestThread& thread, const Stop& stop) {
    if (deliver_fault(thread, stop)) return true;
    crash_report(stop, thread);
    request_exit(128 + (stop.kind == StopKind::MemoryFault ? SIGSEGV : SIGILL));
    if (thread_count() > 1) exit_host_process();
    return false;
}

bool Process::after_stop(GuestThread& thread) {
    if (!thread.has_pending_signals(thread.sigmask) || dispatch_pending_signals(thread)) return true;
    if (thread_count() > 1) exit_host_process();
    return false;
}

Process* Process::reporter_process = nullptr;

// The translator does not always throw when it cannot go on: a failed check inside it calls
// std::terminate directly, and the process dies with the message "terminating" and a stripped
// backtrace. Nothing can catch that, but the guest program counter of the thread it happened on
// is the whole answer, and this thread still knows it.
void Process::install_terminate_reporter() {
    static std::terminate_handler previous = nullptr;
    static std::once_flag once;
    // One guest process has one Process, and it outlives everything: the handler needs it only to
    // name the library an address belongs to.
    reporter_process = this;
    std::call_once(once, [] {
        previous = std::set_terminate([] {
            if (GuestThread* thread = Process::current_thread()) {
                // The bare address says nothing: what is needed is the library and the offset in
                // it, which is exactly what the crash report already resolves.
                const std::uint32_t pc = thread->regs()[15];
                std::string where;
                if (reporter_process != nullptr) {
                    std::unique_lock<std::mutex> lock(reporter_process->mm_mutex_, std::try_to_lock);
                    if (lock.owns_lock()) where = reporter_process->describe_address_locked(pc);
                }
                char detail[192];
                std::snprintf(detail, sizeof detail, "pc 0x%08x %s", pc, where.c_str());
                log("terminating while running guest code at %s", detail);
                runtime_report().note_jni_detail("translator-gave-up", detail, true);
            } else {
                runtime_report().note_jni_detail("translator-gave-up", "outside guest code", true);
            }
            if (previous != nullptr) previous();
            std::abort();
        });
    });
}

namespace {

void report_translator_gave_up(Process& process, GuestThread& thread, const char* reason) {
    char where[128];
    if (reason != nullptr && reason[0] != '\0') {
        std::snprintf(where, sizeof where, "pc 0x%08x: %s", thread.regs()[15], reason);
    } else {
        std::snprintf(where, sizeof where, "pc 0x%08x", thread.regs()[15]);
    }
    log("the translator gave up at %s", where);
    runtime_report().note_jni_detail("translator-gave-up", where, true);
    process.request_exit(71);
}

}  // namespace

void Process::thread_loop(GuestThread& thread) {
    set_current_thread(&thread);
    // The translator throws for what it cannot translate (an instruction its backend does not
    // implement), and nothing above this loop can catch it: the process aborts with "terminating"
    // and no hint of which instruction it was. The program counter and the reason are the whole
    // answer, so they go into the report and this guest thread ends instead.
    try {
        while (dispatch_stop(thread, thread.run()) && after_stop(thread)) {
        }
    } catch (const std::exception& failure) {
        report_translator_gave_up(*this, thread, failure.what());
    } catch (...) {
        report_translator_gave_up(*this, thread, nullptr);
    }
}

std::optional<GuestResult> Process::call_guest(GuestThread& thread, std::uint32_t target, const GuestCall& args) {
    try {
        return thread.call(target, args,
                           [&](const Stop& stop) { return dispatch_stop(thread, stop) && after_stop(thread); });
    } catch (const std::exception& failure) {
        report_translator_gave_up(*this, thread, failure.what());
        return std::nullopt;
    } catch (...) {
        report_translator_gave_up(*this, thread, nullptr);
        return std::nullopt;
    }
}

std::int32_t Process::clone_thread(GuestThread& parent, std::uint32_t flags, std::uint32_t stack,
                                   std::uint32_t parent_tid_addr, std::uint32_t tls, std::uint32_t child_tid_addr) {
    const int processor_id = allocate_processor_id();
    if (processor_id < 0) return -EAGAIN;

    const std::size_t code_cache_size =
        parent.child_code_cache_size != 0 ? parent.child_code_cache_size : worker_code_cache_size();
    auto child = std::make_unique<GuestThread>(mem_, monitor_.get(), static_cast<std::size_t>(processor_id),
                                               precise_faults_, code_cache_size);
    configure_thread(*child);
    child->regs() = parent.regs();
    child->regs()[0] = 0;
    if (stack != 0) child->regs()[13] = stack;
    child->ext_regs() = parent.ext_regs();
    child->set_cpsr(parent.cpsr());
    child->set_fpscr(parent.fpscr());
    child->set_tls((flags & kCloneSetTls) ? tls : parent.tls());
    child->sigmask = parent.sigmask;
    if (flags & kCloneChildClearTid) child->clear_child_tid = child_tid_addr;

    auto tid_promise = std::make_shared<std::promise<pid_t>>();
    std::future<pid_t> tid_future = tid_promise->get_future();
    try {
        std::thread([this, owned = std::move(child), promise = tid_promise, flags, parent_tid_addr,
                     child_tid_addr]() mutable {
            const auto tid = static_cast<pid_t>(::syscall(SYS_gettid));
            owned->tid = tid;
            owned->host_tid.store(tid, std::memory_order_release);
            register_thread(owned.get());
            if (flags & kCloneChildSetTid) write_guest_u32(mem_, child_tid_addr, static_cast<std::uint32_t>(tid));
            if (flags & kCloneParentSetTid) write_guest_u32(mem_, parent_tid_addr, static_cast<std::uint32_t>(tid));
            promise->set_value(tid);
            thread_main(std::move(owned));
        }).detach();
    } catch (const std::system_error&) {
        child.reset();
        std::lock_guard<std::mutex> lock(threads_mutex_);
        processor_ids_.reset(static_cast<std::size_t>(processor_id));
        return -EAGAIN;
    }

    const pid_t tid = tid_future.get();
    return tid;
}

void Process::thread_main(std::unique_ptr<GuestThread> thread) {
    thread_loop(*thread);
    // Stop naming the thread before finish_thread() and the unique_ptr free it: a host signal
    // landing here afterwards must not take the forwarding fast path to a freed GuestThread.
    // Handlers on this host thread run synchronously with this code, so there is no race.
    set_current_thread(nullptr);
    finish_thread(thread);
}

void Process::finish_thread(std::unique_ptr<GuestThread>& thread) {
    if (!thread) return;
    if (thread->clear_child_tid != 0) {
        if (std::uint8_t* p = mem_.host_ptr(thread->clear_child_tid, 4, kPageWrite)) {
            const std::uint32_t zero = 0;
            std::memcpy(p, &zero, 4);
            ::syscall(SYS_futex, p, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
        }
    }
    unregister_thread(thread);
}

void Process::unregister_thread(std::unique_ptr<GuestThread>& thread) {
    if (!thread) return;
    release_gl_slot(mem_, *thread);
    const std::size_t processor_id = thread->processor_id();
    const std::int32_t exiting_tid = thread->diagnostic_tid();
    release_thread_activity(exiting_tid);
    // Waits for in-flight forwarding handlers, so the caller may free the thread afterwards.
    clear_process_signal_target(thread.get());
    {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        std::erase(threads_, thread.get());
    }
    monitor_->ClearProcessor(processor_id);
    // Destroy the GuestThread/JIT before releasing its processor_id so no new thread can claim
    // the same ExclusiveMonitor slot while this JIT still exists.
    thread.reset();
    std::lock_guard<std::mutex> lock(threads_mutex_);
    processor_ids_.reset(processor_id);
    threads_cv_.notify_all();
}

void Process::wait_for_threads() {
    std::unique_lock<std::mutex> lock(threads_mutex_);
    threads_cv_.wait(lock, [&] { return threads_.empty(); });
}

void Process::exit_host_process() {
    std::fflush(stderr);
    std::_Exit(exit_status_);
}

void Process::crash_report(const Stop& stop, GuestThread& thread) const {
    char summary[192];
    if (stop.kind == StopKind::MemoryFault) {
        std::snprintf(summary, sizeof summary, "guest SIGSEGV: %s of 0x%08x, pc 0x%08x",
                      stop.fault_write ? "write" : "read", stop.fault_addr, stop.pc);
    } else {
        std::snprintf(summary, sizeof summary, "guest SIGILL: %s at pc 0x%08x", exception_name(stop.exception),
                      stop.pc);
    }
    log("%s", summary);
    // Recorded before request_exit, so the crash and not the exit status reaches the report.
    runtime_report().note_guest_exit(std::string(summary) + " in " + describe_address(stop.pc));
    const auto& r = thread.regs();
    for (int i = 0; i < 16; i += 4) {
        log("  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x", i, r[i], i + 1, r[i + 1], i + 2, r[i + 2], i + 3, r[i + 3]);
    }
    const long host_tid = ::syscall(SYS_gettid);
    if (thread.tid != 0 && thread.tid != host_tid) {
        log("  cpsr %08x  tls %08x  tid %ld  guest tid %d", thread.cpsr(), thread.tls(), host_tid, thread.tid);
    } else {
        log("  cpsr %08x  tls %08x  tid %ld", thread.cpsr(), thread.tls(), host_tid);
    }
    runtime_report().note_crash_detail("recent-jni-calls", jni_recent_calls());
    runtime_report().note_crash_detail("recent-jni-names", jni_recent_call_names());
    runtime_report().note_crash_detail("recent-jni-strings", jni_recent_string_results());
    runtime_report().note_crash_detail("recent-egl-calls", egl_recent_calls());
    log("  pc in %s", describe_address(stop.pc).c_str());
    log("  lr in %s", describe_address(r[14]).c_str());

    // Bounded, allocation-light crash detail for RuntimeReport: this must never crash inside
    // the crash path, since it runs while the process is already dying.
    char regs_line[256];
    std::snprintf(regs_line, sizeof regs_line,
                  "r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x "
                  "r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x r13=%08x r14=%08x r15=%08x",
                  r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12],
                  r[13], r[14], r[15]);
    runtime_report().note_crash_detail("registers", regs_line);

    char state_line[128];
    if (thread.tid != 0 && thread.tid != host_tid) {
        std::snprintf(state_line, sizeof state_line, "cpsr=%08x tls=%08x tid=%ld guest-tid=%d",
                      thread.cpsr(), thread.tls(), host_tid, thread.tid);
    } else {
        std::snprintf(state_line, sizeof state_line, "cpsr=%08x tls=%08x tid=%ld", thread.cpsr(),
                      thread.tls(), host_tid);
    }
    runtime_report().note_crash_detail("state", state_line);

    runtime_report().note_crash_detail("pc", describe_address(stop.pc));
    runtime_report().note_crash_detail("lr", describe_address(r[14]));

    // The faulting instruction's own operand register, when it is a guest pointer that is still
    // mapped: a guest that dereferences a string as a pointer (the WS3 signature) is only
    // explainable from the bytes of the object it read, not from the address alone.
    for (int index = 0; index < 8; ++index) {
        const std::uint32_t value = r[index];
        const std::uint8_t* bytes = mem_.host_ptr(value, 32, kPageRead);
        if (bytes == nullptr) continue;
        std::uint32_t words[8];
        std::memcpy(words, bytes, sizeof words);
        char line[224];
        std::snprintf(line, sizeof line,
                      "r%d[%08x]=%08x %08x %08x %08x %08x %08x %08x %08x", index, value, words[0],
                      words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
        runtime_report().note_crash_detail("reg-memory-" + std::to_string(index), line);
    }

    const std::uint32_t sp = r[13];
    if (const std::uint8_t* stack_bytes = mem_.host_ptr(sp, 32, kPageRead)) {
        std::uint32_t words[8];
        std::memcpy(words, stack_bytes, sizeof words);
        char stack_line[192];
        std::snprintf(stack_line, sizeof stack_line, "%08x %08x %08x %08x %08x %08x %08x %08x",
                      words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
        runtime_report().note_crash_detail("stack", stack_line);
    } else {
        runtime_report().note_crash_detail("stack", "(unreadable)");
    }

    runtime_report().note_crash_detail("precise", precise_faults_ ? "yes" : "no");
}

}  // namespace zb
