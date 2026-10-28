#include "selftest.h"
#include "coroutines.h"
#include "kernel.h"
#include "sampler.h"
#include "write_watch.h"
#include "platform.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace Mmio { void LongJump(::PPCContext& ctx, uint8_t* base, jmp_buf& buffer, int value); }

namespace
{
#define SELFTEST_CHECK(cond, msg, ...)                                              \
    do                                                                              \
    {                                                                               \
        if (!(cond))                                                                \
        {                                                                           \
            fprintf(stderr, "  [FAIL] %s:%d: " msg "\n",                            \
                __FILE__, __LINE__, ##__VA_ARGS__);                                 \
            fflush(stderr);                                                         \
            Guest::Shutdown();                                                      \
            return 1;                                                               \
        }                                                                           \
    } while (0)

    int RunVmSelfTest()
    {
        printf("=== Gate 3 Self-Test: 4 GB VM, 3x Physical Aliasing & Fault Handler ===\n");
        fflush(stdout);

#ifndef _WIN32
        setenv("COD3_WRITEWATCH", "1", 1);
#else
        _putenv_s("COD3_WRITEWATCH", "1");
#endif

        // Use default.xex if present so XEX loading + import binding are also
        // exercised; otherwise initialize the VM and fault handlers standalone.
        std::filesystem::path defaultXex = "CoD3RecompLib/private/default.xex";
        if (!std::filesystem::exists(defaultXex))
            defaultXex = Platform::ExecutableDirectory() / "../../CoD3RecompLib/private/default.xex";
        const std::string defaultXexStr = defaultXex.string();
        const bool hasXex = std::filesystem::exists(defaultXex);
        const char* xexArg = hasXex ? defaultXexStr.c_str() : nullptr;

        SELFTEST_CHECK(Guest::Initialize(xexArg), "Guest::Initialize failed");
        SELFTEST_CHECK(Guest::Base != nullptr, "Guest::Base is null");

        // 1. Verify 32-byte alignment and 4 GB reservation state.
        const uintptr_t baseAddr = reinterpret_cast<uintptr_t>(Guest::Base);
        SELFTEST_CHECK((baseAddr & 0x1Fu) == 0,
            "Guest::Base (%p) is not 32-byte aligned", (void*)Guest::Base);

        MEMORY_BASIC_INFORMATION mbi{};
        SELFTEST_CHECK(VirtualQuery(Guest::Base, &mbi, sizeof(mbi)) == sizeof(mbi),
            "VirtualQuery on Guest::Base failed");
        SELFTEST_CHECK(mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE,
            "Low memory at Guest::Base not committed RW (State=0x%X Protect=0x%X)",
            unsigned(mbi.State), unsigned(mbi.Protect));

        constexpr uint32_t UncommittedProbe = 0x20001234u;
        constexpr uint32_t UncommittedBlock = UncommittedProbe & ~0xFFFFu;
        SELFTEST_CHECK(VirtualQuery(Guest::Base + UncommittedBlock, &mbi, sizeof(mbi)) == sizeof(mbi),
            "VirtualQuery on uncommitted probe block failed");
        SELFTEST_CHECK(mbi.State == MEM_RESERVE && mbi.Protect == PAGE_NOACCESS,
            "Expected uncommitted block 0x%08X to be MEM_RESERVE/PAGE_NOACCESS (got State=0x%X Protect=0x%X)",
            UncommittedBlock, unsigned(mbi.State), unsigned(mbi.Protect));
        printf("  [PASS] 1. Guest::Base (%p) is 32-byte aligned and 4 GB range is reserved\n",
            (void*)Guest::Base);

        // 2. Verify 3x 512 MB physical memory aliasing (0xA0000000, 0xC0000000, 0xE0000000).
        volatile uint32_t* aliasA = reinterpret_cast<volatile uint32_t*>(Guest::Base + 0xA0001234ull);
        volatile uint32_t* aliasC = reinterpret_cast<volatile uint32_t*>(Guest::Base + 0xC0001234ull);
        volatile uint32_t* aliasE = reinterpret_cast<volatile uint32_t*>(Guest::Base + 0xE0001234ull);

        *aliasA = 0xCAFEBABEu;
        SELFTEST_CHECK(*aliasC == 0xCAFEBABEu,
            "Alias 0xC0001234 read 0x%08X, expected 0xCAFEBABE", *aliasC);
        SELFTEST_CHECK(*aliasE == 0xCAFEBABEu,
            "Alias 0xE0001234 read 0x%08X, expected 0xCAFEBABE", *aliasE);

        *aliasC = 0xDEADBEEFu;
        SELFTEST_CHECK(*aliasA == 0xDEADBEEFu && *aliasE == 0xDEADBEEFu,
            "Write through 0xC0001234 not visible at 0xA0001234/0xE0001234");

        *aliasE = 0x1337C0DEu;
        SELFTEST_CHECK(*aliasA == 0x1337C0DEu && *aliasC == 0x1337C0DEu,
            "Write through 0xE0001234 not visible at 0xA0001234/0xC0001234");
        printf("  [PASS] 2. 3x 512 MB physical memory views (0xA0000000, 0xC0000000, 0xE0000000) are aliased\n");

        // 3. Verify SIGBUS/SIGSEGV demand-commit fault recovery on an uncommitted page.
        const uint64_t committedBefore = Guest::DemandCommitted();
        Guest::Write32(Guest::Base, UncommittedProbe, 0xABCD1234u);
        const uint32_t readBack = Guest::Read32(Guest::Base, UncommittedProbe);
        const uint64_t committedAfter = Guest::DemandCommitted();

        SELFTEST_CHECK(readBack == 0xABCD1234u,
            "Demand-committed readback mismatch: got 0x%08X, expected 0xABCD1234", readBack);
        SELFTEST_CHECK(committedAfter == committedBefore + (64u << 10),
            "Expected DemandCommitted to advance by 64 KB (before=%llu after=%llu)",
            (unsigned long long)committedBefore, (unsigned long long)committedAfter);
        SELFTEST_CHECK(VirtualQuery(Guest::Base + UncommittedBlock, &mbi, sizeof(mbi)) == sizeof(mbi),
            "VirtualQuery after demand-commit failed");
        SELFTEST_CHECK(mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE,
            "Demand-committed block not MEM_COMMIT/PAGE_READWRITE (State=0x%X Protect=0x%X)",
            unsigned(mbi.State), unsigned(mbi.Protect));
#ifndef _WIN32
        size_t duplicateCommitBytes = 12345;
        SELFTEST_CHECK(Platform::CommitExternalRange(Guest::Base + UncommittedBlock, 64u << 10, PAGE_READWRITE, duplicateCommitBytes) &&
                       duplicateCommitBytes == 0,
            "Expected 0 newly committed bytes on already-committed 64 KB block (got %zu)", duplicateCommitBytes);
#endif
        Kernel::FlushFaultDiagnostics();
        printf("  [PASS] 3. SIGBUS/SIGSEGV fault handler demand-committed 64 KB block at 0x%08X and resumed execution\n",
            UncommittedBlock);

        // 4. Verify 16 KB WriteWatch arming, read non-faulting, ESR_EL1 WnR write-fault decoding, and disarming.
#ifdef __APPLE__
        constexpr uint32_t WatchPageSize = 16u << 10;
#else
        constexpr uint32_t WatchPageSize = 4u << 10;
#endif
        constexpr uint32_t WatchPhysOffset = 0x00040000u;
        Guest::Write32(Guest::Base, 0xA0000000u + WatchPhysOffset + 0x100u, 0x11223344u);

        const uint64_t seqBefore = WriteWatch::Sequence();
        SELFTEST_CHECK(WriteWatch::Arm(WatchPhysOffset, WatchPageSize),
            "WriteWatch::Arm failed on physical offset 0x%08X", WatchPhysOffset);
        SELFTEST_CHECK(WriteWatch::Clean(WatchPhysOffset, WatchPageSize, seqBefore),
            "WriteWatch::Clean returned false immediately after Arm");

        // Reading through all 3 physical aliases must NOT fault or mark the page dirty.
        SELFTEST_CHECK(Guest::Read32(Guest::Base, 0xA0000000u + WatchPhysOffset + 0x100u) == 0x11223344u,
            "Read through 0xA0000000 alias failed while armed");
        SELFTEST_CHECK(Guest::Read32(Guest::Base, 0xC0000000u + WatchPhysOffset + 0x100u) == 0x11223344u,
            "Read through 0xC0000000 alias failed while armed");
        SELFTEST_CHECK(Guest::Read32(Guest::Base, 0xE0000000u + WatchPhysOffset + 0x100u) == 0x11223344u,
            "Read through 0xE0000000 alias failed while armed");
        SELFTEST_CHECK(WriteWatch::Clean(WatchPhysOffset, WatchPageSize, seqBefore),
            "WriteWatch::Clean returned false after read-only accesses");

        // Writing through the 0xC0000000 alias must trigger a write fault (ESR_EL1 WnR=1),
        // disarm all 3 windows, mark the page dirty, and complete the store.
        Guest::Write32(Guest::Base, 0xC0000000u + WatchPhysOffset + 0x100u, 0x99887766u);
        SELFTEST_CHECK(!WriteWatch::Clean(WatchPhysOffset, WatchPageSize, seqBefore),
            "WriteWatch::Clean still returned true after writing to armed page");
        SELFTEST_CHECK(Guest::Read32(Guest::Base, 0xA0000000u + WatchPhysOffset + 0x100u) == 0x99887766u &&
                       Guest::Read32(Guest::Base, 0xE0000000u + WatchPhysOffset + 0x100u) == 0x99887766u,
            "Write to armed page did not propagate across physical aliases");
        printf("  [PASS] 4. WriteWatch armed %u KB page, decoded ESR_EL1 WnR write fault, marked dirty & disarmed\n",
            WatchPageSize >> 10);

        // 5. Verify kernel_memory.cpp SmallPage (16 KB on macOS), PROT_NONE freed-memory guard, and MmQueryAddressProtect.
        PPCContext ctx{};
        constexpr uint32_t ScratchParams = 0x00002000u;
        Guest::Write32(Guest::Base, ScratchParams + 0, 0u);       // baseAddress = 0 (let allocator choose)
        Guest::Write32(Guest::Base, ScratchParams + 4, 4096u);    // requestedSize = 4 KB
        ctx.r3.u32 = ScratchParams + 0;
        ctx.r4.u32 = ScratchParams + 4;
        ctx.r5.u32 = 0x00001000u | 0x00002000u;                   // X_MEM_COMMIT | X_MEM_RESERVE
        __imp__NtAllocateVirtualMemory(ctx, Guest::Base);
        SELFTEST_CHECK(ctx.r3.u32 == 0, "NtAllocateVirtualMemory failed with status 0x%08X", ctx.r3.u32);
        const uint32_t virtAddr = Guest::Read32(Guest::Base, ScratchParams + 0);
        const uint32_t virtSize = Guest::Read32(Guest::Base, ScratchParams + 4);
#ifdef __APPLE__
        SELFTEST_CHECK(virtSize == (16u << 10) && (virtAddr & ((16u << 10) - 1)) == 0,
            "Expected 16 KB SmallPage allocation on macOS (got addr=0x%08X size=%u)", virtAddr, virtSize);
#endif

        // Reserve-only (X_MEM_RESERVE without X_MEM_COMMIT) followed by NtFreeVirtualMemory(X_MEM_DECOMMIT):
        // verifies that zeroing an untouched reserved allocation while holding g_allocatorMutex
        // demand-commits lock-free without deadlocking in Kernel::FreedMemoryTouched.
        ctx = PPCContext{};
        Guest::Write32(Guest::Base, ScratchParams + 0, 0u);
        Guest::Write32(Guest::Base, ScratchParams + 4, 64u << 10);
        ctx.r3.u32 = ScratchParams + 0;
        ctx.r4.u32 = ScratchParams + 4;
        ctx.r5.u32 = 0x00002000u;                                  // X_MEM_RESERVE only
        __imp__NtAllocateVirtualMemory(ctx, Guest::Base);
        SELFTEST_CHECK(ctx.r3.u32 == 0, "NtAllocateVirtualMemory(X_MEM_RESERVE) failed with status 0x%08X", ctx.r3.u32);
        const uint32_t reservedOnlyAddr = Guest::Read32(Guest::Base, ScratchParams + 0);

        ctx = PPCContext{};
        ctx.r3.u32 = ScratchParams + 0;
        ctx.r4.u32 = ScratchParams + 4;
        ctx.r5.u32 = 0x00004000u;                                  // X_MEM_DECOMMIT
        __imp__NtFreeVirtualMemory(ctx, Guest::Base);
        SELFTEST_CHECK(ctx.r3.u32 == 0 && Guest::Read32(Guest::Base, reservedOnlyAddr) == 0,
            "NtFreeVirtualMemory(X_MEM_DECOMMIT) on reserved-only region failed");

        // Allocate physical memory via MmAllocatePhysicalMemoryEx, free it via MmFreePhysicalMemory,
        // verify PROT_NONE guard and MmQueryAddressProtect == 0, then verify FreedMemoryTouched fault recovery.
        ctx = PPCContext{};
        ctx.r4.u32 = 64u << 10;
        ctx.r8.u32 = 64u << 10;
        __imp__MmAllocatePhysicalMemoryEx(ctx, Guest::Base);
        const uint32_t physAddr = ctx.r3.u32;
        SELFTEST_CHECK(physAddr >= Guest::PhysicalHeapBase && physAddr < Guest::PhysicalHeapLimit,
            "MmAllocatePhysicalMemoryEx returned invalid address 0x%08X", physAddr);
        Guest::Write32(Guest::Base, physAddr + 0x40u, 0x42424242u);

        ctx = PPCContext{};
        ctx.r3.u32 = physAddr;
        __imp__MmQueryAddressProtect(ctx, Guest::Base);
        SELFTEST_CHECK(ctx.r3.u32 == PAGE_READWRITE,
            "MmQueryAddressProtect on live physical allocation returned 0x%X", ctx.r3.u32);

        ctx = PPCContext{};
        ctx.r3.u32 = 0;
        ctx.r4.u32 = physAddr;
        __imp__MmFreePhysicalMemory(ctx, Guest::Base);

        SELFTEST_CHECK(VirtualQuery(Guest::Ptr(physAddr), &mbi, sizeof(mbi)) == sizeof(mbi),
            "VirtualQuery on freed physical block failed");
        SELFTEST_CHECK(mbi.Protect == PAGE_NOACCESS,
            "Freed physical block was not protected with PAGE_NOACCESS (Protect=0x%X)", unsigned(mbi.Protect));

        ctx = PPCContext{};
        ctx.r3.u32 = physAddr;
        __imp__MmQueryAddressProtect(ctx, Guest::Base);
        SELFTEST_CHECK(ctx.r3.u32 == 0,
            "MmQueryAddressProtect on freed physical block returned 0x%X (expected 0)", ctx.r3.u32);

        // Touching the freed physical block must fault, trigger Kernel::FreedMemoryTouched,
        // restore PAGE_READWRITE on that page, and return the preserved value.
        const uint32_t freedVal = Guest::Read32(Guest::Base, physAddr + 0x40u);
        SELFTEST_CHECK(freedVal == 0x42424242u,
            "FreedMemoryTouched fault recovery read 0x%08X, expected 0x42424242", freedVal);
        Kernel::FlushFaultDiagnostics();
        printf("  [PASS] 5. SmallPage (%u KB), PROT_NONE freed-memory guard & FreedMemoryTouched recovery verified\n",
            virtSize >> 10);

        Guest::Shutdown();
        printf("=== Gate 3 Self-Test: ALL VM ASSERTIONS PASSED ===\n");
        fflush(stdout);
        return 0;
    }

#if defined(__aarch64__)
    extern "C" __attribute__((noinline, used))
    void SelfTestDoLongJump(PPCContext* ctx, uint8_t* base, jmp_buf* buf, int value)
    {
        Mmio::LongJump(*ctx, base, *buf, value);
    }

    // Saves caller x19-x30 and d8-d15, loads unique per-coroutine/per-iteration
    // bit patterns into x19-x28 and d8-d15, yields via Mmio::LongJump, and upon
    // Coroutines::Resume verifies that x19-x30 and d8-d15 are 100% intact.
    // Returns 0 on success, or non-zero on any register mismatch.
    __attribute__((naked, noinline))
    uint64_t SelfTestYieldCheckRegs(PPCContext* /*ctx*/, uint8_t* /*base*/, jmp_buf* /*buf*/, uint64_t /*seed*/)
    {
        __asm__ volatile(
            "sub sp, sp, #0xB0\n"
            "stp x19, x20, [sp, #0x00]\n"
            "stp x21, x22, [sp, #0x10]\n"
            "stp x23, x24, [sp, #0x20]\n"
            "stp x25, x26, [sp, #0x30]\n"
            "stp x27, x28, [sp, #0x40]\n"
            "stp x29, x30, [sp, #0x50]\n"
            "str x3,       [sp, #0x60]\n"
            "stp d8,  d9,  [sp, #0x70]\n"
            "stp d10, d11, [sp, #0x80]\n"
            "stp d12, d13, [sp, #0x90]\n"
            "stp d14, d15, [sp, #0xA0]\n"
            "add x29, sp, #0x50\n"

            "add x19, x3, #19\n"
            "add x20, x3, #20\n"
            "add x21, x3, #21\n"
            "add x22, x3, #22\n"
            "add x23, x3, #23\n"
            "add x24, x3, #24\n"
            "add x25, x3, #25\n"
            "add x26, x3, #26\n"
            "add x27, x3, #27\n"
            "add x28, x3, #28\n"
            "fmov d8,  x19\n"
            "fmov d9,  x20\n"
            "fmov d10, x21\n"
            "fmov d11, x22\n"
            "fmov d12, x23\n"
            "fmov d13, x24\n"
            "fmov d14, x25\n"
            "fmov d15, x26\n"

            "mov w3, #1\n"
            "bl _SelfTestDoLongJump\n"

            "ldr x3, [sp, #0x60]\n"
            "mov x0, #0\n"

            "sub x9, x19, #19\n"
            "eor x9, x9, x3\n"
            "orr x0, x0, x9\n"

            "sub x9, x20, #20\n"
            "eor x9, x9, x3\n"
            "orr x0, x0, x9\n"

            "sub x9, x21, #21\n"
            "eor x9, x9, x3\n"
            "orr x0, x0, x9\n"

            "sub x9, x22, #22\n"
            "eor x9, x9, x3\n"
            "orr x0, x0, x9\n"

            "sub x9, x23, #23\n"
            "eor x9, x9, x3\n"
            "orr x0, x0, x9\n"

            "sub x9, x24, #24\n"
            "eor x9, x9, x3\n"
            "orr x0, x0, x9\n"

            "sub x9, x25, #25\n"
            "eor x9, x9, x3\n"
            "orr x0, x0, x9\n"

            "sub x9, x26, #26\n"
            "eor x9, x9, x3\n"
            "orr x0, x0, x9\n"

            "sub x9, x27, #27\n"
            "eor x9, x9, x3\n"
            "orr x0, x0, x9\n"

            "sub x9, x28, #28\n"
            "eor x9, x9, x3\n"
            "orr x0, x0, x9\n"

            "add x9, sp, #0x50\n"
            "eor x9, x9, x29\n"
            "orr x0, x0, x9\n"

            "fmov x9, d8\n"
            "eor x9, x9, x19\n"
            "orr x0, x0, x9\n"

            "fmov x9, d9\n"
            "eor x9, x9, x20\n"
            "orr x0, x0, x9\n"

            "fmov x9, d10\n"
            "eor x9, x9, x21\n"
            "orr x0, x0, x9\n"

            "fmov x9, d11\n"
            "eor x9, x9, x22\n"
            "orr x0, x0, x9\n"

            "fmov x9, d12\n"
            "eor x9, x9, x23\n"
            "orr x0, x0, x9\n"

            "fmov x9, d13\n"
            "eor x9, x9, x24\n"
            "orr x0, x0, x9\n"

            "fmov x9, d14\n"
            "eor x9, x9, x25\n"
            "orr x0, x0, x9\n"

            "fmov x9, d15\n"
            "eor x9, x9, x26\n"
            "orr x0, x0, x9\n"

            "ldp d8,  d9,  [sp, #0x70]\n"
            "ldp d10, d11, [sp, #0x80]\n"
            "ldp d12, d13, [sp, #0x90]\n"
            "ldp d14, d15, [sp, #0xA0]\n"
            "ldp x29, x30, [sp, #0x50]\n"
            "ldp x27, x28, [sp, #0x40]\n"
            "ldp x25, x26, [sp, #0x30]\n"
            "ldp x23, x24, [sp, #0x20]\n"
            "ldp x21, x22, [sp, #0x10]\n"
            "ldp x19, x20, [sp, #0x00]\n"
            "add sp, sp, #0xB0\n"
            "ret\n"
        );
    }
#endif

    // State for Criterion 1 (4 concurrent coroutines x 1,000 yields/resumes)
    constexpr int NumTestCoroutines = 4;
    constexpr int YieldsPerCoroutine = 250; // 4 * 250 = 1,000 yields/resumes total
    struct CoroutineTestSlot
    {
        uint32_t key = 0;
        jmp_buf jumpBuf{};
        int yieldCount = 0;
        uint64_t regMismatch = 0;
    };
    CoroutineTestSlot g_coroSlots[NumTestCoroutines];

    void CoroutineTestEntry(PPCContext& ctx, uint8_t* base) noexcept
    {
        const uint32_t idx = ctx.r3.u32;
        if (idx >= NumTestCoroutines) return;
        CoroutineTestSlot& slot = g_coroSlots[idx];

        for (int iter = 0; iter < YieldsPerCoroutine; ++iter)
        {
            slot.yieldCount++;
            const uint64_t seed = (uint64_t(idx + 1) << 48) ^
                                  (uint64_t(iter + 1) * 0x9E3779B97F4A7C15ULL);
#if defined(__aarch64__)
            const uint64_t diff = SelfTestYieldCheckRegs(&ctx, base, &slot.jumpBuf, seed);
            if (diff != 0)
            {
                slot.regMismatch = diff;
                return;
            }
#else
            Mmio::LongJump(ctx, base, slot.jumpBuf, 1);
#endif
        }
    }

    // State for Criterion 2 (1,000 Mach thread_suspend / thread_resume parking cycles)
    constexpr int ParkingCycles = 1000;
    std::atomic<uint32_t> g_workerHandle{ 0 };
    std::atomic<int>      g_workerCycle{ 0 };
    std::atomic<bool>     g_workerStarted{ false };
    std::atomic<bool>     g_workerDone{ false };

    void WorkerThreadTestEntry(PPCContext& ctx, uint8_t* base) noexcept
    {
        g_workerStarted.store(true, std::memory_order_release);
        while (g_workerHandle.load(std::memory_order_acquire) == 0)
            std::this_thread::yield();

        const uint32_t selfHandle = g_workerHandle.load(std::memory_order_acquire);
        for (int i = 1; i <= ParkingCycles; ++i)
        {
            g_workerCycle.store(i, std::memory_order_release);
            ctx.r3.u32 = selfHandle;
            ctx.r4.u32 = 0;
            __imp__NtSuspendThread(ctx, base);
        }
        g_workerDone.store(true, std::memory_order_release);
        ctx.r3.u32 = 0;
    }

    // State for Criterion 3 (3-deep recompiled guest function call chain unwinder test)
    int g_onStackCount = 0;
    uint32_t g_onStackFuncs[16] = {};
    int g_walkedCount = 0;
    uint32_t g_walkedFuncs[16] = {};

    __attribute__((noinline))
    void StackLeafTestEntry(PPCContext& /*ctx*/, uint8_t* base) noexcept
    {
        // Clear the hook pointer at 0x82A2A2A0 so sub_820A1948 exits cleanly after this call.
        Guest::Write32(base, 0x82A2A2A0u, 0);

        g_onStackCount = Sampler::FunctionsOnStack(g_onStackFuncs, 16);

        CONTEXT captured{};
        RtlCaptureContext(&captured);
        g_walkedCount = Sampler::WalkGuestStack(&captured, g_walkedFuncs, 16);
    }

    int RunRuntimeSelfTest()
    {
        printf("=== Gate 4 Self-Test: ARM64 Coroutines, Mach Threads, Filesystem & Stack Unwinder ===\n");
        fflush(stdout);

        SELFTEST_CHECK(Guest::Initialize(nullptr), "Guest::Initialize(nullptr) failed");
        SELFTEST_CHECK(Guest::Base != nullptr, "Guest::Base is null");

        alignas(0x40) PPCContext ctx{};
        ctx.r1.u32 = Guest::StackBase - 0x100;
        const uint32_t mainTp = Guest::CreateThreadPointer(GetCurrentThreadId(), 0);
        ctx.r13.u32 = mainTp;
        Kernel::SetCurrentContext(&ctx);
        Kernel::RegisterEntryThread();

        // --------------------------------------------------------------------
        // 1. 4 concurrent coroutines yield via Mmio::LongJump and resume via
        //    Coroutines::Resume 1,000 times total (250 each), verifying x19-x30
        //    and d8-d15 preservation across every switch.
        // --------------------------------------------------------------------
        for (int i = 0; i < NumTestCoroutines; ++i)
        {
            g_coroSlots[i].key = 0xC0000100u + uint32_t(i) * 4u;
            g_coroSlots[i].yieldCount = 0;
            g_coroSlots[i].regMismatch = 0;

            ctx.r3.u32 = uint32_t(i);
            if (setjmp(g_coroSlots[i].jumpBuf) == 0)
            {
                Coroutines::Begin(ctx, Guest::Base, g_coroSlots[i].key, CoroutineTestEntry);
            }
            SELFTEST_CHECK(g_coroSlots[i].yieldCount == 1,
                "Coroutine %d did not yield on Begin (yieldCount=%d)", i, g_coroSlots[i].yieldCount);
            SELFTEST_CHECK(!Coroutines::Finished(g_coroSlots[i].key),
                "Coroutine %d finished prematurely after first yield", i);
        }

        // Interleave resumes across all 4 coroutines for the remaining 249 yields each,
        // plus 1 final resume each to run to completion (total = 4 * 250 = 1,000 yields & resumes).
        for (int step = 1; step <= YieldsPerCoroutine; ++step)
        {
            for (int i = 0; i < NumTestCoroutines; ++i)
            {
                ctx.r3.u32 = uint32_t(i);
                if (setjmp(g_coroSlots[i].jumpBuf) == 0)
                {
                    Coroutines::Resume(ctx, Guest::Base, g_coroSlots[i].key);
                }
                SELFTEST_CHECK(g_coroSlots[i].regMismatch == 0,
                    "Coroutine %d register corruption detected (diff=0x%llX) at step %d",
                    i, (unsigned long long)g_coroSlots[i].regMismatch, step);
            }
        }

        int totalYields = 0;
        for (int i = 0; i < NumTestCoroutines; ++i)
        {
            SELFTEST_CHECK(g_coroSlots[i].yieldCount == YieldsPerCoroutine,
                "Coroutine %d yieldCount=%d, expected %d",
                i, g_coroSlots[i].yieldCount, YieldsPerCoroutine);
            SELFTEST_CHECK(Coroutines::Finished(g_coroSlots[i].key),
                "Coroutine %d not marked finished after %d yields", i, YieldsPerCoroutine);
            totalYields += g_coroSlots[i].yieldCount;
            Coroutines::Destroy(g_coroSlots[i].key);
        }
        printf("  [PASS] 1. %d concurrent coroutines x %d yields = %d Mmio::LongJump/Coroutines::Resume switches (x19-x30 & d8-d15 verified intact)\n",
            NumTestCoroutines, YieldsPerCoroutine, totalYields);

        // --------------------------------------------------------------------
        // 2. Mach thread_suspend / thread_resume worker parking (1,000 cycles)
        //    + case-insensitive filesystem path resolution.
        // --------------------------------------------------------------------
        constexpr uint32_t WorkerGuestEntryAddr = 0x820A0090u;
        auto** workerSlot = reinterpret_cast<PPCFunc**>(
            Guest::Base + Guest::FuncTableBase + (uint64_t(WorkerGuestEntryAddr - PPC_CODE_BASE) * 2));
        PPCFunc* const savedWorkerSlot = *workerSlot;
        *workerSlot = WorkerThreadTestEntry;

        g_workerHandle.store(0, std::memory_order_relaxed);
        g_workerCycle.store(0, std::memory_order_relaxed);
        g_workerStarted.store(false, std::memory_order_relaxed);
        g_workerDone.store(false, std::memory_order_relaxed);

        constexpr uint32_t ScratchHandleOut = 0x00103000u;
        constexpr uint32_t ScratchTidOut    = 0x00103004u;
        Guest::Write32(Guest::Base, ScratchHandleOut, 0);
        Guest::Write32(Guest::Base, ScratchTidOut, 0);

        // Create worker with CREATE_SUSPENDED (0x1) first to verify initial suspend state.
        ctx.r3.u32 = ScratchHandleOut;
        ctx.r4.u32 = 256u << 10;
        ctx.r5.u32 = ScratchTidOut;
        ctx.r6.u32 = 0;
        ctx.r7.u32 = WorkerGuestEntryAddr;
        ctx.r8.u32 = 0;
        ctx.r9.u32 = 0x00000001u; // CreateSuspended
        __imp__ExCreateThread(ctx, Guest::Base);
        SELFTEST_CHECK(ctx.r3.u32 == 0, "ExCreateThread failed with 0x%08X", ctx.r3.u32);

        const uint32_t workerHandle = Guest::Read32(Guest::Base, ScratchHandleOut);
        SELFTEST_CHECK(workerHandle != 0, "ExCreateThread returned null handle");
        g_workerHandle.store(workerHandle, std::memory_order_release);

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        SELFTEST_CHECK(!g_workerStarted.load(std::memory_order_acquire),
            "Thread created with CREATE_SUSPENDED ran before NtResumeThread");

        // Resume from initial CREATE_SUSPENDED.
        ctx.r3.u32 = workerHandle;
        ctx.r4.u32 = 0;
        __imp__NtResumeThread(ctx, Guest::Base);
        SELFTEST_CHECK(ctx.r3.u32 == 0, "Initial NtResumeThread failed with 0x%08X", ctx.r3.u32);

        // Drive 1,000 suspend/wake parking cycles (mixing immediate early resumes and post-park resumes).
        for (int i = 1; i <= ParkingCycles; ++i)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (g_workerCycle.load(std::memory_order_acquire) < i)
            {
                if (std::chrono::steady_clock::now() > deadline)
                    break;
                std::this_thread::yield();
            }
            SELFTEST_CHECK(g_workerCycle.load(std::memory_order_acquire) >= i,
                "Timeout waiting for worker parking cycle %d", i);

            // On odd cycles, pause briefly so the worker reaches thread_suspend;
            // on even cycles, resume immediately to stress the parking/resumeCredits race window.
            if ((i & 1) != 0 && (i <= 20))
                std::this_thread::sleep_for(std::chrono::microseconds(50));

            ctx.r3.u32 = workerHandle;
            ctx.r4.u32 = 0;
            __imp__NtResumeThread(ctx, Guest::Base);
            SELFTEST_CHECK(ctx.r3.u32 == 0, "NtResumeThread failed on cycle %d", i);
        }

        {
            const uint32_t threadObj = Kernel::ThreadObjectFor(workerHandle);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!g_workerDone.load(std::memory_order_acquire) ||
                   (threadObj != 0 && Guest::Read32(Guest::Base, threadObj + 4) == 0))
            {
                if (std::chrono::steady_clock::now() > deadline)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        SELFTEST_CHECK(g_workerDone.load(std::memory_order_acquire),
            "Worker thread did not finish after %d suspend/resume cycles", ParkingCycles);
        *workerSlot = savedWorkerSlot;

        // Verify case-insensitive multi-component guest path resolution.
        {
            const std::filesystem::path tempRoot =
                std::filesystem::temp_directory_path() / "cod3_gate4_fs_test";
            std::error_code ec;
            std::filesystem::remove_all(tempRoot, ec);
            const std::filesystem::path subDir = tempRoot / "sound_assets" / "voice_en";
            std::filesystem::create_directories(subDir, ec);
            const std::filesystem::path targetFile = subDir / "dialogue_01.xma";
            {
                std::ofstream out(targetFile, std::ios::binary);
                out << "cod3_test";
            }
            const std::filesystem::path prevGameRoot = Guest::GameRoot;
            Guest::GameRoot = tempRoot;
            const std::filesystem::path resolved =
                Kernel::ResolveGuestPath("game:\\SoUnD_AsSeTs\\VoIcE_En\\DiAlOgUe_01.XmA");
            const bool sameFile = !resolved.empty() && std::filesystem::equivalent(resolved, targetFile, ec) && !ec;
            Guest::GameRoot = prevGameRoot;
            std::filesystem::remove_all(tempRoot, ec);
            SELFTEST_CHECK(sameFile,
                "Case-insensitive ResolveGuestPath failed (resolved='%s')", resolved.string().c_str());
        }
        printf("  [PASS] 2. Mach thread_suspend/thread_resume passed %d parking cycles + case-insensitive ResolveGuestPath verified\n",
            ParkingCycles);

        // --------------------------------------------------------------------
        // 3. Sampler::WalkGuestStack and Sampler::FunctionsOnStack unwind a
        //    3-deep recompiled guest function call chain:
        //    sub_820A01F0 -> sub_820A1560 -> sub_820A1948
        // --------------------------------------------------------------------
        *workerSlot = StackLeafTestEntry;

        // Set up guest memory objects for:
        //   sub_820A01F0 (r3=obj1, r4=4, r5=1, r6=0x820A1560)
        //   -> sub_820A1560 (reads obj2=*(obj1+12), vtable2=*(obj2+0), target=*(vtable2+8)=0x820A1948)
        //   -> sub_820A1948 (reads hookObj=*(0x82A2A2A0), leaf=*(hookObj+4316)=WorkerGuestEntryAddr)
        constexpr uint32_t Obj1Addr    = 0x00104000u;
        constexpr uint32_t Obj2Addr    = 0x00104100u;
        constexpr uint32_t Vtable2Addr = 0x00104200u;
        constexpr uint32_t HookObjAddr = 0x00105000u;
        memset(Guest::Ptr(Obj1Addr), 0, 0x3000);

        Guest::Write32(Guest::Base, Obj1Addr + 0,  0);
        Guest::Write32(Guest::Base, Obj1Addr + 12, Obj2Addr);
        Guest::Write32(Guest::Base, Obj1Addr + 16, 0);

        Guest::Write32(Guest::Base, Obj2Addr + 0,  Vtable2Addr);
        Guest::Write32(Guest::Base, Obj2Addr + 24, 0);
        Guest::Write32(Guest::Base, Obj2Addr + 56, 0);

        Guest::Write32(Guest::Base, Vtable2Addr + 8, 0x820A1948u);

        Guest::Write32(Guest::Base, 0x82A2A2A0u, HookObjAddr);
        Guest::Write32(Guest::Base, HookObjAddr + 4316u, WorkerGuestEntryAddr);

        g_onStackCount = 0;
        g_walkedCount = 0;
        memset(g_onStackFuncs, 0, sizeof(g_onStackFuncs));
        memset(g_walkedFuncs, 0, sizeof(g_walkedFuncs));

        PPCFunc* const outerGuestFn = Guest::Lookup(0x820A01F0u);
        SELFTEST_CHECK(outerGuestFn != nullptr, "Guest::Lookup(0x820A01F0) returned null");

        ctx = PPCContext{};
        ctx.r1.u32  = Guest::StackBase - 0x1000;
        ctx.r13.u32 = mainTp;
        ctx.r3.u32  = Obj1Addr;
        ctx.r4.u32  = 4;
        ctx.r5.u32  = 1;
        ctx.r6.u32  = 0x820A1560u;
        outerGuestFn(ctx, Guest::Base);

        *workerSlot = savedWorkerSlot;

        SELFTEST_CHECK(g_onStackCount >= 3,
            "Sampler::FunctionsOnStack returned %d frames, expected >= 3", g_onStackCount);
        SELFTEST_CHECK(g_onStackFuncs[0] == 0x820A1948u &&
                       g_onStackFuncs[1] == 0x820A1560u &&
                       g_onStackFuncs[2] == 0x820A01F0u,
            "Sampler::FunctionsOnStack returned [0x%08X, 0x%08X, 0x%08X], expected [0x820A1948, 0x820A1560, 0x820A01F0]",
            g_onStackFuncs[0], g_onStackFuncs[1], g_onStackFuncs[2]);

        SELFTEST_CHECK(g_walkedCount >= 3,
            "Sampler::WalkGuestStack returned %d frames, expected >= 3", g_walkedCount);
        SELFTEST_CHECK(g_walkedFuncs[0] == 0x820A1948u &&
                       g_walkedFuncs[1] == 0x820A1560u &&
                       g_walkedFuncs[2] == 0x820A01F0u,
            "Sampler::WalkGuestStack returned [0x%08X, 0x%08X, 0x%08X], expected [0x820A1948, 0x820A1560, 0x820A01F0]",
            g_walkedFuncs[0], g_walkedFuncs[1], g_walkedFuncs[2]);

        printf("  [PASS] 3. Sampler::FunctionsOnStack & Sampler::WalkGuestStack unwound 3-deep guest chain: sub_%08X <- sub_%08X <- sub_%08X\n",
            g_walkedFuncs[0], g_walkedFuncs[1], g_walkedFuncs[2]);

        Kernel::SetCurrentContext(nullptr);
        Guest::ReleaseThreadPointer(mainTp);
        Guest::Shutdown();
        printf("=== Gate 4 Self-Test: ALL RUNTIME ASSERTIONS PASSED ===\n");
        fflush(stdout);
        return 0;
    }
}

namespace SelfTest
{
    int Run(const std::string& suite)
    {
        if (suite == "vm" || suite == "all")
        {
            const int rc = RunVmSelfTest();
            if (rc != 0 || suite == "vm")
                return rc;
        }
        if (suite == "runtime" || suite == "all")
        {
            const int rc = RunRuntimeSelfTest();
            if (rc != 0 || suite == "runtime")
                return rc;
        }
        if (suite == "render" || suite == "audio")
        {
            fprintf(stderr, "Self-test suite '%s' is not implemented yet.\n", suite.c_str());
            return 1;
        }
        if (suite != "vm" && suite != "runtime" && suite != "all")
        {
            fprintf(stderr, "Unknown self-test suite '%s' (expected vm, runtime, render, audio, or all).\n",
                suite.c_str());
            return 1;
        }
        return 0;
    }
}
