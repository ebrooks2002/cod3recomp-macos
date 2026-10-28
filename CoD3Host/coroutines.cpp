#include "coroutines.h"
#include "kernel.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <map>

#ifdef _WIN32
#include <Windows.h>
#else
#include "platform.h"
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace
{
#ifndef _WIN32
    constexpr DWORD FIBER_FLAG_FLOAT_SWITCH = 0x1;
    using LPFIBER_START_ROUTINE = void (CALLBACK *)(void*);

    struct alignas(16) FiberContext
    {
        uint64_t x19, x20;
        uint64_t x21, x22;
        uint64_t x23, x24;
        uint64_t x25, x26;
        uint64_t x27, x28;
        uint64_t x29, x30; // fp, lr
        uint64_t sp, pad;
        double   d8,  d9;
        double   d10, d11;
        double   d12, d13;
        double   d14, d15;
    };

    static_assert(offsetof(FiberContext, x19) == 0x00, "x19 offset mismatch");
    static_assert(offsetof(FiberContext, x29) == 0x50, "x29 offset mismatch");
    static_assert(offsetof(FiberContext, sp)  == 0x60, "sp offset mismatch");
    static_assert(offsetof(FiberContext, d8)  == 0x70, "d8 offset mismatch");
    static_assert(offsetof(FiberContext, d14) == 0xA0, "d14 offset mismatch");

#if defined(__aarch64__)
    __attribute__((naked, noinline))
    void aarch64_switch_fiber(FiberContext* /*from*/, const FiberContext* /*to*/)
    {
        __asm__ volatile(
            "stp x19, x20, [x0, #0x00]\n"
            "stp x21, x22, [x0, #0x10]\n"
            "stp x23, x24, [x0, #0x20]\n"
            "stp x25, x26, [x0, #0x30]\n"
            "stp x27, x28, [x0, #0x40]\n"
            "stp x29, x30, [x0, #0x50]\n"
            "mov x2, sp\n"
            "str x2,       [x0, #0x60]\n"
            "stp d8,  d9,  [x0, #0x70]\n"
            "stp d10, d11, [x0, #0x80]\n"
            "stp d12, d13, [x0, #0x90]\n"
            "stp d14, d15, [x0, #0xA0]\n"

            "ldp x19, x20, [x1, #0x00]\n"
            "ldp x21, x22, [x1, #0x10]\n"
            "ldp x23, x24, [x1, #0x20]\n"
            "ldp x25, x26, [x1, #0x30]\n"
            "ldp x27, x28, [x1, #0x40]\n"
            "ldp x29, x30, [x1, #0x50]\n"
            "ldr x2,       [x1, #0x60]\n"
            "mov sp, x2\n"
            "ldp d8,  d9,  [x1, #0x70]\n"
            "ldp d10, d11, [x1, #0x80]\n"
            "ldp d12, d13, [x1, #0x90]\n"
            "ldp d14, d15, [x1, #0xA0]\n"
            "ret\n"
        );
    }

    __attribute__((naked, noinline))
    void aarch64_fiber_trampoline()
    {
        __asm__ volatile(
            "mov x29, #0\n"
            "mov x30, #0\n"
            "mov x0, x20\n"
            "blr x19\n"
            "brk #1\n"
        );
    }
#endif

    struct MacFiber
    {
        FiberContext context{};
        void* mapping = nullptr;
        size_t mappingSize = 0;
        bool isThreadFiber = false;
    };

    thread_local MacFiber* t_activeFiber = nullptr;

    void* GetCurrentFiber() noexcept
    {
        return t_activeFiber;
    }

    void* ConvertThreadToFiberEx(void*, DWORD) noexcept
    {
        if (t_activeFiber != nullptr)
            return nullptr;
        auto* fiber = new MacFiber();
        fiber->isThreadFiber = true;
        t_activeFiber = fiber;
        return fiber;
    }

    void* CreateFiberEx(size_t, size_t dwStackReserve, DWORD,
                        LPFIBER_START_ROUTINE lpStartAddress, void* lpParameter) noexcept
    {
        const size_t pageSize = static_cast<size_t>(getpagesize());
        const size_t stackSize = ((dwStackReserve > 0 ? dwStackReserve : (4u << 20)) + pageSize - 1) & ~(pageSize - 1);
        const size_t totalSize = stackSize + pageSize; // +1 guard page at low address

        void* mapping = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mapping == MAP_FAILED)
            return nullptr;

        if (mprotect(mapping, pageSize, PROT_NONE) != 0)
        {
            munmap(mapping, totalSize);
            return nullptr;
        }

        auto* fiber = new MacFiber();
        fiber->mapping = mapping;
        fiber->mappingSize = totalSize;

        const uintptr_t stackTop = reinterpret_cast<uintptr_t>(mapping) + totalSize;
        const uintptr_t initialSp = (stackTop - 16) & ~uintptr_t(0xF);
        auto* frameSentinel = reinterpret_cast<uint64_t*>(initialSp);
        frameSentinel[0] = 0;
        frameSentinel[1] = 0;

        fiber->context.x19 = reinterpret_cast<uint64_t>(lpStartAddress);
        fiber->context.x20 = reinterpret_cast<uint64_t>(lpParameter);
        fiber->context.x29 = 0;
        fiber->context.x30 = reinterpret_cast<uint64_t>(&aarch64_fiber_trampoline);
        fiber->context.sp  = initialSp;
        return fiber;
    }

    void SwitchToFiber(void* lpFiber) noexcept
    {
        auto* target = static_cast<MacFiber*>(lpFiber);
        MacFiber* current = t_activeFiber;
        if (!target || !current || target == current)
            return;
        t_activeFiber = target;
#if defined(__aarch64__)
        aarch64_switch_fiber(&current->context, &target->context);
#endif
    }

    void DeleteFiber(void* lpFiber) noexcept
    {
        auto* fiber = static_cast<MacFiber*>(lpFiber);
        if (!fiber) return;
        if (fiber->mapping != nullptr && fiber->mappingSize > 0)
            munmap(fiber->mapping, fiber->mappingSize);
        delete fiber;
    }
