#include "selftest.h"
#include "audio_out.h"
#include "coroutines.h"
#include "gpu.h"
#include "input.h"
#include "kernel.h"
#include "overlay.h"
#include "render.h"
#include "render_pipeline.h"
#include "render_resources.h"
#include "render_shaders.h"
#include "render_state.h"
#include "sampler.h"
#include "settings.h"
#include "window.h"
#include "write_watch.h"
#include "xenos_hlsl.h"
#include "xma.h"
#include "platform.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#ifdef __APPLE__
#include <SDL2/SDL.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
extern "C" char** environ;
#endif

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

    std::vector<uint32_t> MakeSelftestVsWords()
    {
        const uint64_t vsCf0 = (uint64_t(2) << 44) | (uint64_t(1) << 16) | (uint64_t(3) << 12) | 1ull;
        return {
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
    }

    std::vector<uint32_t> MakeSelftestPsWords()
    {
        const uint64_t psCf0 = (uint64_t(2) << 44) | (uint64_t(0) << 16) | (uint64_t(1) << 12) | 1ull;
        return {
            uint32_t(psCf0 & 0xFFFFFFFFull),
            uint32_t((psCf0 >> 32) & 0xFFFFull),
            0u,
            // Slot 1: ALU add -> oC0 (dest=0, exported=1, mask=0xF), r[0] + c[0]
            0u | (1u << 15) | (0xFu << 16),
            0u,
            (0u << 16) | (0u << 8) | (1u << 31)
        };
    }

#ifdef __APPLE__
    int SpawnSelfTestChild(const char* suiteArg)
    {
        char exePath[4096];
        uint32_t size = sizeof(exePath);
        if (_NSGetExecutablePath(exePath, &size) != 0)
            return -1;
        fflush(stdout);
        fflush(stderr);
        char* argv[] = { exePath, const_cast<char*>(suiteArg), nullptr };
        pid_t pid = 0;
        if (posix_spawn(&pid, exePath, nullptr, nullptr, argv, environ) != 0)
            return -1;
        int status = 0;
        if (waitpid(pid, &status, 0) < 0)
            return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
#endif

    int RunRenderCacheSelfTest()
    {
        const char* expectedCompiledEnv = getenv("COD3_SELFTEST_CACHE_EXPECTED_COMPILED");
        const uint64_t expectedCompiled = expectedCompiledEnv ? uint64_t(strtoull(expectedCompiledEnv, nullptr, 10)) : 0ull;
        const uint64_t expectedFromDisk = (expectedCompiled <= 2) ? (2ull - expectedCompiled) : 0ull;

        SELFTEST_CHECK(Guest::Initialize(nullptr), "render-cache: Guest::Initialize(nullptr) failed");
        SELFTEST_CHECK(Guest::Base != nullptr, "render-cache: Guest::Base is null");
        SELFTEST_CHECK(Render::Enabled(), "render-cache: Render::Enabled() failed");

        const std::vector<uint32_t> vsWords = MakeSelftestVsWords();
        const std::vector<uint32_t> psWords = MakeSelftestPsWords();

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
            "render-cache: RenderShaders::Current returned invalid handle (vs=%u ps=%u)", vsHandle, psHandle);

        const RenderShaders::Program* vsProg = RenderShaders::Get(vsHandle);
        const RenderShaders::Program* psProg = RenderShaders::Get(psHandle);
        SELFTEST_CHECK(vsProg != nullptr && psProg != nullptr, "render-cache: RenderShaders::Get returned null");

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

        SELFTEST_CHECK(vsProg->state.load(std::memory_order_acquire) == RenderShaders::State::Ready,
            "render-cache: VS failed to reach Ready state");
        SELFTEST_CHECK(psProg->state.load(std::memory_order_acquire) == RenderShaders::State::Ready,
            "render-cache: PS failed to reach Ready state");
        SELFTEST_CHECK(vsProg->shader != nullptr && psProg->shader != nullptr,
            "render-cache: shader device objects are null");

        const RenderShaders::Statistics stats = RenderShaders::Stats();
        SELFTEST_CHECK(stats.fromDisk == expectedFromDisk && stats.compiled == expectedCompiled && stats.failed == 0,
            "render-cache: unexpected shader stats (fromDisk=%llu expected=%llu, compiled=%llu expected=%llu, failed=%llu)",
            (unsigned long long)stats.fromDisk, (unsigned long long)expectedFromDisk,
            (unsigned long long)stats.compiled, (unsigned long long)expectedCompiled,
            (unsigned long long)stats.failed);

        Guest::Shutdown();
        return 0;
    }

    int RunRenderSelfTest()
    {
#ifdef __APPLE__
        // Step 0. Re-exec under MTL_DEBUG_LAYER=1 if not already set (and not opted out).
        if (getenv("MTL_DEBUG_LAYER") == nullptr && getenv("COD3_SELFTEST_NO_REEXEC") == nullptr)
        {
            setenv("MTL_DEBUG_LAYER", "1", 1);
            const int childRc = SpawnSelfTestChild("--self-test=render");
            if (childRc < 0)
            {
                fprintf(stderr, "  [FAIL] Failed to re-exec under MTL_DEBUG_LAYER=1\n");
                return 1;
            }
            return childRc;
        }
#endif

        printf("=== Gate 5 Self-Test: Xenos->HLSL Translation, D3D11 Backend, Render Caches & Resolve ===\n");
        fflush(stdout);

        using Microsoft::WRL::ComPtr;

        // Isolate shader disk cache and captured shader microcode in a temporary directory for this self-test.
        const std::filesystem::path tempCacheRoot =
            std::filesystem::temp_directory_path() / ("cod3_gate5_shader_cache_" + std::to_string(GetCurrentProcessId()));
        const std::filesystem::path shaderCacheDir = tempCacheRoot / "shaders_cache";
        std::error_code ec;
        const std::filesystem::path origCwd = std::filesystem::current_path(ec);
        std::filesystem::remove_all(tempCacheRoot, ec);
        std::filesystem::create_directories(shaderCacheDir, ec);
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
        setenv("COD3_SHADER_CACHE_DIR", shaderCacheDir.string().c_str(), 1);
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
        const std::vector<uint32_t> vsWords = MakeSelftestVsWords();
        const std::vector<uint32_t> psWords = MakeSelftestPsWords();

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

#ifdef __APPLE__
        {
            struct Cd3mHeaderCheck
            {
                uint32_t magic;
                uint32_t version;
                uint32_t stage;
                uint32_t flags;
                uint32_t cbCount;
            };
            struct Cd3mCbCheck
            {
                uint16_t d3dSlot;
                uint16_t mslSlot;
                uint32_t declaredBytes;
            };
            SELFTEST_CHECK(vsBlob->GetBufferSize() >= sizeof(Cd3mHeaderCheck) + sizeof(Cd3mCbCheck),
                "VS CD3M blob too small (%zu bytes)", vsBlob->GetBufferSize());
            Cd3mHeaderCheck hdr{};
            std::memcpy(&hdr, vsBlob->GetBufferPointer(), sizeof(hdr));
            SELFTEST_CHECK(hdr.magic == 0x4D334443u && hdr.version == COD3_D3DCOMPILE_BLOB_VERSION && hdr.cbCount >= 1,
                "Unexpected CD3M header (magic=0x%08X version=%u cbCount=%u)",
                hdr.magic, hdr.version, hdr.cbCount);
            bool found4096Cb = false;
            const uint8_t* cbPtr = static_cast<const uint8_t*>(vsBlob->GetBufferPointer()) + 40; // sizeof(ShaderBlobHeader)
            for (uint32_t i = 0; i < hdr.cbCount; ++i)
            {
                Cd3mCbCheck cb{};
                std::memcpy(&cb, cbPtr + i * sizeof(Cd3mCbCheck), sizeof(cb));
                if (cb.d3dSlot == 0 && cb.declaredBytes == 4096)
                    found4096Cb = true;
            }
            SELFTEST_CHECK(found4096Cb, "Expected VS CB slot 0 declaredBytes == 4096 in CD3M v3 header");
        }
#endif

        printf("  [PASS] 1. XenosHlsl::Translate & D3DCompile succeeded (VS=%zu bytes, PS=%zu bytes, CD3M v%u, VS CB0=4096 B)\n",
            vsBlob->GetBufferSize(), psBlob->GetBufferSize(), unsigned(COD3_D3DCOMPILE_BLOB_VERSION));

        // --------------------------------------------------------------------
        // 2. Initialize Guest VM + Render backend, load shaders through
        //    Render::ShaderLoaded, wait for worker compilation, and verify
        //    the on-disk shader cache files were written.
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
        const auto vsFinalState = vsProg->state.load(std::memory_order_acquire);
        const auto psFinalState = psProg->state.load(std::memory_order_acquire);
        SELFTEST_CHECK(vsFinalState == RenderShaders::State::Ready,
            "Vertex shader failed to reach Ready state (problem='%s')",
            vsFinalState == RenderShaders::State::Failed ? vsProg->problem.c_str() : "timed out");
        SELFTEST_CHECK(psFinalState == RenderShaders::State::Ready,
            "Pixel shader failed to reach Ready state (problem='%s')",
            psFinalState == RenderShaders::State::Failed ? psProg->problem.c_str() : "timed out");
        SELFTEST_CHECK(vsProg->shader != nullptr && psProg->shader != nullptr,
            "Compiled shader device objects are null");

        // Verify disk cache files were written with 'C3SH' header.
        uint32_t cachedFilesFound = 0;
        std::filesystem::path firstCacheFile;
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
            if (firstCacheFile.empty()) firstCacheFile = entry.path();
            cachedFilesFound++;
        }
        SELFTEST_CHECK(cachedFilesFound == 2,
            "Expected 2 shader cache files in %s, found %u",
            shaderCacheDir.string().c_str(), cachedFilesFound);

        printf("  [PASS] 2. RenderShaders worker compiled VS (%016llx) & PS (%016llx) and wrote %u disk cache entries\n",
            (unsigned long long)vsProg->hash, (unsigned long long)psProg->hash, cachedFilesFound);

#ifdef __APPLE__
        // --------------------------------------------------------------------
        // 3. Cache reload: spawn child `CoD3 --self-test=render-cache` against
        //    the same cache directory and verify both programs load from disk.
        // --------------------------------------------------------------------
        setenv("COD3_SELFTEST_CACHE_EXPECTED_COMPILED", "0", 1);
        const int reloadRc = SpawnSelfTestChild("--self-test=render-cache");
        SELFTEST_CHECK(reloadRc == 0, "Child --self-test=render-cache failed (exit=%d)", reloadRc);
        printf("  [PASS] 3. Shader disk cache round-trip reload verified in child process (fromDisk=2, compiled=0)\n");

        // --------------------------------------------------------------------
        // 4. Stale blob fallback: downgrade CD3M version in one cache file to 2,
        //    spawn child again, verify it recompiles (compiled=1) and overwrites
        //    the cache file back to v3.
        // --------------------------------------------------------------------
        {
            std::vector<uint8_t> fileBytes;
            {
                std::ifstream in(firstCacheFile, std::ios::binary);
                fileBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            size_t cd3mOffset = std::string::npos;
            const uint32_t cd3mMagic = 0x4D334443u;
            for (size_t i = 0; i + 8 <= fileBytes.size(); ++i)
            {
                if (std::memcmp(fileBytes.data() + i, &cd3mMagic, 4) == 0)
                {
                    cd3mOffset = i;
                    break;
                }
            }
            SELFTEST_CHECK(cd3mOffset != std::string::npos,
                "Could not locate CD3M magic in %s", firstCacheFile.string().c_str());
            const uint32_t staleVersion = 2u;
            std::memcpy(fileBytes.data() + cd3mOffset + 4, &staleVersion, 4);
            {
                std::ofstream out(firstCacheFile, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(fileBytes.data()), std::streamsize(fileBytes.size()));
            }

            setenv("COD3_SELFTEST_CACHE_EXPECTED_COMPILED", "1", 1);
            const int staleRc = SpawnSelfTestChild("--self-test=render-cache");
            unsetenv("COD3_SELFTEST_CACHE_EXPECTED_COMPILED");
            SELFTEST_CHECK(staleRc == 0, "Child --self-test=render-cache (stale blob) failed (exit=%d)", staleRc);

            // Confirm the parent sees v3 restored in the overwritten cache file.
            std::vector<uint8_t> updatedBytes;
            {
                std::ifstream in(firstCacheFile, std::ios::binary);
                updatedBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            SELFTEST_CHECK(cd3mOffset + 8 <= updatedBytes.size(), "Updated cache file unexpectedly truncated");
            uint32_t restoredVersion = 0;
            std::memcpy(&restoredVersion, updatedBytes.data() + cd3mOffset + 4, 4);
            SELFTEST_CHECK(restoredVersion == COD3_D3DCOMPILE_BLOB_VERSION,
                "Expected cache file CD3M version to be restored to %u, got %u",
                unsigned(COD3_D3DCOMPILE_BLOB_VERSION), restoredVersion);
            printf("  [PASS] 4. Stale CD3M v2 cache entry rejected, recompiled (compiled=1), and overwritten with v%u\n",
                restoredVersion);
        }
#endif

        // --------------------------------------------------------------------
        // 5. Execute synthetic DI_PT_RECTLIST draws (non-indexed and indexed)
        //    and 6. Render::Resolve (1x and 4x MSAA) with pixel readback.
        // --------------------------------------------------------------------
        constexpr uint32_t VbPhysAddr         = 0x00200000u;
        constexpr uint32_t IbPhysAddr         = 0x00210000u;
        constexpr uint32_t ResolvePhys1x      = 0x00300000u;
        constexpr uint32_t ResolvePhysIndexed = 0x00340000u;
        constexpr uint32_t ResolvePhysMsaa    = 0x00380000u;

        auto writeGuestFloat = [](uint32_t physAddr, float v) {
            uint32_t bits = 0;
            memcpy(&bits, &v, sizeof(bits));
            Guest::Write32(Guest::Base, Guest::PhysicalAlias(physAddr), bits);
        };

        // 3 vertices for a 64x64 screen-space rectangle list (4th vertex is synthesized by main0_rect):
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

        // 16-bit big-endian indices in guest memory for the indexed DI_PT_RECTLIST draw.
        Guest::Write16(Guest::Base, Guest::PhysicalAlias(IbPhysAddr + 0), 0u);
        Guest::Write16(Guest::Base, Guest::PhysicalAlias(IbPhysAddr + 2), 1u);
        Guest::Write16(Guest::Base, Guest::PhysicalAlias(IbPhysAddr + 4), 2u);

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

        SELFTEST_CHECK(readbackAndVerify(ResolvePhys1x, 191, 128, 191, 255, "1x non-indexed DI_PT_RECTLIST"),
            "1x non-indexed rectangle-list draw + resolve pixel verification failed");

        // Indexed DI_PT_RECTLIST draw: sourceSelect = 0 (DMA), 16-bit indices at IbPhysAddr.
        // Change pixel constant c[0] to (0.0, 0.25, 0.25, 0.0) -> expected oC0 = (0.25, 0.75, 1.0, 1.0) -> RGBA (64, 191, 255, 255).
        writeRegFloat(RenderState::PixelConstants + 0, 0.00f);
        writeRegFloat(RenderState::PixelConstants + 1, 0.25f);
        writeRegFloat(RenderState::PixelConstants + 2, 0.25f);
        writeRegFloat(RenderState::PixelConstants + 3, 0.00f);

        const uint32_t indexedRectInitiator = (3u << 16) | (0u << 6) | uint32_t(RenderState::Rectangles);
        Render::Draw(indexedRectInitiator, IbPhysAddr, 3u);

        writeReg(RenderState::CopyDestBase, ResolvePhysIndexed);
        Render::Resolve();

        SELFTEST_CHECK(readbackAndVerify(ResolvePhysIndexed, 64, 191, 255, 255, "1x indexed DI_PT_RECTLIST"),
            "1x indexed rectangle-list draw + resolve pixel verification failed");
        printf("  [PASS] 5. Non-indexed & indexed DI_PT_RECTLIST draws (synthesizing 4th corner) verified in both triangles\n");

        // --------------------------------------------------------------------
        // 6. Test 4x MSAA render target draw + multisampled shader resolve.
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
        printf("  [PASS] 6. 1x & %ux MSAA Render::Resolve readback verified\n", msaaTarget->samples);

        // --------------------------------------------------------------------
        // 7. Shim parity unit tests on a fresh D3D11CreateDevice instance:
        //    (a) CB window zero-padding past bound constant count
        //    (b) Default rasterizer state (cull back)
        //    (c) Empty scissor rect skips draw
        //    (d) Pipeline cache UID when shader object is released and recreated
        //    (e) UpdateSubresource on textures: command ordering & mip 1 slice
        //    (f) QueryInterface returns E_NOINTERFACE for unknown IIDs
        //    (g) TIMESTAMP / TIMESTAMP_DISJOINT queries around a draw
        // --------------------------------------------------------------------
        {
            ComPtr<ID3D11Device> uDev;
            ComPtr<ID3D11DeviceContext> uCtxBase;
            D3D_FEATURE_LEVEL uFl = D3D_FEATURE_LEVEL_11_0;
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &uDev, &uFl, &uCtxBase);
            SELFTEST_CHECK(SUCCEEDED(hr) && uDev && uCtxBase, "Unit test D3D11CreateDevice failed");

            ComPtr<ID3D11DeviceContext1> uCtx;
            SELFTEST_CHECK(SUCCEEDED(uCtxBase.As(&uCtx)) && uCtx, "QueryInterface(ID3D11DeviceContext1) failed");

            // (f) QueryInterface for unknown IID -> E_NOINTERFACE
            {
                const GUID unknownIid = { 0xDEADBEEFu, 0xCAFEu, 0xBABEu, { 1, 2, 3, 4, 5, 6, 7, 8 } };
                void* out = reinterpret_cast<void*>(0x1);
                SELFTEST_CHECK(uDev->QueryInterface(unknownIid, &out) == E_NOINTERFACE && out == nullptr,
                    "Expected E_NOINTERFACE on device for unknown IID");
                out = reinterpret_cast<void*>(0x1);
                SELFTEST_CHECK(uCtx->QueryInterface(unknownIid, &out) == E_NOINTERFACE && out == nullptr,
                    "Expected E_NOINTERFACE on context for unknown IID");
                ComPtr<IUnknown> unk;
                SELFTEST_CHECK(SUCCEEDED(uDev.As(&unk)) && unk, "Device QI(IUnknown) failed");
                ComPtr<IDXGIDevice> dxgiDev;
                SELFTEST_CHECK(SUCCEEDED(uDev.As(&dxgiDev)) && dxgiDev, "Device QI(IDXGIDevice) failed");
            }

            // Create 32x32 render target + RTV + staging texture
            D3D11_TEXTURE2D_DESC rtDesc{};
            rtDesc.Width = 32;
            rtDesc.Height = 32;
            rtDesc.MipLevels = 1;
            rtDesc.ArraySize = 1;
            rtDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rtDesc.SampleDesc.Count = 1;
            rtDesc.Usage = D3D11_USAGE_DEFAULT;
            rtDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            ComPtr<ID3D11Texture2D> uRt, uStg;
            SELFTEST_CHECK(SUCCEEDED(uDev->CreateTexture2D(&rtDesc, nullptr, &uRt)) && uRt, "Unit CreateTexture2D(RT) failed");
            ComPtr<ID3D11RenderTargetView> uRtv;
            SELFTEST_CHECK(SUCCEEDED(uDev->CreateRenderTargetView(uRt.Get(), nullptr, &uRtv)) && uRtv, "Unit CreateRTV failed");

            D3D11_TEXTURE2D_DESC stgDesc = rtDesc;
            stgDesc.Usage = D3D11_USAGE_STAGING;
            stgDesc.BindFlags = 0;
            stgDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            SELFTEST_CHECK(SUCCEEDED(uDev->CreateTexture2D(&stgDesc, nullptr, &uStg)) && uStg, "Unit CreateTexture2D(STG) failed");

            auto readPixel = [&](uint32_t x, uint32_t y, uint8_t outRgba[4]) -> bool {
                uCtx->CopyResource(uStg.Get(), uRt.Get());
                D3D11_MAPPED_SUBRESOURCE map{};
                if (FAILED(uCtx->Map(uStg.Get(), 0, D3D11_MAP_READ, 0, &map)) || !map.pData)
                    return false;
                const uint8_t* p = static_cast<const uint8_t*>(map.pData) + y * map.RowPitch + x * 4;
                std::memcpy(outRgba, p, 4);
                uCtx->Unmap(uStg.Get(), 0);
                return true;
            };

            auto compileShader = [&](const char* src, const char* target, ID3DBlob** blobOut) -> bool {
                ComPtr<ID3DBlob> err;
                HRESULT cHr = D3DCompile(src, std::strlen(src), "unit", nullptr, nullptr, "main", target,
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blobOut, &err);
                if (FAILED(cHr))
                {
                    fprintf(stderr, "  [FAIL] Unit shader compile (%s) failed: %s\n",
                        target, err ? static_cast<const char*>(err->GetBufferPointer()) : "unknown");
                    return false;
                }
                return true;
            };

            // Full-screen CW triangle VS (front-facing under D3D11 default FrontCounterClockwise=FALSE)
            const char* vsCwSrc =
                "struct Out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
                "Out main(uint id : SV_VertexID) {\n"
                "    Out o;\n"
                "    float2 uv = float2((id << 1) & 2, id & 2);\n"
                "    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);\n"
                "    o.uv = uv;\n"
                "    return o;\n"
                "}\n";
            // Full-screen CCW triangle VS (back-facing under D3D11 default FrontCounterClockwise=FALSE)
            const char* vsCcwSrc =
                "struct Out { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
                "Out main(uint id : SV_VertexID) {\n"
                "    Out o;\n"
                "    uint rev = (id == 1) ? 2 : ((id == 2) ? 1 : 0);\n"
                "    float2 uv = float2((rev << 1) & 2, rev & 2);\n"
                "    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);\n"
                "    o.uv = uv;\n"
                "    return o;\n"
                "}\n";

            ComPtr<ID3DBlob> vsCwBlob, vsCcwBlob;
            SELFTEST_CHECK(compileShader(vsCwSrc, "vs_5_0", &vsCwBlob), "vsCw compile failed");
            SELFTEST_CHECK(compileShader(vsCcwSrc, "vs_5_0", &vsCcwBlob), "vsCcw compile failed");

            ComPtr<ID3D11VertexShader> vsCw, vsCcw;
            SELFTEST_CHECK(SUCCEEDED(uDev->CreateVertexShader(vsCwBlob->GetBufferPointer(), vsCwBlob->GetBufferSize(), nullptr, &vsCw)), "CreateVS(CW) failed");
            SELFTEST_CHECK(SUCCEEDED(uDev->CreateVertexShader(vsCcwBlob->GetBufferPointer(), vsCcwBlob->GetBufferSize(), nullptr, &vsCcw)), "CreateVS(CCW) failed");

            ID3D11RenderTargetView* rtvArr[1] = { uRtv.Get() };
            uCtx->OMSetRenderTargets(1, rtvArr, nullptr);
            D3D11_VIEWPORT vp{ 0.0f, 0.0f, 32.0f, 32.0f, 0.0f, 1.0f };
            uCtx->RSSetViewports(1, &vp);
            uCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            // (a) PS reads cb0[20] with a 16-constant window -> 0
            {
                const char* psCbSrc =
                    "cbuffer cb0 : register(b0) { float4 c[32]; };\n"
                    "float4 main() : SV_Target0 { return c[20]; }\n";
                ComPtr<ID3DBlob> psCbBlob;
                SELFTEST_CHECK(compileShader(psCbSrc, "ps_5_0", &psCbBlob), "psCb compile failed");
                ComPtr<ID3D11PixelShader> psCb;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreatePixelShader(psCbBlob->GetBufferPointer(), psCbBlob->GetBufferSize(), nullptr, &psCb)), "CreatePS(cb) failed");

                std::vector<float> cbData(32 * 4, 1.0f);
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = 32 * 16;
                bd.Usage = D3D11_USAGE_DEFAULT;
                bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                D3D11_SUBRESOURCE_DATA init{ cbData.data(), 0, 0 };
                ComPtr<ID3D11Buffer> cb;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreateBuffer(&bd, &init, &cb)), "CreateBuffer(cb) failed");

                const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
                uCtx->ClearRenderTargetView(uRtv.Get(), red);
                uCtx->VSSetShader(vsCw.Get(), nullptr, 0);
                uCtx->PSSetShader(psCb.Get(), nullptr, 0);
                ID3D11Buffer* cbs[1] = { cb.Get() };
                const UINT first = 0, count = 16;
                uCtx->PSSetConstantBuffers1(0, 1, cbs, &first, &count);
                uCtx->Draw(3, 0);

                uint8_t px[4] = { 255, 255, 255, 255 };
                SELFTEST_CHECK(readPixel(16, 16, px) && px[0] == 0 && px[1] == 0 && px[2] == 0 && px[3] == 0,
                    "7(a) CB window past 16 constants read (%u,%u,%u,%u), expected (0,0,0,0)",
                    px[0], px[1], px[2], px[3]);
            }

            // (b) Back-facing triangle with no RS state bound -> no pixels
            {
                const char* psWhiteSrc = "float4 main() : SV_Target0 { return float4(1, 1, 1, 1); }\n";
                ComPtr<ID3DBlob> psWhiteBlob;
                SELFTEST_CHECK(compileShader(psWhiteSrc, "ps_5_0", &psWhiteBlob), "psWhite compile failed");
                ComPtr<ID3D11PixelShader> psWhite;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreatePixelShader(psWhiteBlob->GetBufferPointer(), psWhiteBlob->GetBufferSize(), nullptr, &psWhite)), "CreatePS(white) failed");

                // First bind a CULL_NONE RS state and draw, then unbind RS (nullptr) to test default state restoration.
                D3D11_RASTERIZER_DESC rsNoneDesc{};
                rsNoneDesc.FillMode = D3D11_FILL_SOLID;
                rsNoneDesc.CullMode = D3D11_CULL_NONE;
                rsNoneDesc.DepthClipEnable = TRUE;
                ComPtr<ID3D11RasterizerState> rsNone;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreateRasterizerState(&rsNoneDesc, &rsNone)), "CreateRS(none) failed");

                uCtx->RSSetState(rsNone.Get());
                uCtx->VSSetShader(vsCcw.Get(), nullptr, 0);
                uCtx->PSSetShader(psWhite.Get(), nullptr, 0);
                uCtx->Draw(3, 0);

                const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                uCtx->ClearRenderTargetView(uRtv.Get(), black);
                uCtx->RSSetState(nullptr); // Default RS should cull back-facing CCW triangle
                uCtx->Draw(3, 0);

                uint8_t px[4] = { 255, 255, 255, 255 };
                SELFTEST_CHECK(readPixel(16, 16, px) && px[0] == 0 && px[1] == 0 && px[2] == 0,
                    "7(b) Back-facing triangle with nullptr RS drew (%u,%u,%u), expected (0,0,0)",
                    px[0], px[1], px[2]);

                // (c) RS with scissor on and an empty rect -> no pixels
                D3D11_RASTERIZER_DESC rsScissorDesc = rsNoneDesc;
                rsScissorDesc.ScissorEnable = TRUE;
                ComPtr<ID3D11RasterizerState> rsScissor;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreateRasterizerState(&rsScissorDesc, &rsScissor)), "CreateRS(scissor) failed");

                uCtx->ClearRenderTargetView(uRtv.Get(), black);
                uCtx->RSSetState(rsScissor.Get());
                uCtx->VSSetShader(vsCw.Get(), nullptr, 0);
                D3D11_RECT emptyRect{ 10, 10, 10, 10 };
                uCtx->RSSetScissorRects(1, &emptyRect);
                uCtx->Draw(3, 0);

                SELFTEST_CHECK(readPixel(16, 16, px) && px[0] == 0 && px[1] == 0 && px[2] == 0,
                    "7(c) Empty scissor rect drew (%u,%u,%u), expected (0,0,0)",
                    px[0], px[1], px[2]);
                uCtx->RSSetState(nullptr);
            }

            // (d) Draw with PS-A, release it, create PS-B, draw -> B's colour
            {
                const char* psASrc = "float4 main() : SV_Target0 { return float4(0, 1, 0, 1); }\n";
                const char* psBSrc = "float4 main() : SV_Target0 { return float4(0, 0, 1, 1); }\n";
                ComPtr<ID3DBlob> psABlob, psBBlob;
                SELFTEST_CHECK(compileShader(psASrc, "ps_5_0", &psABlob), "psA compile failed");
                SELFTEST_CHECK(compileShader(psBSrc, "ps_5_0", &psBBlob), "psB compile failed");

                ID3D11PixelShader* psA = nullptr;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreatePixelShader(psABlob->GetBufferPointer(), psABlob->GetBufferSize(), nullptr, &psA)) && psA, "CreatePS(A) failed");
                uCtx->VSSetShader(vsCw.Get(), nullptr, 0);
                uCtx->PSSetShader(psA, nullptr, 0);
                uCtx->Draw(3, 0);
                uCtx->PSSetShader(nullptr, nullptr, 0);
                psA->Release();

                ComPtr<ID3D11PixelShader> psB;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreatePixelShader(psBBlob->GetBufferPointer(), psBBlob->GetBufferSize(), nullptr, &psB)) && psB, "CreatePS(B) failed");
                uCtx->PSSetShader(psB.Get(), nullptr, 0);
                uCtx->Draw(3, 0);

                uint8_t px[4] = {};
                SELFTEST_CHECK(readPixel(16, 16, px) && px[0] == 0 && px[1] == 0 && px[2] == 255,
                    "7(d) Pipeline cache UID test got (%u,%u,%u), expected (0,0,255)",
                    px[0], px[1], px[2]);
            }

            // (e) Draw sampling a texture, UpdateSubresource the texture, draw again in another region
            //     -> first region old, second new; mip 1 update lands in mip 1
            {
                const char* psTexSrc =
                    "Texture2D tex0 : register(t0);\n"
                    "SamplerState smp0 : register(s0);\n"
                    "struct In { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
                    "float4 main(In i) : SV_Target0 { return tex0.SampleLevel(smp0, float2(0.5, 0.5), 0); }\n";
                ComPtr<ID3DBlob> psTexBlob;
                SELFTEST_CHECK(compileShader(psTexSrc, "ps_5_0", &psTexBlob), "psTex compile failed");
                ComPtr<ID3D11PixelShader> psTex;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreatePixelShader(psTexBlob->GetBufferPointer(), psTexBlob->GetBufferSize(), nullptr, &psTex)), "CreatePS(tex) failed");

                std::vector<uint32_t> redMip0(32 * 32, 0xFF0000FFu);   // RGBA: (255, 0, 0, 255)
                std::vector<uint32_t> redMip1(16 * 16, 0xFF0000FFu);
                std::vector<uint32_t> greenMip0(32 * 32, 0xFF00FF00u); // RGBA: (0, 255, 0, 255)
                std::vector<uint32_t> blueMip1(16 * 16, 0xFFFF0000u);  // RGBA: (0, 0, 255, 255)

                D3D11_TEXTURE2D_DESC td{};
                td.Width = 32;
                td.Height = 32;
                td.MipLevels = 2;
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA initData[2] = {
                    { redMip0.data(), 32 * 4, 0 },
                    { redMip1.data(), 16 * 4, 0 }
                };
                ComPtr<ID3D11Texture2D> sampleTex;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreateTexture2D(&td, initData, &sampleTex)), "CreateTexture2D(sampleTex) failed");
                ComPtr<ID3D11ShaderResourceView> srvMip0;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreateShaderResourceView(sampleTex.Get(), nullptr, &srvMip0)), "CreateSRV(sampleTex) failed");

                const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                uCtx->ClearRenderTargetView(uRtv.Get(), black);
                uCtx->VSSetShader(vsCw.Get(), nullptr, 0);
                uCtx->PSSetShader(psTex.Get(), nullptr, 0);
                ID3D11ShaderResourceView* srvs[1] = { srvMip0.Get() };
                uCtx->PSSetShaderResources(0, 1, srvs);

                // First draw in left half (0..16) -> should see red
                D3D11_VIEWPORT leftVp{ 0.0f, 0.0f, 16.0f, 32.0f, 0.0f, 1.0f };
                uCtx->RSSetViewports(1, &leftVp);
                uCtx->Draw(3, 0);

                // Update mip 0 to green mid-stream
                uCtx->UpdateSubresource(sampleTex.Get(), 0, nullptr, greenMip0.data(), 32 * 4, 0);

                // Second draw in right half (16..32) -> should see green
                D3D11_VIEWPORT rightVp{ 16.0f, 0.0f, 16.0f, 32.0f, 0.0f, 1.0f };
                uCtx->RSSetViewports(1, &rightVp);
                uCtx->Draw(3, 0);

                uint8_t pxLeft[4] = {}, pxRight[4] = {};
                SELFTEST_CHECK(readPixel(8, 16, pxLeft) && pxLeft[0] == 255 && pxLeft[1] == 0 && pxLeft[2] == 0,
                    "7(e) Pre-UpdateSubresource region got (%u,%u,%u), expected red (255,0,0)",
                    pxLeft[0], pxLeft[1], pxLeft[2]);
                SELFTEST_CHECK(readPixel(24, 16, pxRight) && pxRight[0] == 0 && pxRight[1] == 255 && pxRight[2] == 0,
                    "7(e) Post-UpdateSubresource region got (%u,%u,%u), expected green (0,255,0)",
                    pxRight[0], pxRight[1], pxRight[2]);

                // Update mip 1 to blue and verify mip 1 is blue while mip 0 stays green
                uCtx->UpdateSubresource(sampleTex.Get(), 1, nullptr, blueMip1.data(), 16 * 4, 0);
                D3D11_TEXTURE2D_DESC stg2Desc = td;
                stg2Desc.Usage = D3D11_USAGE_STAGING;
                stg2Desc.BindFlags = 0;
                stg2Desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ComPtr<ID3D11Texture2D> stg2;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreateTexture2D(&stg2Desc, nullptr, &stg2)), "CreateTexture2D(stg2) failed");
                uCtx->CopyResource(stg2.Get(), sampleTex.Get());

                D3D11_MAPPED_SUBRESOURCE mapMip0{}, mapMip1{};
                SELFTEST_CHECK(SUCCEEDED(uCtx->Map(stg2.Get(), 0, D3D11_MAP_READ, 0, &mapMip0)) && mapMip0.pData, "Map(mip0) failed");
                const uint8_t* m0 = static_cast<const uint8_t*>(mapMip0.pData);
                SELFTEST_CHECK(m0[0] == 0 && m0[1] == 255 && m0[2] == 0, "7(e) Mip 0 overwritten by mip 1 update");
                uCtx->Unmap(stg2.Get(), 0);

                SELFTEST_CHECK(SUCCEEDED(uCtx->Map(stg2.Get(), 1, D3D11_MAP_READ, 0, &mapMip1)) && mapMip1.pData, "Map(mip1) failed");
                const uint8_t* m1 = static_cast<const uint8_t*>(mapMip1.pData);
                SELFTEST_CHECK(m1[0] == 0 && m1[1] == 0 && m1[2] == 255,
                    "7(e) Mip 1 update got (%u,%u,%u), expected blue (0,0,255)", m1[0], m1[1], m1[2]);
                uCtx->Unmap(stg2.Get(), 1);
                uCtx->RSSetViewports(1, &vp);
            }

            // (g) TIMESTAMP begin/end around a draw -> GetData S_OK after flush, end >= begin
            {
                D3D11_QUERY_DESC qd{};
                qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
                ComPtr<ID3D11Query> qDisjoint, qBegin, qEnd;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreateQuery(&qd, &qDisjoint)) && qDisjoint, "CreateQuery(DISJOINT) failed");
                qd.Query = D3D11_QUERY_TIMESTAMP;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreateQuery(&qd, &qBegin)) && qBegin, "CreateQuery(TIMESTAMP begin) failed");
                SELFTEST_CHECK(SUCCEEDED(uDev->CreateQuery(&qd, &qEnd)) && qEnd, "CreateQuery(TIMESTAMP end) failed");

                uCtx->Begin(qDisjoint.Get());
                uCtx->End(qBegin.Get());
                uCtx->Draw(3, 0);
                uCtx->End(qEnd.Get());
                uCtx->End(qDisjoint.Get());
                uCtx->Flush();

                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disj{};
                UINT64 t0 = 0, t1 = 0;
                const auto qDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                bool gotDisj = false, gotT0 = false, gotT1 = false;
                while (std::chrono::steady_clock::now() < qDeadline)
                {
                    if (!gotDisj && uCtx->GetData(qDisjoint.Get(), &disj, sizeof(disj), 0) == S_OK) gotDisj = true;
                    if (!gotT0 && uCtx->GetData(qBegin.Get(), &t0, sizeof(t0), 0) == S_OK) gotT0 = true;
                    if (!gotT1 && uCtx->GetData(qEnd.Get(), &t1, sizeof(t1), 0) == S_OK) gotT1 = true;
                    if (gotDisj && gotT0 && gotT1) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                SELFTEST_CHECK(gotDisj && gotT0 && gotT1, "7(g) Timed out waiting for TIMESTAMP query GetData");
                SELFTEST_CHECK(!disj.Disjoint && disj.Frequency > 0 && t1 >= t0 && t1 > 0,
                    "7(g) Invalid timestamp query values (disjoint=%d freq=%llu t0=%llu t1=%llu)",
                    int(disj.Disjoint), (unsigned long long)disj.Frequency,
                    (unsigned long long)t0, (unsigned long long)t1);
            }

            // Also verify depth readback (C-6: Depth32Float_Stencil8 -> R32G8X24 staging repack)
            {
                D3D11_TEXTURE2D_DESC dDesc = rtDesc;
                dDesc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
                dDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
                ComPtr<ID3D11Texture2D> dTex, dStg;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreateTexture2D(&dDesc, nullptr, &dTex)) && dTex, "CreateTexture2D(depth) failed");
                D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
                dsvDesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
                dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                ComPtr<ID3D11DepthStencilView> dsv;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreateDepthStencilView(dTex.Get(), &dsvDesc, &dsv)) && dsv, "CreateDSV failed");
                uCtx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.25f, 0);

                dDesc.Usage = D3D11_USAGE_STAGING;
                dDesc.BindFlags = 0;
                dDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreateTexture2D(&dDesc, nullptr, &dStg)) && dStg, "CreateTexture2D(depth staging) failed");
                uCtx->CopyResource(dStg.Get(), dTex.Get());
                D3D11_MAPPED_SUBRESOURCE dMap{};
                SELFTEST_CHECK(SUCCEEDED(uCtx->Map(dStg.Get(), 0, D3D11_MAP_READ, 0, &dMap)) && dMap.pData, "Map(depth staging) failed");
                float z0 = 0.0f, zMid = 0.0f;
                std::memcpy(&z0, static_cast<const uint8_t*>(dMap.pData), 4);
                std::memcpy(&zMid, static_cast<const uint8_t*>(dMap.pData) + 16 * dMap.RowPitch + 16 * 8, 4);
                uCtx->Unmap(dStg.Get(), 0);
                SELFTEST_CHECK(std::fabs(z0 - 0.25f) < 1e-4f && std::fabs(zMid - 0.25f) < 1e-4f,
                    "Depth staging readback got z0=%f zMid=%f, expected 0.25", z0, zMid);
            }

            // (h) MipLevels == 0 full mip-chain calculation + GenerateMips, high VS SRV slots (>= 64),
            //     adapter outliving factory, and immediate context outliving device pointer.
            {
                D3D11_TEXTURE2D_DESC autoMipDesc = rtDesc;
                autoMipDesc.Width = 32;
                autoMipDesc.Height = 32;
                autoMipDesc.MipLevels = 0;
                autoMipDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                autoMipDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
                ComPtr<ID3D11Texture2D> autoMipTex;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreateTexture2D(&autoMipDesc, nullptr, &autoMipTex)) && autoMipTex,
                    "CreateTexture2D(MipLevels=0) failed");
                D3D11_TEXTURE2D_DESC queriedMipDesc{};
                autoMipTex->GetDesc(&queriedMipDesc);
                SELFTEST_CHECK(queriedMipDesc.MipLevels == 6,
                    "Expected MipLevels=0 on 32x32 texture to resolve to 6 mips, got %u", queriedMipDesc.MipLevels);

                ComPtr<ID3D11ShaderResourceView> autoMipSrv;
                SELFTEST_CHECK(SUCCEEDED(uDev->CreateShaderResourceView(autoMipTex.Get(), nullptr, &autoMipSrv)) && autoMipSrv,
                    "CreateShaderResourceView(MipLevels=0) failed");
                uCtx->GenerateMips(autoMipSrv.Get());

                // Bind and unbind at Xenos vertex fetch slot t96 (>= 64)
                ID3D11ShaderResourceView* hiSrv = autoMipSrv.Get();
                uCtx->VSSetShaderResources(96, 1, &hiSrv);
                ID3D11ShaderResourceView* nullSrv = nullptr;
                uCtx->VSSetShaderResources(96, 1, &nullSrv);

                // Adapter outliving factory
                ComPtr<IDXGIFactory1> tempFac;
                ComPtr<IDXGIAdapter1> tempAdp;
                SELFTEST_CHECK(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&tempFac))) && tempFac, "CreateDXGIFactory1 failed");
                SELFTEST_CHECK(SUCCEEDED(tempFac->EnumAdapters1(0, &tempAdp)) && tempAdp, "EnumAdapters1(0) failed");
                tempFac.Reset();
                ComPtr<IDXGIFactory1> parentFac;
                SELFTEST_CHECK(SUCCEEDED(tempAdp->GetParent(IID_PPV_ARGS(&parentFac))) && parentFac,
                    "Adapter->GetParent failed after initial factory reference was released");

                // Immediate context outliving device reference (ppDevice == nullptr)
                ComPtr<ID3D11DeviceContext> standaloneCtx;
                SELFTEST_CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                    nullptr, 0, D3D11_SDK_VERSION, nullptr, nullptr, &standaloneCtx)) && standaloneCtx,
                    "D3D11CreateDevice(ppDevice=nullptr) failed");
                ComPtr<ID3D11Device> recoveredDev;
                standaloneCtx->GetDevice(&recoveredDev);
                SELFTEST_CHECK(recoveredDev != nullptr, "Context->GetDevice returned null after ppDevice=nullptr");
            }

            printf("  [PASS] 7. Shim parity unit tests passed: (a) CB window 0-pad, (b) default RS cull, (c) empty scissor, (d) PSO UID, (e) UpdateSubresource ordering/mip, (f) QI E_NOINTERFACE, (g) GPU timestamps, (h) MipLevels=0/SRV128/lifetimes\n");

