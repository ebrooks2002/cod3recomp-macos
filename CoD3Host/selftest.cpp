#include "selftest.h"
#include "coroutines.h"
#include "gpu.h"
#include "kernel.h"
#include "render.h"
#include "render_pipeline.h"
#include "render_resources.h"
#include "render_shaders.h"
#include "render_state.h"
#include "sampler.h"
#include "write_watch.h"
#include "xenos_hlsl.h"
#include "platform.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cmath>
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

        // A sampler's view of the threads, taken while the worker is alive and
        // kept until after it exits: its handle must stay bound to the worker.
        std::vector<Kernel::ThreadSample> earlySamples = Kernel::SampleGuestThreads();
        HANDLE earlyWorker = nullptr;
        const uint32_t workerId = Guest::Read32(Guest::Base, ScratchTidOut);   // the guest thread id
        for (const auto& sample : earlySamples)
            if (sample.handle != nullptr && sample.id == workerId) earlyWorker = sample.handle;
        SELFTEST_CHECK(earlyWorker != nullptr, "SampleGuestThreads gave no handle for the live worker");

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

        // The worker has exited: the table no longer hands out a handle for
        // it, and the sample taken earlier still names the dead worker (so
        // suspending through it fails) rather than whatever thread might have
        // been given a recycled port name.
        {
            std::vector<Kernel::ThreadSample> lateSamples = Kernel::SampleGuestThreads();
            bool lateHandle = false;
            for (const auto& sample : lateSamples)
                if (sample.id == workerId && sample.handle != nullptr) lateHandle = true;
            Kernel::ReleaseThreadSamples(lateSamples);
            SELFTEST_CHECK(!lateHandle, "SampleGuestThreads still handed out a handle for the exited worker");

            bool stale = false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (SuspendThread(earlyWorker) == DWORD(-1)) { stale = true; break; }
                ResumeThread(earlyWorker);   // still unwinding its exit; let it finish
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Kernel::ReleaseThreadSamples(earlySamples);
            SELFTEST_CHECK(stale, "A sampled handle to the exited worker still suspends a live thread");
        }

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
        printf("  [PASS] 2. Mach thread_suspend/thread_resume passed %d parking cycles, sampled thread handle outlived its thread safely + case-insensitive ResolveGuestPath verified\n",
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

        // --------------------------------------------------------------------
        // 4. Hardware watchpoint: fires on a write to the watched word, is
        //    re-armed so a second write fires too, and selects only the
        //    watched bytes and access (not the neighbouring word, not reads).
        // --------------------------------------------------------------------
        SELFTEST_CHECK(Kernel::SelfTestWatchpoint(),
            "Kernel::SelfTestWatchpoint failed (see the watchpoint line above)");
        printf("  [PASS] 4. Hardware watchpoint fired on both writes, re-armed after each hit, ignored neighbour word and reads\n");

        Kernel::SetCurrentContext(nullptr);
        Guest::ReleaseThreadPointer(mainTp);
        Guest::Shutdown();
        printf("=== Gate 4 Self-Test: ALL RUNTIME ASSERTIONS PASSED ===\n");
        fflush(stdout);
        return 0;
    }

    int RunRenderSelfTest()
    {
        printf("=== Gate 5 Self-Test: Xenos->HLSL Translation, D3D11 Backend, Render Caches & Resolve ===\n");
        fflush(stdout);

        using Microsoft::WRL::ComPtr;

        // Isolate shader disk cache and captured shader microcode in a temporary directory for this self-test.
        const std::filesystem::path tempCacheRoot =
            std::filesystem::temp_directory_path() / "cod3_gate5_shader_cache";
        std::error_code ec;
        const std::filesystem::path origCwd = std::filesystem::current_path(ec);
        std::filesystem::remove_all(tempCacheRoot, ec);
        std::filesystem::create_directories(tempCacheRoot, ec);
        std::filesystem::current_path(tempCacheRoot, ec);
        struct ScopedTempDirCleanup
        {
            std::filesystem::path origCwd;
            std::filesystem::path tempRoot;
            ~ScopedTempDirCleanup()
            {
                std::error_code cleanupEc;
                if (!origCwd.empty())
                    std::filesystem::current_path(origCwd, cleanupEc);
                std::filesystem::remove_all(tempRoot, cleanupEc);
            }
        } tempDirGuard{ origCwd, tempCacheRoot };
#ifndef _WIN32
        setenv("LOCALAPPDATA", tempCacheRoot.string().c_str(), 1);
        setenv("COD3_NOPRECOMPILE", "1", 1);
        setenv("COD3_SCALE", "1", 1);
#else
        _putenv_s("LOCALAPPDATA", tempCacheRoot.string().c_str());
        _putenv_s("COD3_NOPRECOMPILE", "1");
        _putenv_s("COD3_SCALE", "1");
#endif

        // --------------------------------------------------------------------
        // 1. Construct real Xenos vertex and pixel microcode programs,
        //    translate via XenosHlsl::Translate, and compile via D3DCompile.
        // --------------------------------------------------------------------
        // Vertex microcode (4 slots = 12 dwords):
        //   Slot 0 (CF): exece address=1, count=3, sequence=0b000001 (slot 1 is vfetch, slots 2-3 are ALU)
        //   Slot 1 (vfetch): r[1].xyzw = vb0.Load4(r[0].x * 4), FMT_32_32_32_32_FLOAT (38), stride=4 dwords
        //   Slot 2 (ALU):    oPos.xyzw = r[1].xyzw + c[0].xyzw
        //   Slot 3 (ALU):    o0.xyzw   = c[1].xyzw + c[0].xyzw
        const uint64_t vsCf0 = (uint64_t(2) << 44) | (uint64_t(1) << 16) | (uint64_t(3) << 12) | 1ull;
        const std::vector<uint32_t> vsWords = {
            uint32_t(vsCf0 & 0xFFFFFFFFull),
            uint32_t((vsCf0 >> 32) & 0xFFFFull),
            0u,
            // Slot 1: Vertex fetch from slot 0 into r[1].xyzw, format 38 (float4), stride 4
            (1u << 12),
            0x688u | (38u << 16),
            4u,
            // Slot 2: ALU add -> oPos (dest=62, exported=1, mask=0xF), r[1] + c[0]
            62u | (1u << 15) | (0xFu << 16),
            0u,
            (1u << 16) | (0u << 8) | (1u << 31),
            // Slot 3: ALU add -> o0 (dest=0, exported=1, mask=0xF), c[1] + c[0]
            0u | (1u << 15) | (0xFu << 16),
            0u,
            (1u << 16) | (0u << 8)
        };

        // Pixel microcode (2 slots = 6 dwords):
        //   Slot 0 (CF): exece address=1, count=1, sequence=0 (slot 1 is ALU)
        //   Slot 1 (ALU): oC0.xyzw = r[0].xyzw + c[0].xyzw
        const uint64_t psCf0 = (uint64_t(2) << 44) | (uint64_t(0) << 16) | (uint64_t(1) << 12) | 1ull;
        const std::vector<uint32_t> psWords = {
            uint32_t(psCf0 & 0xFFFFFFFFull),
            uint32_t((psCf0 >> 32) & 0xFFFFull),
            0u,
            // Slot 1: ALU add -> oC0 (dest=0, exported=1, mask=0xF), r[0] + c[0]
            0u | (1u << 15) | (0xFu << 16),
            0u,
            (0u << 16) | (0u << 8) | (1u << 31)
        };

        const XenosHlsl::Translation vsTrans = XenosHlsl::Translate(vsWords, false);
        SELFTEST_CHECK(vsTrans.ok, "Vertex microcode translation failed: %s", vsTrans.problem.c_str());
        SELFTEST_CHECK(vsTrans.vertexFetches.size() == 1 && vsTrans.vertexFetches[0].slot == 0 && vsTrans.vertexFetches[0].stride == 4,
            "Unexpected vertexFetches metadata (size=%zu)", vsTrans.vertexFetches.size());
        SELFTEST_CHECK(vsTrans.constantMap.size() == 2 && vsTrans.constantMap[0] == 0 && vsTrans.constantMap[1] == 1,
            "Unexpected vertex constantMap size=%zu", vsTrans.constantMap.size());

        const XenosHlsl::Translation psTrans = XenosHlsl::Translate(psWords, true);
        SELFTEST_CHECK(psTrans.ok, "Pixel microcode translation failed: %s", psTrans.problem.c_str());
        SELFTEST_CHECK(psTrans.colourTargets == 1 && !psTrans.writesDepth,
            "Unexpected pixel translation targets (colourTargets=%u writesDepth=%d)",
            psTrans.colourTargets, psTrans.writesDepth ? 1 : 0);
        SELFTEST_CHECK(psTrans.constantMap.size() == 1 && psTrans.constantMap[0] == 0,
            "Unexpected pixel constantMap size=%zu", psTrans.constantMap.size());

        ComPtr<ID3DBlob> vsBlob, vsErr, psBlob, psErr;
        HRESULT hr = D3DCompile(vsTrans.hlsl.data(), vsTrans.hlsl.size(), "selftest_vs",
            nullptr, nullptr, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &vsErr);
        SELFTEST_CHECK(SUCCEEDED(hr) && vsBlob && vsBlob->GetBufferSize() > 0,
            "D3DCompile(vs_5_0) failed (hr=0x%08X): %s", unsigned(hr),
            vsErr ? static_cast<const char*>(vsErr->GetBufferPointer()) : "no error blob");

        hr = D3DCompile(psTrans.hlsl.data(), psTrans.hlsl.size(), "selftest_ps",
            nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &psErr);
        SELFTEST_CHECK(SUCCEEDED(hr) && psBlob && psBlob->GetBufferSize() > 0,
            "D3DCompile(ps_5_0) failed (hr=0x%08X): %s", unsigned(hr),
            psErr ? static_cast<const char*>(psErr->GetBufferPointer()) : "no error blob");

        printf("  [PASS] 1. XenosHlsl::Translate & D3DCompile succeeded (VS=%zu bytes bytecode, PS=%zu bytes bytecode)\n",
            vsBlob->GetBufferSize(), psBlob->GetBufferSize());

        // --------------------------------------------------------------------
        // 2. Initialize Guest VM + Render backend, load shaders through
        //    Render::ShaderLoaded, wait for worker compilation, and verify
        //    the on-disk shader cache round-trip.
        // --------------------------------------------------------------------
        SELFTEST_CHECK(Guest::Initialize(nullptr), "Guest::Initialize(nullptr) failed");
        SELFTEST_CHECK(Guest::Base != nullptr, "Guest::Base is null");
        SELFTEST_CHECK(Render::Enabled(), "Render::Enabled() failed to initialize D3D11 device");

        constexpr uint32_t VsGuestAddr = 0xA0110000u;
        constexpr uint32_t PsGuestAddr = 0xA0111000u;
        for (size_t i = 0; i < vsWords.size(); ++i)
            Guest::Write32(Guest::Base, VsGuestAddr + uint32_t(i * 4), vsWords[i]);
        for (size_t i = 0; i < psWords.size(); ++i)
            Guest::Write32(Guest::Base, PsGuestAddr + uint32_t(i * 4), psWords[i]);

        Render::ShaderLoaded(false, VsGuestAddr, uint32_t(vsWords.size()));
        Render::ShaderLoaded(true,  PsGuestAddr, uint32_t(psWords.size()));

        const RenderState::Handle vsHandle = RenderShaders::Current(false);
        const RenderState::Handle psHandle = RenderShaders::Current(true);
        SELFTEST_CHECK(vsHandle != 0 && psHandle != 0,
            "RenderShaders::Current returned invalid handle (vs=%u ps=%u)", vsHandle, psHandle);

        const RenderShaders::Program* vsProg = RenderShaders::Get(vsHandle);
        const RenderShaders::Program* psProg = RenderShaders::Get(psHandle);
        SELFTEST_CHECK(vsProg != nullptr && psProg != nullptr, "RenderShaders::Get returned null");

        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline)
            {
                const auto vsState = vsProg->state.load(std::memory_order_acquire);
                const auto psState = psProg->state.load(std::memory_order_acquire);
                if ((vsState == RenderShaders::State::Ready || vsState == RenderShaders::State::Failed) &&
                    (psState == RenderShaders::State::Ready || psState == RenderShaders::State::Failed))
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        SELFTEST_CHECK(vsProg->state.load(std::memory_order_acquire) == RenderShaders::State::Ready,
            "Vertex shader failed to reach Ready state (problem='%s')", vsProg->problem.c_str());
        SELFTEST_CHECK(psProg->state.load(std::memory_order_acquire) == RenderShaders::State::Ready,
            "Pixel shader failed to reach Ready state (problem='%s')", psProg->problem.c_str());
        SELFTEST_CHECK(vsProg->shader != nullptr && psProg->shader != nullptr,
            "Compiled shader device objects are null");

        // Verify disk cache files were written with 'C3SH' header and matching bytecode size.
        const std::filesystem::path shaderCacheDir = tempCacheRoot / "CoD3Recomp" / "shaders";
        uint32_t cachedFilesFound = 0;
        for (const auto& entry : std::filesystem::directory_iterator(shaderCacheDir, ec))
        {
            if (!entry.is_regular_file()) continue;
            std::ifstream in(entry.path(), std::ios::binary);
            uint32_t magic = 0, version = 0;
            in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
            in.read(reinterpret_cast<char*>(&version), sizeof(version));
            SELFTEST_CHECK(magic == 0x48533343u && version == 3u,
                "Invalid shader cache header in %s (magic=0x%08X version=%u)",
                entry.path().string().c_str(), magic, version);
            cachedFilesFound++;
        }
        SELFTEST_CHECK(cachedFilesFound == 2,
            "Expected 2 shader cache files in %s, found %u",
            shaderCacheDir.string().c_str(), cachedFilesFound);

        printf("  [PASS] 2. RenderShaders worker compiled VS (%016llx) & PS (%016llx) and wrote %u disk cache entries\n",
            (unsigned long long)vsProg->hash, (unsigned long long)psProg->hash, cachedFilesFound);

        // --------------------------------------------------------------------
        // 3. Execute a synthetic draw command (DI_PT_RECTLIST rectangle list
        //    expansion + vertex buffer upload + constant ring + pipeline state)
        //    followed by Render::Resolve (1x and 4x MSAA) and read back pixels.
        // --------------------------------------------------------------------
        constexpr uint32_t VbPhysAddr      = 0x00200000u;
        constexpr uint32_t ResolvePhys1x   = 0x00300000u;
        constexpr uint32_t ResolvePhysMsaa = 0x00380000u;

        auto writeGuestFloat = [](uint32_t physAddr, float v) {
            uint32_t bits = 0;
            memcpy(&bits, &v, sizeof(bits));
            Guest::Write32(Guest::Base, Guest::PhysicalAlias(physAddr), bits);
        };

        // 3 vertices for a 64x64 screen-space rectangle list (4th vertex is synthesized by rect_main):
        //   v0 = (0,  0,  0.5, 1)
        //   v1 = (64, 0,  0.5, 1)
        //   v2 = (0,  64, 0.5, 1)
        const float rectVerts[3][4] = {
            {  0.0f,  0.0f, 0.5f, 1.0f },
            { 64.0f,  0.0f, 0.5f, 1.0f },
            {  0.0f, 64.0f, 0.5f, 1.0f },
        };
        for (uint32_t v = 0; v < 3; ++v)
            for (uint32_t c = 0; c < 4; ++c)
                writeGuestFloat(VbPhysAddr + (v * 4 + c) * 4, rectVerts[v][c]);

        auto writeReg = [](uint32_t regIndex, uint32_t value) {
            Gpu::WriteRegister(Gpu::ApertureBase + regIndex * 4u, value);
        };
        auto writeRegFloat = [&](uint32_t regIndex, float value) {
            uint32_t bits = 0;
            memcpy(&bits, &value, sizeof(bits));
            writeReg(regIndex, bits);
        };

        // Configure Xenos registers for a 64x64 surface draw.
        writeReg(RenderState::SurfaceInfo,        64u);                      // pitch = 64, 1x MSAA
        writeReg(RenderState::ColorInfo0,         0u);                       // tile 0, format 0 (RGBA8)
        writeReg(RenderState::DepthInfo,          0u);
        writeReg(RenderState::WindowOffset,       0u);
        writeReg(RenderState::ScissorTopLeft,     0u);
        writeReg(RenderState::ScissorBottomRight, (64u << 16) | 64u);
        writeReg(RenderState::ColorMask,          0x0000000Fu);              // write RGBA on RT0
        writeReg(RenderState::ModeControl,        4u);                       // color + depth mode
        writeReg(RenderState::DepthControl,       (7u << 4) | (1u << 2) | (1u << 1)); // Z enable, write, ALWAYS
        writeReg(RenderState::BlendControl0,      0x00010001u);              // opaque (ONE, ZERO)
        writeReg(RenderState::BlendControl1,      0x00010001u);
        writeReg(RenderState::BlendControl2,      0x00010001u);
        writeReg(RenderState::BlendControl3,      0x00010001u);
        writeReg(RenderState::SuScModeControl,    0u);                       // cull none
        writeReg(RenderState::VteControl,         0x100u);                   // pre-scaled screen coords (ww = 1.0)
        writeReg(RenderState::VertexControl,      1u);                       // exact pixel centers
        writeReg(RenderState::ColorControl,       0u);                       // alpha test off
        writeReg(RenderState::IndexOffset,        0u);
        writeReg(RenderState::MinVertexIndex,     0u);
        writeReg(RenderState::MaxVertexIndex,     2u);

        // Vertex fetch constant slot 0: base = VbPhysAddr, size = 12 dwords (3 vertices * 4 dwords), endian = 2
        writeReg(RenderState::FetchConstants + 0, VbPhysAddr);
        writeReg(RenderState::FetchConstants + 1, (12u << 2) | 2u);

        // Vertex constants: c[0] = (0, 0, 0, 0), c[1] = (0.25, 0.5, 0.75, 1.0)
        for (uint32_t i = 0; i < 4; ++i)
            writeRegFloat(RenderState::VertexConstants + i, 0.0f);
        writeRegFloat(RenderState::VertexConstants + 4 + 0, 0.25f);
        writeRegFloat(RenderState::VertexConstants + 4 + 1, 0.50f);
        writeRegFloat(RenderState::VertexConstants + 4 + 2, 0.75f);
        writeRegFloat(RenderState::VertexConstants + 4 + 3, 1.00f);

        // Pixel constants: c[0] = (0.50, 0.0, 0.0, 0.0)
        // Expected output color oC0 = c_vs[1] + c_ps[0] = (0.75, 0.50, 0.75, 1.0) -> RGBA (191, 128, 191, 255)
        writeRegFloat(RenderState::PixelConstants + 0, 0.50f);
        writeRegFloat(RenderState::PixelConstants + 1, 0.00f);
        writeRegFloat(RenderState::PixelConstants + 2, 0.00f);
        writeRegFloat(RenderState::PixelConstants + 3, 0.00f);

        // Initiator: primitive = Rectangles (8), sourceSelect = 2 (auto-index), indexCount = 3
        const uint32_t rectInitiator = (3u << 16) | (2u << 6) | uint32_t(RenderState::Rectangles);
        Render::Draw(rectInitiator, 0u, 0u);

        // Resolve the 64x64 color target to ResolvePhys1x and clear RT0 afterward.
        writeReg(RenderState::CopyControl,    (1u << 8) | (1u << 9));        // copy RT0 + clear color & depth
        writeReg(RenderState::CopyDestBase,   ResolvePhys1x);
        writeReg(RenderState::CopyDestPitch,  64u);
        writeReg(RenderState::CopyDestInfo,   0u);
        writeReg(RenderState::CopyColorClear, 0xFF000000u);
        writeReg(RenderState::CopyDepthClear, 0xFFFFFF00u);
        Render::Resolve();

        auto readbackAndVerify = [&](uint32_t resolvedPhysAddr, uint8_t expR, uint8_t expG, uint8_t expB, uint8_t expA, const char* label) -> bool {
            const RenderResources::Resolved* resolved = RenderResources::ResolvedAt(resolvedPhysAddr);
            if (resolved == nullptr || resolved->texture == nullptr)
            {
                fprintf(stderr, "  [FAIL] ResolvedAt(0x%08X) returned null for %s\n", resolvedPhysAddr, label);
                return false;
            }
            ComPtr<ID3D11Device> device;
            resolved->texture->GetDevice(&device);
            if (!device)
            {
                fprintf(stderr, "  [FAIL] GetDevice() returned null for %s\n", label);
                return false;
            }
            ComPtr<ID3D11DeviceContext> context;
            device->GetImmediateContext(&context);
            if (!context)
            {
                fprintf(stderr, "  [FAIL] GetImmediateContext() returned null for %s\n", label);
                return false;
            }

            D3D11_TEXTURE2D_DESC desc{};
            resolved->texture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;

            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)) || !staging)
            {
                fprintf(stderr, "  [FAIL] CreateTexture2D(STAGING) failed for %s\n", label);
                return false;
            }
            context->CopyResource(staging.Get(), resolved->texture);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || mapped.pData == nullptr)
            {
                fprintf(stderr, "  [FAIL] Map(STAGING) failed for %s\n", label);
                return false;
            }

            // Sample center pixel (32, 32) and corner pixel (48, 48) (which lies in the synthesized 2nd triangle of the rectangle list!)
            const uint32_t coords[2][2] = { { 32u, 32u }, { 48u, 48u } };
            bool ok = true;
            for (const auto& pt : coords)
            {
                const uint8_t* px = static_cast<const uint8_t*>(mapped.pData) + pt[1] * mapped.RowPitch + pt[0] * 4u;
                const int dr = std::abs(int(px[0]) - int(expR));
                const int dg = std::abs(int(px[1]) - int(expG));
                const int db = std::abs(int(px[2]) - int(expB));
                const int da = std::abs(int(px[3]) - int(expA));
                if (dr > 2 || dg > 2 || db > 2 || da > 2)
                {
                    fprintf(stderr, "  [FAIL] %s pixel (%u,%u) RGBA=(%u,%u,%u,%u), expected (%u,%u,%u,%u)\n",
                        label, pt[0], pt[1], px[0], px[1], px[2], px[3], expR, expG, expB, expA);
                    ok = false;
                }
            }
            context->Unmap(staging.Get(), 0);
            return ok;
        };

        SELFTEST_CHECK(readbackAndVerify(ResolvePhys1x, 191, 128, 191, 255, "1x DI_PT_RECTLIST resolve"),
            "1x rectangle-list draw + resolve pixel verification failed");
        printf("  [PASS] 3. 1x DI_PT_RECTLIST draw (synthesizing 4th corner) + Render::Resolve verified RGBA=(191,128,191,255)\n");

        // --------------------------------------------------------------------
        // 4. Test 4x MSAA render target draw + multisampled shader resolve.
        // --------------------------------------------------------------------
        RenderResources::RequestMultisample(4);
        RenderResources::BeginFrame(2);

        // SurfaceInfo with MSAA bits (1 << 16) selects the MSAA target class; update pixel constant c[0] to (0.0, 0.25, 0.0, 0.0)
        // so expected color is (0.25, 0.75, 0.75, 1.0) -> RGBA (64, 191, 191, 255).
        writeReg(RenderState::SurfaceInfo, 64u | (1u << 16));
        writeRegFloat(RenderState::PixelConstants + 0, 0.00f);
        writeRegFloat(RenderState::PixelConstants + 1, 0.25f);
        writeRegFloat(RenderState::PixelConstants + 2, 0.00f);
        writeRegFloat(RenderState::PixelConstants + 3, 0.00f);

        Render::Draw(rectInitiator, 0u, 0u);

        writeReg(RenderState::CopyDestBase, ResolvePhysMsaa);
        Render::Resolve();

        const RenderResources::ColorTarget* msaaTarget =
            RenderResources::ColorTargetOf(RenderResources::ColorTargetFor(0u, 64u | (1u << 16)));
        SELFTEST_CHECK(msaaTarget != nullptr && msaaTarget->samples >= 2,
            "Expected MSAA ColorTarget to have >= 2 samples (got %u)",
            msaaTarget ? msaaTarget->samples : 0u);
        SELFTEST_CHECK(readbackAndVerify(ResolvePhysMsaa, 64, 191, 191, 255, "4x MSAA resolve"),
            "4x MSAA draw + resolve pixel verification failed");
        printf("  [PASS] 4. %ux MSAA render target draw + multisampled Render::Resolve verified RGBA=(64,191,191,255)\n",
            msaaTarget->samples);

        const RenderPipeline::Statistics pipeStats = RenderPipeline::Stats();
        const RenderResources::Statistics resStats = RenderResources::Stats();
        SELFTEST_CHECK(pipeStats.blendStates >= 1 && pipeStats.depthStates >= 1 && pipeStats.rasterizerStates >= 1,
            "Expected pipeline state caches to be populated (blend=%u depth=%u rast=%u)",
            pipeStats.blendStates, pipeStats.depthStates, pipeStats.rasterizerStates);
        SELFTEST_CHECK(resStats.buffers >= 1 && resStats.colorTargets >= 1 && resStats.depthTargets >= 1 && resStats.resolved >= 2,
            "Expected resource caches to be populated (buffers=%u colorTargets=%u depthTargets=%u resolved=%u)",
            resStats.buffers, resStats.colorTargets, resStats.depthTargets, resStats.resolved);

        Guest::Shutdown();
        printf("=== Gate 5 Self-Test: ALL RENDER ASSERTIONS PASSED ===\n");
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
        if (suite == "render" || suite == "all")
        {
            const int rc = RunRenderSelfTest();
            if (rc != 0 || suite == "render")
                return rc;
        }
        if (suite == "audio")
        {
            fprintf(stderr, "Self-test suite '%s' is not implemented yet.\n", suite.c_str());
            return 1;
        }
        if (suite != "vm" && suite != "runtime" && suite != "render" && suite != "all")
        {
            fprintf(stderr, "Unknown self-test suite '%s' (expected vm, runtime, render, audio, or all).\n",
                suite.c_str());
            return 1;
        }
        return 0;
    }
}
