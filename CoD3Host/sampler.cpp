// Finds out where the guest is, when it is not asking the runtime for anything.
//
// A recompiled title that stops making progress without blocking on a kernel
// call is spinning somewhere in its own code, and from outside that is
// invisible. This suspends the guest threads, walks their stacks, and names the
// guest function each frame belongs to.
//
// Naming them needs no debug symbols. Every recompiled function is a real
// function in this binary, and PPCFuncMappings already pairs each one with the
// guest address it came from. Inverting that table turns a host return address
// into `sub_82XXXXXX`, which can be looked up in the disassembly.

#include "kernel.h"
#include "sampler.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <map>
#include <utility>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <TlHelp32.h>
#else
#include "platform.h"
#include <dlfcn.h>
#include <libunwind.h>
#endif

namespace
{
    struct HostFunction
    {
        uintptr_t host;
        uint32_t guest;
    };

    std::vector<HostFunction> g_functions;   // sorted by host address
    uintptr_t g_recompCodeStart = UINTPTR_MAX;
    std::atomic<bool> g_running{ false };
    std::thread g_thread;

    void BuildFunctionMap()
    {
        if (!g_functions.empty()) return;

        for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
        {
            const uintptr_t hostAddr  = reinterpret_cast<uintptr_t>(PPCFuncMappings[i].host);
            const uint32_t  guestAddr = static_cast<uint32_t>(PPCFuncMappings[i].guest);
            g_functions.push_back({ hostAddr, guestAddr });

#if defined(__aarch64__)
            // In CoD3RecompLib, PPC_WEAK_FUNC(sub_82XXXXXX) is emitted right after
            // PPC_FUNC_IMPL(__imp__sub_82XXXXXX) and compiles to a single `b __imp__sub_82XXXXXX`
            // instruction at offset 0. Register `__imp__sub_82XXXXXX` as well so return addresses
            // inside `__imp__sub_82XXXXXX` resolve to `sub_82XXXXXX`.
            if (guestAddr < 0x8257835Cu)
            {
                const uint32_t insn0 = *reinterpret_cast<const uint32_t*>(hostAddr);
                if ((insn0 & 0xFC000000u) == 0x14000000u) // unconditional b imm26
                {
                    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(insn0 << 6)) >> 4;
                    const uintptr_t target = static_cast<uintptr_t>(static_cast<int64_t>(hostAddr) + offset);
                    g_functions.push_back({ target, guestAddr });
                    if (target < g_recompCodeStart)
                        g_recompCodeStart = target;
                }
            }
#endif
        }
        std::sort(g_functions.begin(), g_functions.end(),
            [](const HostFunction& a, const HostFunction& b) {
                return a.host != b.host ? (a.host < b.host) : (a.guest < b.guest);
            });
        g_functions.erase(
            std::unique(g_functions.begin(), g_functions.end(),
                [](const HostFunction& a, const HostFunction& b) { return a.host == b.host; }),
            g_functions.end());
    }

    // The guest function a host address falls inside, or zero when the address
    // belongs to the runtime rather than to translated code.
    uint32_t GuestFunctionAt(uintptr_t address)
    {
        if (g_functions.empty()) return 0;

        auto it = std::upper_bound(g_functions.begin(), g_functions.end(), address,
            [](uintptr_t value, const HostFunction& f) { return value < f.host; });
        if (it == g_functions.begin()) return 0;
        --it;

        // Recompiled functions are large but not unbounded. A hit far past a
        // function start is host code that happens to sit after it.
        if (address - it->host > (1u << 20)) return 0;

#ifndef _WIN32
        if (it->host < g_recompCodeStart && (address - it->host) > 0x1000u)
            return 0;

        if (it->host < g_recompCodeStart ||
            (it + 1) == g_functions.end() ||
            ((it + 1)->host - it->host) > 0x4000u)
        {
            Dl_info info{};
            if (dladdr(reinterpret_cast<const void*>(address), &info) != 0 && info.dli_saddr != nullptr)
            {
                const uintptr_t symStart = reinterpret_cast<uintptr_t>(info.dli_saddr);
                if (symStart != it->host)
                    return 0;
            }
        }
#endif
        return it->guest;
    }

    // Unwinds without debug symbols, using the exception tables the linker
    // already puts in the image.
    //
    // The thread is suspended only for as long as it takes to copy its
    // registers, and is running again before the unwind starts. The unwind
    // looks up function entries, and that lookup takes a lock inside ntdll
    // which the suspended thread can be holding, in the middle of an
    // exception dispatch of its own: the sampler then waits for a thread
    // that cannot run until the sampler lets it, and the whole runtime
    // stops behind the pair of them. Walking a copy of the registers while
    // the thread runs on reads a stack that may be changing under it, which
    // costs at worst one wrong sample, and never a deadlock.
    // The unwind proper, in a function of its own with no C++ objects so it
    // can sit inside a structured exception handler: a stale register from
    // a thread that has moved on can point anywhere.
    void UnwindFrames(CONTEXT* context, uint32_t* frames, int& count, int limit)
    {
#ifdef _WIN32
        __try
        {
            for (int depth = 0; depth < 64 && count < limit; depth++)
            {
                const uint32_t guest = GuestFunctionAt(static_cast<uintptr_t>(context->Rip));
                if (guest != 0)
                {
                    // Collapse the long runs of one function calling itself
                    // that a deep guest call chain produces.
                    if (count == 0 || frames[count - 1] != guest) frames[count++] = guest;
                }

                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context->Rip, &imageBase, nullptr);
                if (entry == nullptr) break;

                void* handlerData = nullptr;
                DWORD64 establisherFrame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context->Rip, entry,
                    context, &handlerData, &establisherFrame, nullptr);
                if (context->Rip == 0) break;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // The snapshot was stale; what was collected before it went
            // wrong is still a sample.
        }
