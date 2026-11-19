#include "zb/guest_thread.h"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <bit>
#include <climits>
#include <cstdlib>
#include <cstring>

#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/interface/halt_reason.h>

#include "zb/cp15.h"
#include <cstdio>

#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

constexpr Dynarmic::HaltReason kStopHalt = Dynarmic::HaltReason::UserDefined1;
constexpr Dynarmic::HaltReason kInterruptHalt = Dynarmic::HaltReason::UserDefined2;
// With check_halt_on_memory_access, code emitted after each memory access tests this bit and
// returns with PC set to the accessing instruction, so faults are precise.
constexpr Dynarmic::HaltReason kMemoryAbortHalt = Dynarmic::HaltReason::MemoryAbort;

constexpr std::uint32_t kCpsrThumb = 0x20;
constexpr std::uint32_t kCpsrEndian = 0x200;
constexpr std::uint32_t kCpsrItMask = 0x0600FC00;

template <typename T>
T load(const std::uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

template <typename T>
void store(std::uint8_t* p, T v) {
    std::memcpy(p, &v, sizeof v);
}

std::atomic<int> g_unsafe_fp_override{-1};

bool unsafe_fp_enabled() {
    const int override_val = g_unsafe_fp_override.load(std::memory_order_acquire);
    if (override_val >= 0) return override_val != 0;
    static const bool env_enabled = [] {
        const char* v = std::getenv("ZB_UNSAFE_FP");
        return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    return env_enabled;
}

}  // namespace

void set_unsafe_fp_override(bool enabled) {
    g_unsafe_fp_override.store(enabled ? 1 : 0, std::memory_order_release);
}

GuestThread::GuestThread(GuestMemory& mem, Dynarmic::ExclusiveMonitor* monitor, std::size_t processor_id,
                         bool precise_faults, std::size_t code_cache_size)
    : mem_(mem), processor_id_(processor_id) {
    cp15_ = std::make_shared<Cp15>(&tpidruro_, &tpidrurw_);

    Dynarmic::A32::UserConfig cfg;
    cfg.callbacks = this;
    cfg.global_monitor = monitor;
    cfg.processor_id = processor_id;
    cfg.arch_version = Dynarmic::A32::ArchVersion::v8;
    cfg.fastmem_pointer = static_cast<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(mem.base()));
    cfg.fastmem_exclusive_access = true;
    cfg.coprocessors[15] = cp15_;
    cfg.define_unpredictable_behaviour = true;
    cfg.enable_cycle_counting = false;
    cfg.always_little_endian = true;
    cfg.wall_clock_cntpct = true;
    cfg.check_halt_on_memory_access = precise_faults;
    cfg.code_cache_size = code_cache_size;
    if (unsafe_fp_enabled()) {
        cfg.unsafe_optimizations = true;
        cfg.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA |
                             Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue |
                             Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP |
                             Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
    }
    jit_ = std::make_unique<Dynarmic::A32::Jit>(cfg);
}

GuestThread::~GuestThread() = default;

std::array<std::uint32_t, 16>& GuestThread::regs() {
    return jit_->Regs();
}

std::array<std::uint32_t, 64>& GuestThread::ext_regs() {
    return jit_->ExtRegs();
}

std::uint32_t GuestThread::cpsr() const {
    return jit_->Cpsr();
}

std::uint32_t GuestThread::fpscr() const {
    return jit_->Fpscr();
}

void GuestThread::set_fpscr(std::uint32_t value) {
    jit_->SetFpscr(value);
}

void GuestThread::set_cpsr(std::uint32_t value) {
    jit_->SetCpsr(value);
}

// The translator throws for an instruction its backend cannot emit, and that throw crosses every
// entry into translated code: the run loop, a Java-to-guest native call, a nested guest call. It
// is caught here, at the one place they all share, and turned into the same stop a guest fault
// produces, so the caller unwinds normally and the report names the program counter instead of
// the process dying with "terminating".
Stop GuestThread::translator_gave_up(const char* reason) {
    const std::uint32_t pc = regs()[15];
    char detail[128];
    std::snprintf(detail, sizeof detail, "pc 0x%08x: %s", pc, reason != nullptr ? reason : "unknown");
    log("the translator gave up at %s", detail);
    runtime_report().note_jni_detail("translator-gave-up", detail, true);
    Stop stop;
    stop.kind = StopKind::Exception;
    stop.exception = Dynarmic::A32::Exception::UndefinedInstruction;
    stop.pc = pc;
    return stop;
}

