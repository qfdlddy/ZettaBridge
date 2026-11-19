// Verifies inline ARM64 fastmem exclusive monitor correctness: ABA rejection across 8/16/32/64
// widths, non-exclusive store detection, CLREX, cross-stripe nested LDREX fallback, fastmem fault
// unlock recovery, and multi-thread atomic increment spanning multiple stripe_active words.
#include <sys/mman.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <dynarmic/interface/exclusive_monitor.h>

#include "check.h"
#include "zb/guest_memory.h"
#include "zb/guest_thread.h"

namespace {

constexpr std::uint32_t kCpsrUser = 0x10;
constexpr std::uint32_t kSvcStop = 0xEF5AFFFF;

void put32(zb::GuestMemory& m, std::uint32_t addr, std::uint32_t value) {
    std::memcpy(m.base() + addr, &value, 4);
}

template <typename T>
T read_val(zb::GuestMemory& m, std::uint32_t addr) {
    T v{};
    std::memcpy(&v, m.base() + addr, sizeof v);
    return v;
}

template <typename T>
void write_val(zb::GuestMemory& m, std::uint32_t addr, T v) {
    std::memcpy(m.base() + addr, &v, sizeof v);
}

std::uint64_t read_data64(zb::GuestMemory& m, std::uint32_t addr) {
    return read_val<std::uint64_t>(m, addr);
}

void write_data64(zb::GuestMemory& m, std::uint32_t addr, std::uint64_t v) {
    write_val<std::uint64_t>(m, addr, v);
}

void test_aba_widths(zb::GuestMemory& mem, Dynarmic::ExclusiveMonitor& monitor) {
    zb::GuestThread t0(mem, &monitor, 0, true);
    zb::GuestThread t1(mem, &monitor, 1, true);
    t0.set_cpsr(kCpsrUser);
    t1.set_cpsr(kCpsrUser);

    struct WidthCase {
        const char* name;
        std::uint32_t ldrex_insn;
        std::uint32_t strex_insn;
        std::uint32_t pc_base;
        std::uint64_t val_a;
        std::uint64_t val_b;
        std::uint64_t val_c;
        std::uint64_t mask;
    } cases[] = {
        {"8bit", 0xE1D01F9F, 0xE1C02F93, 0x10000, 0x11, 0x22, 0x33, 0xFF},
        {"16bit", 0xE1F01F9F, 0xE1E02F93, 0x10040, 0x1122, 0x3344, 0x5566, 0xFFFF},
        {"32bit", 0xE1901F9F, 0xE1802F93, 0x10080, 0x11223344u, 0x55667788u, 0x99AABBCu, 0xFFFFFFFFu},
        {"64bit", 0xE1B06F9F, 0xE1A02F94, 0x100C0, 0x0102030405060708ull, 0x1112131415161718ull,
         0x2122232425262728ull, ~0ull},
    };

    for (const auto& c : cases) {
        // Two-instruction sequence on a page with SVC split so t1 runs A->B->A between LDREX & STREX.
        // LDREX at pc_base, SVC at pc_base+4, STREX at pc_base+8, SVC at pc_base+12
        for (int iter = 0; iter < 50; ++iter) {
            const std::uint32_t data_addr = 0x20000 + (static_cast<std::uint32_t>(iter) & 0x3F) * 16;
            write_data64(mem, data_addr, c.val_a);

            // Thread 0 starts LDREX [r0]
            t0.regs()[0] = data_addr;
            t0.regs()[2] = 0xDEAD;
            t0.regs()[3] = static_cast<std::uint32_t>(c.val_c);
            t0.regs()[4] = static_cast<std::uint32_t>(c.val_c);
            t0.regs()[5] = static_cast<std::uint32_t>(c.val_c >> 32);
            t0.regs()[15] = c.pc_base;
            zb::Stop s0 = t0.run();
            CHECK(s0.kind == zb::StopKind::Svc);
            CHECK(t0.regs()[15] == c.pc_base + 8);

            // Thread 1 performs A -> B then B -> A cleanly while Thread 0 is suspended after LDREX.
            t1.regs()[0] = data_addr;
            t1.regs()[3] = static_cast<std::uint32_t>(c.val_b);
            t1.regs()[4] = static_cast<std::uint32_t>(c.val_b);
            t1.regs()[5] = static_cast<std::uint32_t>(c.val_b >> 32);
            t1.regs()[15] = c.pc_base;
            CHECK(t1.run().kind == zb::StopKind::Svc);
            CHECK(t1.run().kind == zb::StopKind::Svc);
            CHECK(t1.regs()[2] == 0);
            CHECK((read_data64(mem, data_addr) & c.mask) == (c.val_b & c.mask));

            t1.regs()[3] = static_cast<std::uint32_t>(c.val_a);
            t1.regs()[4] = static_cast<std::uint32_t>(c.val_a);
            t1.regs()[5] = static_cast<std::uint32_t>(c.val_a >> 32);
            t1.regs()[15] = c.pc_base;
            CHECK(t1.run().kind == zb::StopKind::Svc);
            CHECK(t1.run().kind == zb::StopKind::Svc);
            CHECK(t1.regs()[2] == 0);
            CHECK((read_data64(mem, data_addr) & c.mask) == (c.val_a & c.mask));

            // Now memory equals val_a again! Thread 0 attempts STREX(val_c) and MUST FAIL (r2 == 1).
            CHECK(t0.run().kind == zb::StopKind::Svc);
            if (t0.regs()[2] != 1) {
                std::fprintf(stderr, "ABA BUG in %s iter=%d r2=%u\n", c.name, iter, t0.regs()[2]);
                CHECK(t0.regs()[2] == 1);
            }
            CHECK((read_data64(mem, data_addr) & c.mask) == (c.val_a & c.mask));
        }
    }
    std::puts("  aba_widths PASS");
}

void test_clrex_nonexcl_cross_stripe_and_fault(zb::GuestMemory& mem, Dynarmic::ExclusiveMonitor& monitor) {
    zb::GuestThread t0(mem, &monitor, 0, true);
    t0.set_cpsr(kCpsrUser);

    // 1. Non-exclusive store before STREX fails even though monitor wasn't cleared via STREX.
    write_data64(mem, 0x20000, 100);
    t0.regs()[0] = 0x20000;
    t0.regs()[2] = 0xDEAD;
    t0.regs()[3] = 200;
    t0.regs()[15] = 0x10080;  // LDREX [r0]; SVC
    CHECK(t0.run().kind == zb::StopKind::Svc);
    write_data64(mem, 0x20000, 999);  // plain non-exclusive overwrite
    CHECK(t0.run().kind == zb::StopKind::Svc);
    CHECK(t0.regs()[2] == 1);
    CHECK(read_val<std::uint32_t>(mem, 0x20000) == 999);

    // 2. CLREX clears reservation: LDREX [r0]; CLREX; STREX r2, r3, [r0]; SVC at 0x10100.
    write_data64(mem, 0x20000, 10);
    t0.regs()[0] = 0x20000;
    t0.regs()[2] = 0xDEAD;
    t0.regs()[3] = 20;
    t0.regs()[15] = 0x10100;
    CHECK(t0.run().kind == zb::StopKind::Svc);
    CHECK(t0.regs()[2] == 1);
    CHECK(read_val<std::uint32_t>(mem, 0x20000) == 10);

    // 3. Cross-stripe nested LDREX fallback + Rvalue == Xaddr aliasing (LDREX r0, [r0]) at 0x10140:
    //    LDREX r1, [r0]; LDREX r0, [r1 + offset via r6]; STREX r2, r3, [r6]; SVC
    write_data64(mem, 0x20000, 0x1111);
    write_data64(mem, 0x20004, 0x2222);
    t0.regs()[0] = 0x20000;
    t0.regs()[6] = 0x20004;
    t0.regs()[2] = 0xDEAD;
    t0.regs()[3] = 0x3333;
    t0.regs()[15] = 0x10140;
    CHECK(t0.run().kind == zb::StopKind::Svc);
    CHECK(t0.regs()[2] == 0);
    CHECK(read_val<std::uint32_t>(mem, 0x20004) == 0x3333);

    // 4. Fastmem SIGSEGV while stripe spinlock is held: must abort cleanly AND release stripe lock!
    // Unmapped LDREX at 0x10080 -> address 0x30000
    t0.regs()[0] = 0x30000;
    t0.regs()[15] = 0x10080;
    CHECK(t0.run().kind == zb::StopKind::MemoryFault);

    // Fastmem STLXR SIGSEGV while stripe spinlock is held: LDREX on RW page, switch page to RO,
    // then STREX on the SAME vaddr so execution reaches STLXR inside the stripe lock and faults.
    write_data64(mem, 0x20000, 0x77);
    t0.regs()[0] = 0x20000;
    t0.regs()[15] = 0x10080;
    CHECK(t0.run().kind == zb::StopKind::Svc);
    CHECK(mem.protect(0x20000, 0x1000, PROT_READ));
    CHECK(t0.run().kind == zb::StopKind::MemoryFault);
    CHECK(mem.protect(0x20000, 0x1000, PROT_READ | PROT_WRITE));

    // Verify stripe lock corresponding to 0x30000 and 0x20000 was released: t1 succeeds on 0x20000.
    zb::GuestThread t1(mem, &monitor, 1, true);
    t1.set_cpsr(kCpsrUser);
    write_data64(mem, 0x20000, 50);
    t1.regs()[0] = 0x20000;
    t1.regs()[3] = 51;
    t1.regs()[15] = 0x10080;
    CHECK(t1.run().kind == zb::StopKind::Svc);
    CHECK(t1.run().kind == zb::StopKind::Svc);
    CHECK(t1.regs()[2] == 0);
    CHECK(read_val<std::uint32_t>(mem, 0x20000) == 51);
    std::puts("  clrex_nonexcl_cross_stripe_and_fault PASS");
}

void test_multithread_word_span(zb::GuestMemory& mem, Dynarmic::ExclusiveMonitor& monitor) {
    // 4 threads spanning proc_id words 0, 1, 2 (0, 1, 65, 130) hammering one counter via LDREX/STREX loop.
    constexpr int kThreads = 4;
    constexpr int kProcIds[kThreads] = {0, 1, 65, 130};
    constexpr std::uint32_t kIters = 15000;
    write_val<std::uint32_t>(mem, 0x20000, 0);

    std::atomic<int> ready{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            zb::GuestThread t(mem, &monitor, kProcIds[i], false);
            t.set_cpsr(kCpsrUser);
            t.regs()[0] = 0x20000;
            t.regs()[3] = kIters;
            t.regs()[15] = 0x10180;
            ready.fetch_add(1, std::memory_order_release);
            while (ready.load(std::memory_order_acquire) < kThreads) {}
            const zb::Stop stop = t.run();
            CHECK(stop.kind == zb::StopKind::Svc);
        });
    }
    for (auto& th : threads) th.join();

    const std::uint32_t final_val = read_val<std::uint32_t>(mem, 0x20000);
    std::printf("  multithread counter=%u (expected %u)\n", final_val, kThreads * kIters);
    CHECK(final_val == kThreads * kIters);
}

}  // namespace

