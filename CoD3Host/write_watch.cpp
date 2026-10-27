#include "write_watch.h"
#include "kernel.h"
#include "settings.h"

#include <atomic>
#include <cstdlib>
#include <mutex>

#include "platform.h"
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace
{
#ifdef __APPLE__
    constexpr uint32_t PageShift = 14;                      // 16 KB hardware pages on Apple Silicon
#else
    constexpr uint32_t PageShift = 12;
#endif
    constexpr uint32_t WindowSize = 0x20000000u;           // 512 MB of physical memory
    constexpr uint32_t Pages = WindowSize >> PageShift;
    constexpr uint32_t Windows[] = { 0xA0000000u, 0xC0000000u, 0xE0000000u };

    std::atomic<uint8_t> g_armed[Pages];
    std::atomic<uint64_t> g_written[Pages];   // the sequence the page was last noted written at
    std::atomic<uint64_t> g_sequence{ 1 };
    std::mutex g_lock;                        // arming and Forget
    std::atomic<bool> g_enabledInitialized{ false };
    std::atomic<bool> g_enabledValue{ false };

    bool Protect(uint32_t firstPage, uint32_t count, DWORD protection)
    {
        bool all = true;
        for (const uint32_t window : Windows)
        {
#ifdef _WIN32
            DWORD was = 0;
            DWORD* oldProt = &was;
#else
            DWORD* oldProt = nullptr;
#endif
            if (!VirtualProtect(Guest::Base + window + (size_t(firstPage) << PageShift),
                                size_t(count) << PageShift, protection, oldProt))
                all = false;
        }
        return all;
    }

    // Disarm a single page lock-free (async-signal-safe).
    bool DisarmPageLockFree(uint32_t page) noexcept
    {
        const uint8_t prev = g_armed[page].exchange(0, std::memory_order_acq_rel);
        const uint64_t now = g_sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
        g_written[page].store(now, std::memory_order_release);
        if (prev != 0)
        {
            Protect(page, 1, PAGE_READWRITE);
            return true;
        }
        return false;
    }

    // The armed pages of [first, end) disarmed and noted written now
    // (g_lock held), a run of them at a time.
    void Disarm(uint32_t first, uint32_t end)
    {
        uint32_t page = first;
        while (page < end)
        {
            if (g_armed[page].load(std::memory_order_acquire) == 0) { page++; continue; }
            uint32_t last = page;
            while (last < end && g_armed[last].load(std::memory_order_acquire) != 0) last++;
            const uint64_t now = g_sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
            for (uint32_t p = page; p < last; p++)
            {
                g_written[p].store(now, std::memory_order_release);
                g_armed[p].store(0, std::memory_order_release);
            }
            Protect(page, last - page, PAGE_READWRITE);
            page = last;
        }
    }

    // The section's pages a guest address range covers, through any window
    // (or a physical address below them). False when it lies outside.
    bool PagesOf(uint32_t address, uint32_t bytes, uint32_t& first, uint32_t& end)
    {
        if (bytes == 0) return false;
        uint32_t offset = address;
        if (address >= Windows[0]) offset = address & (WindowSize - 1);
        else if (address >= WindowSize) return false;   // the virtual heap and below: not physical memory
        const uint64_t last = uint64_t(offset) + bytes - 1;
        if (last >= WindowSize) return false;
        first = offset >> PageShift;
        end = uint32_t(last >> PageShift) + 1;
        return true;
    }
}

// COD3_WRITEWATCH=0|1, else the settings' "Watch memory writes". Read once,
// after the settings are loaded (the first caller is the guest's own start).
bool WriteWatch::Enabled()
{
    if (!g_enabledInitialized.load(std::memory_order_acquire))
    {
        const char* t = getenv("COD3_WRITEWATCH");
        const bool val = (t != nullptr && t[0] != 0) ? (t[0] != '0') : Settings::Get().watchWrites;
        g_enabledValue.store(val, std::memory_order_relaxed);
        g_enabledInitialized.store(true, std::memory_order_release);
    }
    return g_enabledValue.load(std::memory_order_relaxed);
}

uint64_t WriteWatch::Sequence() { return g_sequence.load(std::memory_order_acquire); }

