#include "selftest.h"
#include "kernel.h"
#include "write_watch.h"
#include "platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

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
        printf("  [PASS] 5. SmallPage (%u KB), PROT_NONE freed-memory guard & FreedMemoryTouched recovery verified\n",
            virtSize >> 10);

        Guest::Shutdown();
        printf("=== Gate 3 Self-Test: ALL VM ASSERTIONS PASSED ===\n");
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
        if (suite == "runtime" || suite == "render" || suite == "audio")
        {
            fprintf(stderr, "Self-test suite '%s' is not implemented yet.\n", suite.c_str());
            return 1;
        }
        if (suite != "vm" && suite != "all")
        {
            fprintf(stderr, "Unknown self-test suite '%s' (expected vm, runtime, render, audio, or all).\n",
                suite.c_str());
            return 1;
        }
        return 0;
    }
}