Stop GuestThread::run() {
    pending_ = Stop{};
    for (;;) {
        Dynarmic::HaltReason reason{};
        try {
            reason = jit_->Run();
        } catch (const std::exception& failure) {
            return translator_gave_up(failure.what());
        } catch (...) {
            return translator_gave_up(nullptr);
        }
        jit_->ClearHalt(kStopHalt | kMemoryAbortHalt);
        if (pending_.kind == StopKind::None && Dynarmic::Has(reason, kInterruptHalt)) {
            jit_->ClearHalt(kInterruptHalt);
            pending_.kind = StopKind::Interrupted;
            break;
        }
        // Another thread invalidated translated code while we were running; the JIT applies
        // the invalidation at the start of the next Run().
        if (pending_.kind == StopKind::None && Dynarmic::Has(reason, Dynarmic::HaltReason::CacheInvalidation)) continue;
        break;
    }
    Stop s = pending_;
    if (s.kind != StopKind::Exception) s.pc = jit_->Regs()[15];
    return s;
}

std::optional<GuestResult> GuestThread::call(std::uint32_t target, const GuestCall& args,
                                             const GuestStopHandler& handle_stop) {
    if (in_fast_svc_) {
        log("internal error: nested guest call attempted inside in-JIT fast svc");
        std::abort();
    }
    const auto saved_regs = regs();
    const auto saved_ext = ext_regs();
    const std::uint32_t saved_cpsr = cpsr();
    const std::uint32_t saved_fpscr = fpscr();

    const auto restore = [&] {
        regs() = saved_regs;
        ext_regs() = saved_ext;
        set_cpsr(saved_cpsr);
        set_fpscr(saved_fpscr);
    };

    const std::uint64_t bytes = static_cast<std::uint64_t>(args.stack.size()) * 4;
    if (bytes > saved_regs[13]) return std::nullopt;
    const std::uint32_t call_sp = static_cast<std::uint32_t>((saved_regs[13] - bytes) & ~7u);
    std::uint8_t* stack = mem_.host_ptr(call_sp, bytes, kPageWrite);
    if (bytes != 0 && stack == nullptr) return std::nullopt;
    if (bytes != 0) std::memcpy(stack, args.stack.data(), static_cast<std::size_t>(bytes));

    regs()[0] = args.regs[0];
    regs()[1] = args.regs[1];
    regs()[2] = args.regs[2];
    regs()[3] = args.regs[3];
    regs()[13] = call_sp;
    regs()[14] = kHostReturnAddress;
    regs()[15] = target & ~1u;
    // A fresh call starts outside any IT block with little-endian data; only T follows the target.
    set_cpsr((saved_cpsr & ~(kCpsrThumb | kCpsrItMask | kCpsrEndian)) | ((target & 1u) ? kCpsrThumb : 0));

    ++call_depth;
    struct CallScope {
        int& depth;
        const decltype(restore)& fn;
        ~CallScope() {
            --depth;
            fn();
        }
    } scope{call_depth, restore};

    std::optional<GuestResult> result;
    for (;;) {
        const Stop stop = run();
        if (stop.kind == StopKind::Svc && stop.swi == kHostReturnSwi) {
            if (regs()[13] == call_sp) {
                result = GuestResult{regs()[0], regs()[1]};
                break;
            }
            // Not this frame's return: the handler treats the svc as an illegal instruction.
            log("host-to-guest return with sp 0x%08x, frame sp 0x%08x: a longjmp or unwind crossed the "
                "host-to-guest call frame",
                regs()[13], call_sp);
        }
        if (!handle_stop(stop)) break;
    }
    return result;
}

void GuestThread::invalidate(std::uint32_t addr, std::uint64_t len) {
    if (len == 0) return;
    if (len >= kGuestSpaceSize || static_cast<std::uint64_t>(addr) + len > kGuestSpaceSize) {
        jit_->ClearCache();
        return;
    }
    jit_->InvalidateCacheRange(addr, static_cast<std::size_t>(len));
}