int main() {
    zb::GuestMemory mem;
    CHECK(mem.ok());
    Dynarmic::ExclusiveMonitor monitor(132);
    CHECK(mem.map_anon(0x10000, 0x1000, PROT_READ | PROT_WRITE));
    CHECK(mem.map_anon(0x20000, 0x1000, PROT_READ | PROT_WRITE));

    // Code layouts for 8/16/32/64-bit split LDREX; SVC; STREX; SVC:
    // 0x10000 (8-bit):  LDREXB r1, [r0]; SVC; STREXB r2, r3, [r0]; SVC
    put32(mem, 0x10000, 0xE1D01F9F);
    put32(mem, 0x10004, kSvcStop);
    put32(mem, 0x10008, 0xE1C02F93);
    put32(mem, 0x1000C, kSvcStop);

    // 0x10040 (16-bit): LDREXH r1, [r0]; SVC; STREXH r2, r3, [r0]; SVC
    put32(mem, 0x10040, 0xE1F01F9F);
    put32(mem, 0x10044, kSvcStop);
    put32(mem, 0x10048, 0xE1E02F93);
    put32(mem, 0x1004C, kSvcStop);

    // 0x10080 (32-bit): LDREX r1, [r0]; SVC; STREX r2, r3, [r0]; SVC
    put32(mem, 0x10080, 0xE1901F9F);
    put32(mem, 0x10084, kSvcStop);
    put32(mem, 0x10088, 0xE1802F93);
    put32(mem, 0x1008C, kSvcStop);

    // 0x100C0 (64-bit): LDREXD r6, r7, [r0]; SVC; STREXD r2, r4, r5, [r0]; SVC
    put32(mem, 0x100C0, 0xE1B06F9F);
    put32(mem, 0x100C4, kSvcStop);
    put32(mem, 0x100C8, 0xE1A02F94);
    put32(mem, 0x100CC, kSvcStop);

    // 0x10100: LDREX r1, [r0]; CLREX; STREX r2, r3, [r0]; SVC
    put32(mem, 0x10100, 0xE1901F9F);
    put32(mem, 0x10104, 0xF57FF01F);
    put32(mem, 0x10108, 0xE1802F93);
    put32(mem, 0x1010C, kSvcStop);

    // 0x10140: LDREX r1, [r0]; MOV r0, r6; LDREX r0, [r0] (aliases Rvalue == Xaddr); STREX r2, r3, [r6]; SVC
    put32(mem, 0x10140, 0xE1901F9F);
    put32(mem, 0x10144, 0xE1A00006);  // mov r0, r6
    put32(mem, 0x10148, 0xE1900F9F);  // ldrex r0, [r0]
    put32(mem, 0x1014C, 0xE1862F93);  // strex r2, r3, [r6]
    put32(mem, 0x10150, kSvcStop);

    // 0x10180: concurrent increment loop (r0 = ptr, r3 = count):
    //   0x10180: ldrex r1, [r0]
    //   0x10184: add r1, r1, #1
    //   0x10188: strex r2, r1, [r0]
    //   0x1018C: cmp r2, #0
    //   0x10190: bne 0x10180
    //   0x10194: subs r3, r3, #1
    //   0x10198: bne 0x10180
    //   0x1019C: svc #0x5affff
    put32(mem, 0x10180, 0xE1901F9F);
    put32(mem, 0x10184, 0xE2811001);
    put32(mem, 0x10188, 0xE1802F91);
    put32(mem, 0x1018C, 0xE3520000);
    put32(mem, 0x10190, 0x1AFFFFFA);
    put32(mem, 0x10194, 0xE2533001);
    put32(mem, 0x10198, 0x1AFFFFF8);
    put32(mem, 0x1019C, kSvcStop);

    CHECK(mem.protect(0x10000, 0x1000, PROT_READ | PROT_EXEC));

    test_aba_widths(mem, monitor);
    test_clrex_nonexcl_cross_stripe_and_fault(mem, monitor);
    test_multithread_word_span(mem, monitor);

    std::puts("exclusive_aba_test PASS");
    return 0;
}