bool WriteWatch::Arm(uint32_t physical, uint32_t bytes)
{
    if (!Enabled()) return false;
    uint32_t first, end;
    if (!PagesOf(physical & (WindowSize - 1), bytes, first, end)) return false;
    std::lock_guard<std::mutex> lock(g_lock);
    uint32_t page = first;
    while (page < end)
    {
        if (g_armed[page].load(std::memory_order_acquire) != 0) { page++; continue; }
        uint32_t last = page;
        while (last < end && g_armed[last].load(std::memory_order_acquire) == 0) last++;
        const uint64_t seqBeforeProtect = g_sequence.load(std::memory_order_acquire);
        // Armed before they are protected: a write that faults in between
        // disarms the page lock-free and bumps g_sequence.
        for (uint32_t p = page; p < last; p++) g_armed[p].store(1, std::memory_order_release);
        if (!Protect(page, last - page, PAGE_READONLY))
        {
            const uint64_t now = g_sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
            for (uint32_t p = page; p < last; p++)
            {
                g_written[p].store(now, std::memory_order_release);
                g_armed[p].store(0, std::memory_order_release);
            }
            Protect(page, last - page, PAGE_READWRITE);
            return false;
        }
        // If a signal handler disarmed a page while Protect(..., PAGE_READONLY)
        // was still in flight, restore PAGE_READWRITE for that page so the
        // late read-only mprotect does not leave a disarmed page read-only.
        for (uint32_t p = page; p < last; p++)
        {
            if (g_armed[p].load(std::memory_order_acquire) == 0 ||
                g_written[p].load(std::memory_order_acquire) > seqBeforeProtect)
            {
                g_armed[p].store(0, std::memory_order_release);
                Protect(p, 1, PAGE_READWRITE);
            }
        }
        page = last;
    }
    return true;
}

bool WriteWatch::Clean(uint32_t physical, uint32_t bytes, uint64_t since)
{
    uint32_t first, end;
    if (!PagesOf(physical & (WindowSize - 1), bytes, first, end)) return false;
    for (uint32_t page = first; page < end; page++)
        if (g_armed[page].load(std::memory_order_acquire) == 0 || g_written[page].load(std::memory_order_acquire) > since)
            return false;
    return true;
}

void WriteWatch::Forget(uint32_t guestAddress, uint32_t bytes)
{
    if (!Enabled()) return;
    uint32_t first, end;
    if (!PagesOf(guestAddress, bytes, first, end)) return;
    std::lock_guard<std::mutex> lock(g_lock);
    Disarm(first, end);
}

bool WriteWatch::HandleFault(const void* hostAddress)
{
    if (Guest::Base == nullptr || hostAddress < Guest::Base ||
        !g_enabledInitialized.load(std::memory_order_acquire) ||
        !g_enabledValue.load(std::memory_order_relaxed))
    {
        return false;
    }
    const uintptr_t at = uintptr_t(hostAddress) - uintptr_t(Guest::Base);
    uint32_t page = Pages;
    for (const uint32_t window : Windows)
        if (at >= window && at < uint64_t(window) + WindowSize) page = uint32_t((at - window) >> PageShift);
    if (page >= Pages) return false;
    if (g_armed[page].load(std::memory_order_acquire) != 0)
    {
        if (DisarmPageLockFree(page))
            return true;
    }
    // Not armed: another thread disarmed it a moment ago, and the write goes
    // through when it is made again; or the page is protected for another
    // reason (freed memory), which is the other handlers' business.
#ifdef __APPLE__
    mach_vm_address_t regionAddr = reinterpret_cast<mach_vm_address_t>(hostAddress) & ~mach_vm_address_t((1u << PageShift) - 1);
    mach_vm_size_t regionSize = 0;
    vm_region_basic_info_data_64_t regionInfo{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t objectName = MACH_PORT_NULL;
    const kern_return_t kr = mach_vm_region(
        mach_task_self(), &regionAddr, &regionSize, VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&regionInfo), &count, &objectName);
    if (objectName != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), objectName);
    if (kr != KERN_SUCCESS || regionAddr > reinterpret_cast<mach_vm_address_t>(hostAddress))
        return false;
    return (regionInfo.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE);
#else
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(hostAddress, &info, sizeof(info)) == 0) return false;
    return info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE;
#endif
}