void GuestThread::begin_blocking() {
    blocking_.store(1, std::memory_order_seq_cst);
}

void GuestThread::end_blocking() {
    blocking_.store(0, std::memory_order_seq_cst);
}

void GuestThread::interrupt() {
    // seq_cst: the poster sets the pending bit before reading this flag, and the waiter sets the
    // flag before it checks for pending signals, so at least one of the two sees the other.
    const std::int32_t active = host_tid.load(std::memory_order_seq_cst);
    const std::int32_t target = active != 0 ? active : tid;
    if (blocking_.load(std::memory_order_seq_cst) == 0 || target == 0) return;
    ::syscall(SYS_tgkill, ::getpid(), target, host_interrupt_signal());
}

void GuestThread::post_signal(const g::siginfo32& info) {
    const int sig = info.si_signo;
    if (sig < 1 || sig > 64) return;
    const std::size_t slot = static_cast<std::size_t>(sig);
    std::uint8_t expected = 0;
    if (pending_state_[slot].compare_exchange_strong(expected, 1, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
        pending_info_[slot] = info;
        pending_state_[slot].store(2, std::memory_order_release);
        pending_signals_.fetch_or(1ULL << (sig - 1), std::memory_order_seq_cst);
    } else if (expected == 2) {
        // Already populated and ready: coalesce without overwriting siginfo32 in flight.
        pending_signals_.fetch_or(1ULL << (sig - 1), std::memory_order_seq_cst);
    }
    jit_->HaltExecution(kInterruptHalt);
    wake();
    interrupt();
}

void GuestThread::park(std::uint32_t token) {
    ::syscall(SYS_futex, &park_word_, FUTEX_WAIT_PRIVATE, token, nullptr, nullptr, 0);
}

void GuestThread::wake() {
    park_word_.fetch_add(1);
    ::syscall(SYS_futex, &park_word_, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
}

bool GuestThread::take_signal(std::uint64_t blocked, g::siginfo32& out) {
    std::uint64_t pending = pending_signals_.load(std::memory_order_acquire);
    for (;;) {
        const std::uint64_t deliverable = pending & ~blocked;
        if (deliverable == 0) return false;
        const int bit = std::countr_zero(deliverable);
        const std::size_t slot = static_cast<std::size_t>(bit + 1);
        if (pending_signals_.compare_exchange_weak(pending, pending & ~(1ULL << bit),
                                                   std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (pending_state_[slot].load(std::memory_order_acquire) == 2) {
                out = pending_info_[slot];
                pending_state_[slot].store(0, std::memory_order_release);
            } else {
                out = g::siginfo32{};
                out.si_signo = static_cast<std::int32_t>(slot);
            }
            return true;
        }
    }
}

void GuestThread::halt() {
    jit_->HaltExecution(kStopHalt);
}

bool GuestThread::check_access(std::uint32_t vaddr, std::uint32_t len, std::uint8_t need, bool write) {
    if (mem_.accessible(vaddr, len, need)) return true;
    if (pending_.kind == StopKind::None) {
        pending_.kind = StopKind::MemoryFault;
        pending_.fault_addr = vaddr;
        pending_.fault_write = write;
    }
    jit_->HaltExecution(kMemoryAbortHalt);
    return false;
}

std::uint8_t GuestThread::MemoryRead8(std::uint32_t vaddr) {
    return check_access(vaddr, 1, kPageRead, false) ? mem_.base()[vaddr] : 0;
}

std::uint16_t GuestThread::MemoryRead16(std::uint32_t vaddr) {
    return check_access(vaddr, 2, kPageRead, false) ? load<std::uint16_t>(mem_.base() + vaddr) : 0;
}

std::uint32_t GuestThread::MemoryRead32(std::uint32_t vaddr) {
    return check_access(vaddr, 4, kPageRead, false) ? load<std::uint32_t>(mem_.base() + vaddr) : 0;
}

std::uint64_t GuestThread::MemoryRead64(std::uint32_t vaddr) {
    return check_access(vaddr, 8, kPageRead, false) ? load<std::uint64_t>(mem_.base() + vaddr) : 0;
}

void GuestThread::MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) {
    if (check_access(vaddr, 1, kPageWrite, true)) mem_.base()[vaddr] = value;
}

void GuestThread::MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) {
    if (check_access(vaddr, 2, kPageWrite, true)) store(mem_.base() + vaddr, value);
}

void GuestThread::MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) {
    if (check_access(vaddr, 4, kPageWrite, true)) store(mem_.base() + vaddr, value);
}