#else
        if (!context || !frames || limit <= 0)
            return;

        constexpr uintptr_t PtrMask = 0x0000FFFFFFFFFFFFULL;
        auto recordAddress = [&](uintptr_t addr) {
            if (addr == 0 || count >= limit) return;
            const uint32_t guest = GuestFunctionAt(addr & PtrMask);
            if (guest != 0 && (count == 0 || frames[count - 1] != guest))
                frames[count++] = guest;
        };

        recordAddress(static_cast<uintptr_t>(context->Rip));
        if (context->Lr > 4)
            recordAddress(static_cast<uintptr_t>(context->Lr - 4));

        uintptr_t fp = static_cast<uintptr_t>(context->Rbp) & PtrMask;
        for (int depth = 0; depth < 64 && count < limit; depth++)
        {
            if (fp == 0 || (fp & 0x7) != 0)
                break;

            uint64_t frameRecord[2] = {};
            if (!Platform::SafeReadMemory(reinterpret_cast<const void*>(fp), frameRecord, sizeof(frameRecord)))
                break;

            const uintptr_t nextFp = static_cast<uintptr_t>(frameRecord[0]) & PtrMask;
            const uintptr_t savedLr = static_cast<uintptr_t>(frameRecord[1]) & PtrMask;
            if (savedLr <= 4)
                break;

            recordAddress(savedLr - 4);

            if (nextFp <= fp || (nextFp - fp) > (16u << 20))
                break;
            fp = nextFp;
        }