#ifdef __APPLE__
            // --------------------------------------------------------------------
            // 8. Swap chain test on a hidden SDL_WINDOW_METAL window:
            //    CreateSwapChainForHwnd 128x128, Present(1), Present(0),
            //    ResizeBuffers(160, 120), Present(0); then Render::SetWindow +
            //    Render::PresentIdle() creates the renderer's swap chain.
            // --------------------------------------------------------------------
            {
                const bool allowNoWindow = getenv("COD3_SELFTEST_NO_WINDOW") != nullptr;
                if (SDL_Init(SDL_INIT_VIDEO) != 0)
                {
                    if (allowNoWindow)
                    {
                        printf("  [SKIP] 8. Swap chain test: SKIPPED (SDL_Init failed: %s)\n", SDL_GetError());
                    }
                    else
                    {
                        SELFTEST_CHECK(false, "SDL_Init(SDL_INIT_VIDEO) failed: %s", SDL_GetError());
                    }
                }
                else
                {
                    SDL_Window* win1 = SDL_CreateWindow("CoD3Gate5Swap1", 0, 0, 128, 128, SDL_WINDOW_HIDDEN | SDL_WINDOW_METAL);
                    if (!win1 && allowNoWindow)
                    {
                        printf("  [SKIP] 8. Swap chain test: SKIPPED (SDL_CreateWindow failed: %s)\n", SDL_GetError());
                        SDL_QuitSubSystem(SDL_INIT_VIDEO);
                    }
                    else
                    {
                        SELFTEST_CHECK(win1 != nullptr, "SDL_CreateWindow(SDL_WINDOW_METAL) failed: %s", SDL_GetError());
                        ComPtr<IDXGIDevice> dxgiDev;
                        ComPtr<IDXGIAdapter> dxgiAdp;
                        ComPtr<IDXGIFactory2> dxgiFac;
                        SELFTEST_CHECK(SUCCEEDED(uDev.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&dxgiAdp)) &&
                            SUCCEEDED(dxgiAdp->GetParent(IID_PPV_ARGS(&dxgiFac))) && dxgiFac,
                            "DXGI factory hierarchy query failed");

                        DXGI_SWAP_CHAIN_DESC1 scDesc{};
                        scDesc.Width = 128;
                        scDesc.Height = 128;
                        scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                        scDesc.SampleDesc.Count = 1;
                        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                        scDesc.BufferCount = 3;
                        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

                        ComPtr<IDXGISwapChain1> sc1;
                        SELFTEST_CHECK(SUCCEEDED(dxgiFac->CreateSwapChainForHwnd(uDev.Get(), win1, &scDesc, nullptr, nullptr, &sc1)) && sc1,
                            "CreateSwapChainForHwnd(128x128) failed");

                        ComPtr<IDXGISwapChain2> sc2;
                        SELFTEST_CHECK(SUCCEEDED(sc1.As(&sc2)) && SUCCEEDED(sc2->SetMaximumFrameLatency(2)),
                            "IDXGISwapChain2::SetMaximumFrameLatency failed");

                        {
                            ComPtr<ID3D11Texture2D> bb;
                            ComPtr<ID3D11RenderTargetView> bbRtv;
                            SELFTEST_CHECK(SUCCEEDED(sc1->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb, "SwapChain GetBuffer(0) failed");
                            SELFTEST_CHECK(SUCCEEDED(uDev->CreateRenderTargetView(bb.Get(), nullptr, &bbRtv)) && bbRtv, "CreateRTV(backBuffer) failed");
                            const float clr[4] = { 0.1f, 0.2f, 0.3f, 1.0f };
                            uCtx->ClearRenderTargetView(bbRtv.Get(), clr);
                            SELFTEST_CHECK(SUCCEEDED(sc1->Present(1, 0)), "SwapChain Present(1) failed");
                            SELFTEST_CHECK(SUCCEEDED(sc1->Present(0, 0)), "SwapChain Present(0) failed");
                        }

                        uCtx->OMSetRenderTargets(0, nullptr, nullptr);
                        SELFTEST_CHECK(SUCCEEDED(sc1->ResizeBuffers(0, 160, 120, DXGI_FORMAT_UNKNOWN, 0)),
                            "SwapChain ResizeBuffers(160, 120) failed");
                        {
                            ComPtr<ID3D11Texture2D> bb;
                            SELFTEST_CHECK(SUCCEEDED(sc1->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb, "GetBuffer after ResizeBuffers failed");
                            D3D11_TEXTURE2D_DESC bbDesc{};
                            bb->GetDesc(&bbDesc);
                            SELFTEST_CHECK(bbDesc.Width == 160 && bbDesc.Height == 120,
                                "Expected resized backBuffer 160x120, got %ux%u", bbDesc.Width, bbDesc.Height);
                            SELFTEST_CHECK(SUCCEEDED(sc1->Present(0, 0)), "SwapChain Present(0) after resize failed");
                        }
                        sc2.Reset();
                        sc1.Reset();
                        SDL_DestroyWindow(win1);

                        // Also test the renderer's own swap chain path via Render::SetWindow + Render::PresentIdle()
                        SDL_Window* win2 = SDL_CreateWindow("CoD3Gate5Swap2", 0, 0, 128, 128, SDL_WINDOW_HIDDEN | SDL_WINDOW_METAL);
                        SELFTEST_CHECK(win2 != nullptr, "SDL_CreateWindow(win2) failed");
                        Render::SetWindow(win2, 128, 128);
                        Render::PresentIdle();
                        Render::SetWindow(nullptr, 0, 0);
                        SDL_DestroyWindow(win2);
                        SDL_QuitSubSystem(SDL_INIT_VIDEO);

                        printf("  [PASS] 8. Swap chain creation, Present(1/0), ResizeBuffers(160x120), and Render::SetWindow + PresentIdle verified\n");
                    }
                }
            }
#endif
        }

        // --------------------------------------------------------------------
        // 9. Cache statistics assertions.
        // --------------------------------------------------------------------
        const RenderPipeline::Statistics pipeStats = RenderPipeline::Stats();
        const RenderResources::Statistics resStats = RenderResources::Stats();
        SELFTEST_CHECK(pipeStats.blendStates >= 1 && pipeStats.depthStates >= 1 && pipeStats.rasterizerStates >= 1,
            "Expected pipeline state caches to be populated (blend=%u depth=%u rast=%u)",
            pipeStats.blendStates, pipeStats.depthStates, pipeStats.rasterizerStates);
        SELFTEST_CHECK(resStats.buffers >= 1 && resStats.colorTargets >= 1 && resStats.depthTargets >= 1 && resStats.resolved >= 2,
            "Expected resource caches to be populated (buffers=%u colorTargets=%u depthTargets=%u resolved=%u)",
            resStats.buffers, resStats.colorTargets, resStats.depthTargets, resStats.resolved);
        printf("  [PASS] 9. Pipeline and resource cache statistics verified (blend=%u depth=%u rast=%u buffers=%u colorTargets=%u depthTargets=%u resolved=%u)\n",
            pipeStats.blendStates, pipeStats.depthStates, pipeStats.rasterizerStates,
            resStats.buffers, resStats.colorTargets, resStats.depthTargets, resStats.resolved);

        Guest::Shutdown();
        printf("=== Gate 5 Self-Test: ALL RENDER ASSERTIONS PASSED ===\n");
        fflush(stdout);
        return 0;
    }

    int RunAudioSelfTest()
    {
        printf("=== Gate 6 Self-Test: SDL Windowing, Input, ImGui Overlay, Screenshot, 6->2 Audio & XMA ===\n");
        fflush(stdout);

        using Microsoft::WRL::ComPtr;

        const std::filesystem::path tempRoot =
            std::filesystem::temp_directory_path() / ("cod3_gate6_selftest_" + std::to_string(GetCurrentProcessId()));
        const std::filesystem::path screenshotDir = tempRoot / "screenshots";
        const std::filesystem::path wavPath = tempRoot / "audiodump.wav";
        std::error_code ec;
        std::filesystem::remove_all(tempRoot, ec);
        std::filesystem::create_directories(screenshotDir, ec);
        struct ScopedCleanup
        {
            std::filesystem::path root;
            ~ScopedCleanup()
            {
                std::error_code cleanupEc;
                std::filesystem::remove_all(root, cleanupEc);
            }
        } cleanupGuard{ tempRoot };

#ifndef _WIN32
        setenv("COD3_HIDDEN_WINDOW", "1", 1);
        setenv("COD3_BACKGROUND", "1", 1);
        setenv("COD3_SCREENSHOT_DIR", screenshotDir.string().c_str(), 1);
        setenv("COD3_AUDIODUMP", wavPath.string().c_str(), 1);
        setenv("SDL_AUDIODRIVER", "dummy", 1);
#endif

        SELFTEST_CHECK(Guest::Initialize(nullptr), "Guest::Initialize(nullptr) failed");
        SELFTEST_CHECK(Guest::Base != nullptr, "Guest::Base is null");

        // --------------------------------------------------------------------
        // 1. Settings::Load & DisplayModes + KeyName
        // --------------------------------------------------------------------
        Settings::Load(tempRoot);
        const auto& displayModes = Settings::DisplayModes();
        SELFTEST_CHECK(!displayModes.empty(), "Settings::DisplayModes returned empty list");
        SELFTEST_CHECK(std::strcmp(Settings::KeyName('W'), "W") == 0 &&
                       std::strcmp(Settings::KeyName(VK_SPACE), "Space") == 0,
            "Settings::KeyName unexpected output ('%s', '%s')",
            Settings::KeyName('W'), Settings::KeyName(VK_SPACE));
        printf("  [PASS] 1. Settings::Load, DisplayModes (%zu modes), and KeyName verified\n",
            displayModes.size());

        // --------------------------------------------------------------------
        // 2. Window::Open (hidden SDL_WINDOW_METAL window on main thread),
        //    Window::SetFrontBuffer, and Window::Close
        // --------------------------------------------------------------------
        Window::Open();
        SELFTEST_CHECK(Window::IsOpen() && Window::Handle() != nullptr,
            "Window::Open failed to create hidden SDL Metal window");
        Window::SetFrontBuffer(0xA0100000u, 1280, 720);

        // --------------------------------------------------------------------
        // 3. Dear ImGui Overlay::Initialize, event handling, F11 settings menu
        //    + console rendering via ImGui_ImplDX11_RenderDrawData, and F12
        //    CoreGraphics/ImageIO PNG screenshot output.
        // --------------------------------------------------------------------
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
        SELFTEST_CHECK(SUCCEEDED(hr) && dev && ctx, "D3D11CreateDevice failed (hr=0x%08X)", unsigned(hr));

        D3D11_TEXTURE2D_DESC backDesc{};
        backDesc.Width = 320;
        backDesc.Height = 200;
        backDesc.MipLevels = 1;
        backDesc.ArraySize = 1;
        backDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        backDesc.SampleDesc.Count = 1;
        backDesc.Usage = D3D11_USAGE_DEFAULT;
        backDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> backTex;
        ComPtr<ID3D11RenderTargetView> backRtv;
        SELFTEST_CHECK(SUCCEEDED(dev->CreateTexture2D(&backDesc, nullptr, &backTex)) && backTex,
            "CreateTexture2D(backTex) failed");
        SELFTEST_CHECK(SUCCEEDED(dev->CreateRenderTargetView(backTex.Get(), nullptr, &backRtv)) && backRtv,
            "CreateRenderTargetView(backRtv) failed");

        const float clearColor[4] = { 0.12f, 0.25f, 0.50f, 1.0f };
        ctx->ClearRenderTargetView(backRtv.Get(), clearColor);

        Overlay::Initialize(Window::Handle(), dev.Get(), ctx.Get());
        Overlay::NoteFrame();
        Overlay::ConsolePrint("Gate 6 overlay self-test line");

        // Open settings menu (F11), render one ImGui frame over backRtv, and request a PNG screenshot.
        Overlay::Toggle();
        SELFTEST_CHECK(Overlay::IsOpen(), "Overlay::Toggle did not open settings menu");
#ifndef _WIN32
        {
            SDL_Event moveEv{};
            moveEv.type = SDL_MOUSEMOTION;
            moveEv.motion.x = 50;
            moveEv.motion.y = 50;
            SELFTEST_CHECK(Overlay::HandleSDLEvent(&moveEv),
                "Overlay::HandleSDLEvent did not consume mouse event while menu open");
        }
#endif
        Overlay::RequestScreenshot();
        // Dear ImGui measures newly-created auto-sized windows on frame 1 and renders them on frame 2.
        Overlay::Render(backRtv.Get(), backTex.Get(), 320, 200);
        Overlay::Render(backRtv.Get(), backTex.Get(), 320, 200);
        Overlay::Toggle();
        SELFTEST_CHECK(!Overlay::IsOpen(), "Overlay::Toggle did not close settings menu");

        // Also test console overlay rendering.
        Overlay::ToggleConsole();
        SELFTEST_CHECK(Overlay::IsOpen(), "Overlay::ToggleConsole did not open console");
        Overlay::Render(backRtv.Get(), backTex.Get(), 320, 200);
        Overlay::ToggleConsole();
        SELFTEST_CHECK(!Overlay::IsOpen(), "Overlay::ToggleConsole did not close console");

        // Verify that the PNG screenshot was written with a valid 8-byte PNG signature.
        bool foundValidPng = false;
        std::filesystem::path pngFile;
        for (const auto& entry : std::filesystem::directory_iterator(screenshotDir, ec))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".png") continue;
            std::ifstream in(entry.path(), std::ios::binary);
            uint8_t sig[8] = {};
            in.read(reinterpret_cast<char*>(sig), sizeof(sig));
            const uint8_t expectedPngSig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
            if (in.gcount() == 8 && std::memcmp(sig, expectedPngSig, 8) == 0)
            {
                foundValidPng = true;
                pngFile = entry.path();
                break;
            }
        }
        SELFTEST_CHECK(foundValidPng, "Overlay screenshot did not produce a valid PNG file in %s",
            screenshotDir.string().c_str());

        // Read back the rendered backTex after ImGui draw to confirm non-clear pixels were drawn by ImGui.
        D3D11_TEXTURE2D_DESC stageDesc = backDesc;
        stageDesc.Usage = D3D11_USAGE_STAGING;
        stageDesc.BindFlags = 0;
        stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> stageTex;
        SELFTEST_CHECK(SUCCEEDED(dev->CreateTexture2D(&stageDesc, nullptr, &stageTex)) && stageTex,
            "CreateTexture2D(stageTex) failed");
        ctx->CopyResource(stageTex.Get(), backTex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        SELFTEST_CHECK(SUCCEEDED(ctx->Map(stageTex.Get(), 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData,
            "Map(stageTex) failed");
        uint32_t imguiDrawnPixels = 0;
        const uint8_t* baseRow = static_cast<const uint8_t*>(mapped.pData);
        const uint8_t bgB = baseRow[0], bgG = baseRow[1], bgR = baseRow[2];
        for (uint32_t y = 0; y < 200; ++y)
        {
            const uint8_t* row = baseRow + y * mapped.RowPitch;
            for (uint32_t x = 0; x < 320; ++x)
            {
                if (row[x * 4 + 0] != bgB || row[x * 4 + 1] != bgG || row[x * 4 + 2] != bgR)
                    imguiDrawnPixels++;
            }
        }
        ctx->Unmap(stageTex.Get(), 0);
        SELFTEST_CHECK(imguiDrawnPixels > 100,
            "Expected ImGui overlay to draw >100 pixels onto backbuffer (got %u)", imguiDrawnPixels);

        Window::Close();
        SELFTEST_CHECK(!Window::IsOpen(), "Window::Close did not mark window closed");
        printf("  [PASS] 2. SDL Metal window + Dear ImGui overlay rendered (%u UI pixels) and wrote valid PNG (%s)\n",
            imguiDrawnPixels, pngFile.filename().string().c_str());

        // --------------------------------------------------------------------
        // 4. Input::State & XamInputGetState guest controller state poll
        // --------------------------------------------------------------------
        {
            const Input::Pad pad = Input::State();
            const uint32_t pkt = Input::PacketNumber();
            SELFTEST_CHECK(pkt >= 1 && Input::KeyboardHelp()[0] != '\0',
                "Input::PacketNumber / KeyboardHelp invalid (pkt=%u)", pkt);
            (void)pad;

            PPCContext guestCtx{};
            constexpr uint32_t GuestInputStateAddr = 0x00108000u;
            std::memset(Guest::Ptr(GuestInputStateAddr), 0xFF, 16);
            guestCtx.r3.u32 = 0; // userIndex 0
            guestCtx.r4.u32 = 0;
            guestCtx.r5.u32 = GuestInputStateAddr;
            __imp__XamInputGetState(guestCtx, Guest::Base);
            SELFTEST_CHECK(guestCtx.r3.u32 == 0,
                "XamInputGetState(user 0) failed with 0x%08X", guestCtx.r3.u32);
            const uint32_t guestPkt = Guest::Read32(Guest::Base, GuestInputStateAddr);
            SELFTEST_CHECK(guestPkt == pkt,
                "XamInputGetState packet number mismatch (guest=%u host=%u)", guestPkt, pkt);
        }
        printf("  [PASS] 3. Input::State and XamInputGetState verified\n");

        // --------------------------------------------------------------------
        // 5. Audio::SubmitFrame 6-channel planar big-endian float -> stereo
        //    S16LE downmix + SDL2 audio queue + COD3_AUDIODUMP WAV verification
        // --------------------------------------------------------------------
        {
            constexpr uint32_t AudioFrameGuestAddr = 0xA0200000u;
            constexpr uint32_t SamplesPerCh = 256;
            constexpr uint32_t NumChannels = 6;
            auto writeBeFloat = [](uint32_t addr, float v) {
                uint32_t bits = 0;
                std::memcpy(&bits, &v, sizeof(bits));
                Guest::Write32(Guest::Base, addr, bits);
            };
            // Populate 6 planar channels: L=+0.5, R=-0.25, C=+0.2, LFE=0.0, LS=+0.1, RS=-0.1
            const float chValues[NumChannels] = { 0.5f, -0.25f, 0.2f, 0.0f, 0.1f, -0.1f };
            for (uint32_t ch = 0; ch < NumChannels; ++ch)
            {
                for (uint32_t s = 0; s < SamplesPerCh; ++s)
                    writeBeFloat(AudioFrameGuestAddr + (ch * SamplesPerCh + s) * 4u, chValues[ch]);
            }

            SELFTEST_CHECK(Audio::SubmitFrame(AudioFrameGuestAddr),
                "Audio::SubmitFrame failed");
            Audio::Report();
            Audio::Close();

            // Verify the dumped WAV header and downmixed stereo samples:
            // L_out = ToPcm(0.5 + 0.707*0.2 + 0.707*0.1) = ToPcm(0.7121) = int16(0.7121 * 0.8 * 32767) = 18667
            // R_out = ToPcm(-0.25 + 0.707*0.2 + 0.707*-0.1) = ToPcm(-0.1793) = int16(-0.1793 * 0.8 * 32767) = -4700
            std::ifstream wavIn(wavPath, std::ios::binary);
            std::vector<uint8_t> wavBytes((std::istreambuf_iterator<char>(wavIn)),
                                           std::istreambuf_iterator<char>());
            SELFTEST_CHECK(wavBytes.size() == 44 + SamplesPerCh * 2 * sizeof(int16_t),
                "Unexpected WAV dump size (%zu bytes, expected %zu)",
                wavBytes.size(), size_t(44 + SamplesPerCh * 2 * sizeof(int16_t)));
            SELFTEST_CHECK(std::memcmp(wavBytes.data(), "RIFF", 4) == 0 &&
                           std::memcmp(wavBytes.data() + 8, "WAVEfmt ", 8) == 0 &&
                           std::memcmp(wavBytes.data() + 36, "data", 4) == 0,
                "Invalid WAV header in %s", wavPath.string().c_str());

            int16_t sampleL = 0, sampleR = 0;
            std::memcpy(&sampleL, wavBytes.data() + 44, sizeof(int16_t));
            std::memcpy(&sampleR, wavBytes.data() + 46, sizeof(int16_t));
            SELFTEST_CHECK(std::abs(int(sampleL) - 18667) <= 2 && std::abs(int(sampleR) - (-4700)) <= 2,
                "6->2 downmix sample mismatch: got L=%d R=%d (expected L~18667 R~-4700)",
                int(sampleL), int(sampleR));
            printf("  [PASS] 4. Audio::SubmitFrame 6-ch planar BE float -> stereo S16LE downmix & WAV dump verified (L=%d, R=%d)\n",
                int(sampleL), int(sampleR));
        }

        // --------------------------------------------------------------------
        // 6. Xma::Available / Create / Reset / Destroy + XMACreateContext /
        //    XMAInitializeContext / XMAReleaseContext kernel audio round-trip
        // --------------------------------------------------------------------
        {
            if (Xma::Available())
            {
                Xma::Stream* s1 = Xma::Create(1, 48000);
                Xma::Stream* s2 = Xma::Create(2, 44100);
                SELFTEST_CHECK(s1 != nullptr && s2 != nullptr,
                    "Xma::Create failed when Xma::Available() is true");
                Xma::Reset(s1);
                Xma::Destroy(s1);
                Xma::Destroy(s2);
                printf("  [PASS] 5. XMA FFmpeg dynamic loader active: Xma::Create(mono/stereo), Reset, and Destroy verified\n");
            }
            else
            {
                printf("  [PASS] 5. XMA fallback active (%s)\n", Xma::Unavailable());
            }

            PPCContext xmaCtx{};
            constexpr uint32_t CtxOutPtr  = 0x00109000u;
            constexpr uint32_t InitParams = 0x00109100u;
            Guest::Write32(Guest::Base, CtxOutPtr, 0);
            xmaCtx.r3.u32 = CtxOutPtr;
            __imp__XMACreateContext(xmaCtx, Guest::Base);
            const uint32_t createdCtx = Guest::Read32(Guest::Base, CtxOutPtr);
            SELFTEST_CHECK(xmaCtx.r3.u32 == 0 && createdCtx != 0,
                "XMACreateContext failed (r3=0x%08X ctx=0x%08X)", xmaCtx.r3.u32, createdCtx);

            std::memset(Guest::Ptr(InitParams), 0, 64);
            Guest::Write32(Guest::Base, InitParams + 5 * 4, 0xA0210000u); // output buffer
            Guest::Write32(Guest::Base, InitParams + 6 * 4, 8u);          // outputBlocks
            Guest::Write32(Guest::Base, InitParams + 8 * 4, 2u);          // subframes
            Guest::Write32(Guest::Base, InitParams + 9 * 4, 1u);          // stereo
            Guest::Write32(Guest::Base, InitParams + 10 * 4, 3u);         // 48000 Hz
            xmaCtx = PPCContext{};
            xmaCtx.r3.u32 = createdCtx;
            xmaCtx.r4.u32 = InitParams;
            __imp__XMAInitializeContext(xmaCtx, Guest::Base);
            SELFTEST_CHECK(xmaCtx.r3.u32 == 0, "XMAInitializeContext failed");

            xmaCtx = PPCContext{};
            xmaCtx.r3.u32 = createdCtx;
            __imp__XMAReleaseContext(xmaCtx, Guest::Base);
            SELFTEST_CHECK(xmaCtx.r3.u32 == 0, "XMAReleaseContext failed");
            printf("  [PASS] 6. XMACreateContext / XMAInitializeContext / XMAReleaseContext kernel round-trip verified\n");
        }

        Guest::Shutdown();
        printf("=== Gate 6 Self-Test: ALL WINDOW, INPUT, OVERLAY & AUDIO ASSERTIONS PASSED ===\n");
        fflush(stdout);
        return 0;
    }
}

namespace SelfTest
{
    int Run(const std::string& suite)
    {
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        if (suite == "render-cache")
        {
            return RunRenderCacheSelfTest();
        }
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
        if (suite == "audio" || suite == "all")
        {
            const int rc = RunAudioSelfTest();
            if (rc != 0 || suite == "audio")
                return rc;
        }
        if (suite != "vm" && suite != "runtime" && suite != "render" && suite != "audio" && suite != "all")
        {
            fprintf(stderr, "Unknown self-test suite '%s' (expected vm, runtime, render, audio, or all).\n",
                suite.c_str());
            return 1;
        }
        return 0;
    }
}