void GuestThread::MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) {
    if (check_access(vaddr, 8, kPageWrite, true)) store(mem_.base() + vaddr, value);
}

namespace {

std::mutex& unaligned_exclusive_mutex() {
    static std::mutex mu;
    return mu;
}

template <typename T>
bool atomic_write_exclusive(std::uint8_t* base, std::uint32_t vaddr, T value, T expected) {
    if constexpr (sizeof(T) == 1) {
        return __atomic_compare_exchange_n(base + vaddr, &expected, value, false, __ATOMIC_SEQ_CST,
                                           __ATOMIC_SEQ_CST);
    } else {
        if ((vaddr & (sizeof(T) - 1)) == 0) {
            return __atomic_compare_exchange_n(reinterpret_cast<T*>(base + vaddr), &expected, value,
                                               false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        }
        std::lock_guard<std::mutex> lock(unaligned_exclusive_mutex());
        T current{};
        std::memcpy(&current, base + vaddr, sizeof(T));
        if (current != expected) return false;
        std::memcpy(base + vaddr, &value, sizeof(T));
        return true;
    }
}

}  // namespace

// Hardware CAS when naturally aligned (matching JIT fastmem LDAXR/STLXR), with mutex fallback
// only for rare unaligned exclusive accesses.
bool GuestThread::MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t expected) {
    if (!check_access(vaddr, 1, kPageRead | kPageWrite, true)) return false;
    return atomic_write_exclusive<std::uint8_t>(mem_.base(), vaddr, value, expected);
}

bool GuestThread::MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t expected) {
    if (!check_access(vaddr, 2, kPageRead | kPageWrite, true)) return false;
    return atomic_write_exclusive<std::uint16_t>(mem_.base(), vaddr, value, expected);
}

bool GuestThread::MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t expected) {
    if (!check_access(vaddr, 4, kPageRead | kPageWrite, true)) return false;
    return atomic_write_exclusive<std::uint32_t>(mem_.base(), vaddr, value, expected);
}

bool GuestThread::MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t expected) {
    if (!check_access(vaddr, 8, kPageRead | kPageWrite, true)) return false;
    return atomic_write_exclusive<std::uint64_t>(mem_.base(), vaddr, value, expected);
}

std::optional<std::uint32_t> GuestThread::MemoryReadCode(std::uint32_t vaddr) {
    if (!mem_.accessible(vaddr, 4, kPageExec)) return std::nullopt;
    return load<std::uint32_t>(mem_.base() + vaddr);
}

bool GuestThread::IsReadOnlyMemory(std::uint32_t vaddr) {
    return mem_.accessible(vaddr, 8, kPageRead | kPageExec) && !mem_.any_flag(vaddr, 8, kPageWrite);
}

void GuestThread::InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) {
    log("interpreter fallback requested at pc 0x%08x (%zu instructions)", pc, num_instructions);
    if (pending_.kind == StopKind::None) {
        pending_.kind = StopKind::Exception;
        pending_.exception = Dynarmic::A32::Exception::UndefinedInstruction;
        pending_.pc = pc;
    }
    halt();
}

void GuestThread::CallSVC(std::uint32_t swi) {
    if (pending_.kind == StopKind::None && fast_svc_handler_ != nullptr &&
        !has_pending_signals(sigmask)) {
        in_fast_svc_ = true;
        const bool handled = fast_svc_handler_(fast_svc_ctx_, *this, swi);
        in_fast_svc_ = false;
        if (handled) return;
    }
    if (pending_.kind == StopKind::None) {
        pending_.kind = StopKind::Svc;
        pending_.swi = swi;
    }
    halt();
}

void GuestThread::ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) {
    if (pending_.kind == StopKind::None) {
        pending_.kind = StopKind::Exception;
        pending_.exception = exception;
        pending_.pc = pc;
    }
    halt();
}

}  // namespace zb