#endif
    }

    void WalkStack(HANDLE thread, uint32_t* frames, int& count, int limit)
    {
        count = 0;

        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (SuspendThread(thread) == DWORD(-1)) return;
        const BOOL captured = GetThreadContext(thread, &context);
        ResumeThread(thread);
        if (!captured) return;

        Kernel::SetUnwinding(true);
        UnwindFrames(&context, frames, count, limit);
        Kernel::SetUnwinding(false);
    }

    void SampleOnce()
    {
        std::vector<Kernel::ThreadSample> threads = Kernel::SampleGuestThreads();
        if (threads.empty()) return;

        // Three samples a fifth of a second apart. A thread doing work shows a
        // different stack each time; a stuck one shows the same stack, and that
        // difference is the whole question when nothing is progressing.
        // One stack tells you where a thread is. A histogram of many tells you
        // whether it is going anywhere. A thread doing work spreads across its
        // call graph; a thread stuck in a loop piles up on a few addresses, and
        // that difference is the whole question when nothing is progressing.
        constexpr int Samples = 40;

        printf("\n");
        printf("guest threads, %d samples each, innermost frame:\n", Samples);

        for (const auto& thread : threads)
        {
            if (thread.handle == nullptr) continue;

            std::map<uint32_t, int> innermost;
            std::map<uint32_t, int> callers;
            int outside = 0;
            uint32_t deepest[10] = {};
            int deepestCount = 0;

            for (int sample = 0; sample < Samples; sample++)
            {
                uint32_t frames[10] = {};
                int count = 0;

                WalkStack(thread.handle, frames, count, 10);

                if (count == 0) { outside++; }
                else
                {
                    innermost[frames[0]]++;
                    if (count > 1) callers[frames[1]]++;
                    if (count > deepestCount)
                    {
                        deepestCount = count;
                        for (int i = 0; i < count; i++) deepest[i] = frames[i];
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }

            printf("  thread %u (os %u):", thread.id, thread.osId);
            if (const PPCContext* context = Kernel::ContextOf(thread.osId))
                printf(" r1 %08X lr %08X", context->r1.u32, uint32_t(context->lr));
            if (innermost.empty())
            {
                printf(" not in recompiled code\n");
                continue;
            }
            if (outside > 0) printf(" outside x%d", outside);

            // Busiest first, so a loop shows up at the front.
            std::vector<std::pair<int, uint32_t>> ordered;
            for (const auto& entry : innermost)
                ordered.push_back({ entry.second, entry.first });
            std::sort(ordered.begin(), ordered.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });

            for (size_t i = 0; i < ordered.size() && i < 5; i++)
                printf(" sub_%08X x%d", ordered[i].second, ordered[i].first);
            printf(" (%zu distinct)\n", innermost.size());

            if (deepestCount > 1)
            {
                printf("      stack:");
                for (int i = 0; i < deepestCount; i++) printf(" sub_%08X", deepest[i]);
                printf("\n");
            }

            // The guest's own call chain, from its registers: the link
            // register is the return into the function that made the call
            // this thread is in, and every frame above it keeps its caller's
            // return address eight bytes below the back chain pointer it
            // stored, the way the title's prologues save it. This walks
            // through the host code the histogram cannot see.
            if (const PPCContext* context = Kernel::ContextOf(thread.osId))
            {
                // The argument and scratch registers too: what a loop that
                // never ends is looping over.
                printf("      r3-r11:");
                for (int r = 3; r <= 11; r++) printf(" %08X", Kernel::Register(*context, r));
                printf("  r28-r31:");
                for (int r = 28; r <= 31; r++) printf(" %08X", Kernel::Register(*context, r));
                printf("\n");
                uint32_t frame = context->r1.u32;
                uint32_t address = uint32_t(context->lr);
                if (frame >= 0x10000 && frame < 0xC0000000u && address != 0)
                {
                    printf("      guest stack: %08X", address);
                    for (int depth = 0; depth < 14; depth++)
                    {
                        const uint32_t caller = Guest::Read32(Guest::Base, frame);
                        if (caller <= frame || caller - frame > 0x100000 || (caller & 7) != 0 ||
                            caller >= 0xC0000000u)
                            break;
                        address = Guest::Read32(Guest::Base, caller - 8);
                        if (address < 0x82000000u || address >= 0x8A000000u) break;
                        printf(" %08X", address);
                        frame = caller;
                    }
                    printf("\n");
                }
            }
        }
        Kernel::ReportRecentCalls();
        fflush(stdout);
    }
}

bool Sampler::ThreadTimesWanted()
{
    static const bool wanted = getenv("COD3_THREADTIME") != nullptr;
    return wanted;
}

void Sampler::ReportThreadTimes(const std::vector<std::pair<uint32_t, const char*>>& named)
{
    static std::map<DWORD, uint64_t> previous;   // os id: CPU time, in 100 ns
    static auto previousWall = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double wall = std::chrono::duration<double>(now - previousWall).count();
    previousWall = now;

    std::map<uint32_t, Kernel::ThreadSample> guest;
    for (const Kernel::ThreadSample& thread : Kernel::SampleGuestThreads()) guest[thread.osId] = thread;

    struct Row { double percent; DWORD os; };
    std::vector<Row> rows;
    std::map<DWORD, uint64_t> current;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD process = GetCurrentProcessId();
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != process) continue;
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (thread == nullptr) continue;
        FILETIME created, exited, kernel, user;
        if (GetThreadTimes(thread, &created, &exited, &kernel, &user))
        {
            const uint64_t cpu = (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                                 (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
            current[entry.th32ThreadID] = cpu;
            auto before = previous.find(entry.th32ThreadID);
            const uint64_t used = before != previous.end() ? cpu - before->second : 0;
            if (wall > 0 && before != previous.end()) rows.push_back({ double(used) * 1e-7 / wall * 100.0, entry.th32ThreadID });
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    previous.swap(current);
    if (rows.empty()) return;

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.percent > b.percent; });
    double total = 0;
    for (const Row& row : rows) total += row.percent;
    printf("threads, CPU over %.1f s (%.0f%% of one core in all):", wall, total);
    for (size_t i = 0; i < rows.size() && i < 12 && rows[i].percent >= 1.0; i++)
    {
        const Row& row = rows[i];
        const char* name = nullptr;
        for (const auto& [os, text] : named)
            if (os == row.os) name = text;
        auto found = guest.find(row.os);
        if (name != nullptr) printf(" %s %.0f%%,", name, row.percent);
        else if (found != guest.end()) printf(" guest %u (%08X) %.0f%%,", found->second.id, found->second.startAddress, row.percent);
        else printf(" host %lu %.0f%%,", (unsigned long)row.os, row.percent);
    }
    printf("\n");
    fflush(stdout);
}