#endif
    struct Coroutine
    {
        uint32_t key = 0;
        void* fiber = nullptr;
        void* resumer = nullptr;         // the engine's fiber, while running
        PPCContext* ctx = nullptr;
        uint8_t* base = nullptr;
        PPCFunc* entry = nullptr;
        bool running = false;            // inside its entry function
        bool finished = false;
        uint32_t generation = 0;         // the game module's lifetime it began in

        // The longjmp the thread asked for when it yielded.
        bool jumpPending = false;
        jmp_buf* jumpBuffer = nullptr;
        int jumpValue = 0;
    };

    // Script threads run on the title's main thread only; the table and the
    // fiber state are that thread's.
    thread_local std::map<uint32_t, Coroutine*>* t_table = nullptr;
    thread_local Coroutine* t_current = nullptr;
    thread_local void* t_engineFiber = nullptr;

    constexpr size_t FiberReserve = 4u << 20;   // host stack, reserved
    constexpr size_t FiberCommit = 64u << 10;

    int g_announced = 0;

    // The game module's lifetimes, and what the threads did in the current one.
    uint32_t g_generation = 0;
    uint32_t g_begun = 0, g_resumed = 0, g_destroyed = 0, g_staleResumes = 0;

    // COD3_TRACECOROUTINES=1: every ten seconds, how many threads began,
    // resumed and were destroyed in them, and how many are suspended.
    uint32_t g_windowBegun = 0, g_windowResumed = 0, g_windowDestroyed = 0;
    void Window()
    {
        static const bool wanted = getenv("COD3_TRACECOROUTINES") != nullptr;
        if (!wanted) return;
        static uint64_t since = GetTickCount64();
        const uint64_t now = GetTickCount64();
        if (now - since < 10000) return;
        since = now;
        uint32_t suspended = 0;
        if (t_table != nullptr) for (const auto& entry : *t_table) if (entry.second->running) suspended++;
        printf("coroutines: in ten seconds %u begun, %u resumed, %u destroyed; %u suspended now, generation %u\n",
            g_windowBegun, g_windowResumed, g_windowDestroyed, suspended, g_generation);
        fflush(stdout);
        g_windowBegun = g_windowResumed = g_windowDestroyed = 0;
    }

    void CALLBACK Start(void* argument)
    {
        Coroutine* coroutine = static_cast<Coroutine*>(argument);
        coroutine->running = true;
        coroutine->entry(*coroutine->ctx, coroutine->base);
        // The start function returned: the thread ran to its end without
        // yielding, or came back from its last yield and finished. Either
        // way the engine continues after the call that started it.
        coroutine->running = false;
        coroutine->finished = true;
        SwitchToFiber(coroutine->resumer);
    }

    void EnsureFiberThread()
    {
        if (t_engineFiber != nullptr) return;
        t_engineFiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
        if (t_engineFiber == nullptr)
        {
            // Already a fiber: the current one is the engine's.
            t_engineFiber = GetCurrentFiber();
        }
        t_table = new std::map<uint32_t, Coroutine*>();
    }

    // Switches to the thread's fiber and, once it yields or finishes, does
    // the longjmp it asked for, if any.
    void Switch(Coroutine& coroutine)
    {
        coroutine.resumer = GetCurrentFiber();
        Coroutine* outer = t_current;
        t_current = &coroutine;
        SwitchToFiber(coroutine.fiber);
        t_current = outer;

        if (g_announced < 60)
        {
            g_announced++;
            printf("coroutines: thread 0x%08X %s%s r3 0x%08X r1 0x%08X\n", coroutine.key,
                coroutine.finished ? "finished" : "yielded",
                coroutine.jumpPending ? " with a longjmp" : "",
                coroutine.ctx->r3.u32, coroutine.ctx->r1.u32);
            fflush(stdout);
        }
        if (coroutine.jumpPending)
        {
            coroutine.jumpPending = false;
            jmp_buf* buffer = coroutine.jumpBuffer;
            const int value = coroutine.jumpValue;
            longjmp(*buffer, value);
        }
    }
}