void Sampler::Start(int intervalSeconds)
{
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) return;

    BuildFunctionMap();
    g_thread = std::thread([intervalSeconds]()
    {
        // One report after things have settled, then at the requested interval.
        std::this_thread::sleep_for(std::chrono::seconds(8));
        while (g_running.load(std::memory_order_acquire))
        {
            SampleOnce();
            for (int i = 0; i < intervalSeconds && g_running.load(); i++)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
}

void Sampler::Stop()
{
    if (g_running.exchange(false) && g_thread.joinable())
        g_thread.join();
}

uint32_t Sampler::FunctionAt(unsigned long long hostAddress)
{
    BuildFunctionMap();
    return GuestFunctionAt(static_cast<uintptr_t>(hostAddress));
}

int Sampler::WalkGuestStack(void* winContext, uint32_t* functions, int limit)
{
    if (!winContext || !functions || limit <= 0)
        return 0;

    BuildFunctionMap();

    // The handler's context is the state at the fault, so it is walked in
    // place rather than copied: nothing else is going to run on this stack.
    CONTEXT context = *static_cast<CONTEXT*>(winContext);
    int count = 0;

#ifdef _WIN32
    for (int depth = 0; depth < 64 && count < limit; depth++)
    {
        const uint32_t guest = GuestFunctionAt(static_cast<uintptr_t>(context.Rip));
        if (guest != 0 && (count == 0 || functions[count - 1] != guest))
            functions[count++] = guest;

        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (entry == nullptr) break;

        void* handlerData = nullptr;
        DWORD64 establisherFrame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, entry,
            &context, &handlerData, &establisherFrame, nullptr);
        if (context.Rip == 0) break;
    }
#else
    UnwindFrames(&context, functions, count, limit);
#endif
    return count;
}

int Sampler::FunctionsOnStack(uint32_t* functions, int limit)
{
    if (!functions || limit <= 0)
        return 0;

#ifndef _WIN32
    BuildFunctionMap();
    unw_context_t uc{};
    unw_cursor_t cursor{};
    if (unw_getcontext(&uc) == 0 && unw_init_local(&cursor, &uc) == 0)
    {
        constexpr uintptr_t PtrMask = 0x0000FFFFFFFFFFFFULL;
        int count = 0;
        for (int depth = 0; depth < 64 && count < limit; ++depth)
        {
            unw_word_t ip = 0;
            if (unw_get_reg(&cursor, UNW_REG_IP, &ip) == 0 && ip > 4)
            {
                const uintptr_t lookupAddr = (depth == 0)
                    ? (static_cast<uintptr_t>(ip) & PtrMask)
                    : ((static_cast<uintptr_t>(ip) & PtrMask) - 4);
                const uint32_t guest = GuestFunctionAt(lookupAddr);
                if (guest != 0 && (count == 0 || functions[count - 1] != guest))
                    functions[count++] = guest;
            }
            if (unw_step(&cursor) <= 0)
                break;
        }
        if (count > 0)
            return count;
    }
#endif

    CONTEXT context{};
    RtlCaptureContext(&context);
    return WalkGuestStack(&context, functions, limit);
}

void Sampler::SampleNow()
{
    // The same report the timer prints, on demand: what every guest thread is
    // doing at this exact moment, which is the question the moment something
    // in memory has just been found overwritten.
    SampleOnce();
}