void Coroutines::Begin(PPCContext& ctx, uint8_t* base, uint32_t key, PPCFunc* entry)
{
    EnsureFiberThread();
    Coroutine*& slot = (*t_table)[key];
    if (slot == nullptr) slot = new Coroutine();
    Coroutine& coroutine = *slot;
    if (coroutine.fiber != nullptr)
    {
        if (coroutine.running)
        {
            printf("coroutines: thread 0x%08X is started again while suspended; "
                   "its old fiber is abandoned\n", key);
            fflush(stdout);
        }
        DeleteFiber(coroutine.fiber);
        coroutine.fiber = nullptr;
    }
    coroutine.key = key;
    coroutine.ctx = &ctx;
    coroutine.base = base;
    coroutine.entry = entry;
    coroutine.running = false;
    coroutine.finished = false;
    coroutine.jumpPending = false;
    coroutine.generation = g_generation;
    g_begun++;
    g_windowBegun++;
    Window();
    coroutine.fiber = CreateFiberEx(FiberCommit, FiberReserve, FIBER_FLAG_FLOAT_SWITCH,
                                    Start, &coroutine);
    if (g_announced++ < 12)
    {
        printf("coroutines: thread 0x%08X begins\n", key);
        fflush(stdout);
    }
    if (coroutine.fiber == nullptr)
    {
        printf("coroutines: no fiber for thread 0x%08X (error %lu); running it inline\n",
            key, GetLastError());
        fflush(stdout);
        entry(ctx, base);
        return;
    }
    Switch(coroutine);
}

void Coroutines::Resume(PPCContext& ctx, uint8_t* base, uint32_t key)
{
    EnsureFiberThread();
    auto found = t_table->find(key);
    Coroutine* coroutine = found != t_table->end() ? found->second : nullptr;
    if (coroutine == nullptr || coroutine->fiber == nullptr || !coroutine->running)
    {
        // Nothing suspended under this key. A thread that was never begun
        // through Begin, or one that finished, has no fiber to continue;
        // the engine's continuation address says where it wanted to go.
        printf("coroutines: thread 0x%08X resumed at 0x%08X but nothing is suspended for it\n",
            key, uint32_t(ctx.lr));
        fflush(stdout);
        return;
    }
    coroutine->ctx = &ctx;
    coroutine->base = base;
    g_resumed++;
    g_windowResumed++;
    Window();
    if (coroutine->generation != g_generation && g_staleResumes++ < 20)
    {
        printf("coroutines: thread 0x%08X resumed on a fiber begun in generation %u, now %u: its host frames are from before the restart\n",
            key, coroutine->generation, g_generation);
        fflush(stdout);
    }
    if (g_announced < 60)
    {
        g_announced++;
        printf("coroutines: thread 0x%08X resumes at 0x%08X, r1 0x%08X\n", key, uint32_t(ctx.lr), ctx.r1.u32);
        fflush(stdout);
    }
    Switch(*coroutine);
}

bool Coroutines::YieldIfCoroutine(jmp_buf& buffer, int value)
{
    Coroutine* coroutine = t_current;
    if (coroutine == nullptr) return false;
    coroutine->jumpPending = true;
    coroutine->jumpBuffer = &buffer;
    coroutine->jumpValue = value;
    SwitchToFiber(coroutine->resumer);
    // Resumed: the engine has loaded this thread's registers and switched
    // back; t_current was set by Switch.
    return true;
}

bool Coroutines::Finished(uint32_t key)
{
    if (t_table == nullptr) return false;
    auto found = t_table->find(key);
    return found != t_table->end() && found->second->finished;
}

void Coroutines::Destroy(uint32_t key)
{
    if (t_table == nullptr) return;
    auto found = t_table->find(key);
    if (found == t_table->end()) return;
    Coroutine* coroutine = found->second;
    t_table->erase(found);
    g_destroyed++;
    g_windowDestroyed++;
    if (coroutine == t_current)
    {
        // Destroying oneself: the fiber cannot be deleted from inside.
        // It is left to leak, which a level does a handful of times.
        return;
    }
    if (coroutine->fiber != nullptr) DeleteFiber(coroutine->fiber);
    delete coroutine;
}

bool Coroutines::Inside()
{
    return t_current != nullptr;
}

void Coroutines::NewGeneration()
{
    uint32_t suspended = 0, finished = 0;
    if (t_table != nullptr)
        for (const auto& entry : *t_table)
        {
            if (entry.second->running) suspended++;
            else if (entry.second->finished) finished++;
        }
    printf("coroutines: generation %u ends: %u threads begun, %u resumes, %u destroyed, %u fibers still suspended, %u finished and kept, %u stale resumes so far\n",
        g_generation, g_begun, g_resumed, g_destroyed, suspended, finished, g_staleResumes);
    fflush(stdout);
    g_generation++;
    g_begun = g_resumed = g_destroyed = 0;
}
