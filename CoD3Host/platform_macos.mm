#include "platform.h"

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#include <crt_externs.h>
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <spawn.h>
#include <sys/stat.h>

#include <atomic>
#include <condition_variable>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>

namespace
{
    std::mutex g_allocMutex;
    // base -> size of every region VirtualAlloc reserved on its own.
    std::map<uintptr_t, size_t> g_virtualAllocations;
    // Committed host-page ranges [start, end), merged. Commitment is tracked
    // here rather than inferred from PROT_NONE, because PAGE_NOACCESS memory
    // can be committed and a reservation is not the same as a revoked page.
    std::map<uintptr_t, uintptr_t> g_committed;

    // Lock-free, async-signal-safe commitment bitmap for the 4 GB guest address
    // space reservation [g_externalBase, g_externalBase + g_externalSize).
    // 1 bit per 4 KB page across up to 4 GB = 1,048,576 bits = 16,384 uint64_t words (128 KB).
    constexpr size_t ExternalPageShift = 12;
    constexpr size_t ExternalMaxPages  = 0x100000000ULL >> ExternalPageShift;
    constexpr size_t ExternalWords     = ExternalMaxPages / 64;
    std::atomic<uintptr_t> g_externalBase{ 0 };
    std::atomic<size_t>    g_externalSize{ 0 };
    std::atomic<uint64_t>  g_externalCommitted[ExternalWords];

    bool InExternalReservation(uintptr_t address, uintptr_t& extBase, size_t& extSize) noexcept
    {
        extBase = g_externalBase.load(std::memory_order_acquire);
        extSize = g_externalSize.load(std::memory_order_acquire);
        return extBase != 0 && address >= extBase && (address - extBase) < extSize;
    }

    bool IsExternalPageCommitted(uintptr_t extBase, uintptr_t address) noexcept
    {
        const size_t page = (address - extBase) >> ExternalPageShift;
        if (page >= ExternalMaxPages) return false;
        const uint64_t word = g_externalCommitted[page >> 6].load(std::memory_order_acquire);
        return (word & (1ULL << (page & 63))) != 0;
    }

    bool IsExternalRangeCommitted(uintptr_t extBase, size_t extSize, uintptr_t start, uintptr_t end) noexcept
    {
        if (end <= start || start < extBase || (end - extBase) > extSize) return false;
        const size_t firstPage = (start - extBase) >> ExternalPageShift;
        const size_t lastPage  = (end - extBase) >> ExternalPageShift;
        if (lastPage > ExternalMaxPages) return false;
        size_t page = firstPage;
        while (page < lastPage)
        {
            const size_t wordIdx = page >> 6;
            const size_t bitOff  = page & 63;
            const size_t rem     = lastPage - page;
            const size_t bits    = (bitOff + rem <= 64) ? rem : (64 - bitOff);
            const uint64_t mask  = (bits == 64) ? ~uint64_t(0) : (((1ULL << bits) - 1ULL) << bitOff);
            if ((g_externalCommitted[wordIdx].load(std::memory_order_acquire) & mask) != mask)
                return false;
            page += bits;
        }
        return true;
    }

    size_t SetExternalCommitted(uintptr_t extBase, size_t extSize, uintptr_t start, uintptr_t end, bool commit) noexcept
    {
        if (end <= start || start < extBase) return 0;
        const size_t firstPage = (start - extBase) >> ExternalPageShift;
        size_t lastPage = (end - extBase) >> ExternalPageShift;
        const size_t maxPages = (extSize >> ExternalPageShift) < ExternalMaxPages
            ? (extSize >> ExternalPageShift)
            : ExternalMaxPages;
        if (lastPage > maxPages) lastPage = maxPages;
        size_t newlyTransitionedPages = 0;
        size_t page = firstPage;
        while (page < lastPage)
        {
            const size_t wordIdx = page >> 6;
            const size_t bitOff  = page & 63;
            const size_t rem     = lastPage - page;
            const size_t bits    = (bitOff + rem <= 64) ? rem : (64 - bitOff);
            const uint64_t mask  = (bits == 64) ? ~uint64_t(0) : (((1ULL << bits) - 1ULL) << bitOff);
            if (commit)
            {
                const uint64_t prev = g_externalCommitted[wordIdx].fetch_or(mask, std::memory_order_acq_rel);
                newlyTransitionedPages += static_cast<size_t>(__builtin_popcountll((~prev) & mask));
            }
            else
            {
                const uint64_t prev = g_externalCommitted[wordIdx].fetch_and(~mask, std::memory_order_acq_rel);
                newlyTransitionedPages += static_cast<size_t>(__builtin_popcountll(prev & mask));
            }
            page += bits;
        }
        return newlyTransitionedPages << ExternalPageShift;
    }

    uintptr_t ExternalStateRunEnd(uintptr_t extBase, size_t extSize, uintptr_t address, uintptr_t limit) noexcept
    {
        const uintptr_t extEnd = extBase + extSize;
        if (limit > extEnd) limit = extEnd;
        const bool targetState = IsExternalPageCommitted(extBase, address);
        const size_t pageSize = 1ULL << ExternalPageShift;
        uintptr_t cur = (address & ~(pageSize - 1)) + pageSize;
        while (cur < limit)
        {
            const size_t page = (cur - extBase) >> ExternalPageShift;
            if ((page & 63) == 0 && cur + (64 * pageSize) <= limit)
            {
                const uint64_t word = g_externalCommitted[page >> 6].load(std::memory_order_acquire);
                if (word == (targetState ? ~uint64_t(0) : uint64_t(0)))
                {
                    cur += 64 * pageSize;
                    continue;
                }
            }
            if (IsExternalPageCommitted(extBase, cur) != targetState)
                return cur;
            cur += pageSize;
        }
        return limit;
    }

    // g_allocMutex held for all of these.
    bool FindAllocationLocked(uintptr_t address, uintptr_t& base, size_t& size)
    {
        auto it = g_virtualAllocations.upper_bound(address);
        if (it == g_virtualAllocations.begin()) return false;
        --it;
        if (address >= it->first + it->second) return false;
        base = it->first;
        size = it->second;
        return true;
    }

    bool IsCommittedLocked(uintptr_t address)
    {
        auto it = g_committed.upper_bound(address);
        if (it == g_committed.begin()) return false;
        --it;
        return address < it->second;
    }

    bool IsRangeCommittedLocked(uintptr_t start, uintptr_t end)
    {
        if (end <= start) return false;
        auto it = g_committed.upper_bound(start);
        if (it == g_committed.begin()) return false;
        --it;
        return end <= it->second;
    }

    // The end of the run of pages starting at address that share its state.
    uintptr_t StateRunEndLocked(uintptr_t address, uintptr_t limit)
    {
        auto it = g_committed.upper_bound(address);
        if (it != g_committed.begin())
        {
            auto prev = std::prev(it);
            if (address < prev->second) return prev->second < limit ? prev->second : limit;
        }
        return (it != g_committed.end() && it->first < limit) ? it->first : limit;
    }

    void SetCommittedLocked(uintptr_t start, uintptr_t end, bool commit)
    {
        if (end <= start) return;

        auto it = g_committed.lower_bound(start);
        if (it != g_committed.begin())
        {
            auto prev = std::prev(it);
            if (prev->second > start)
            {
                const uintptr_t prevEnd = prev->second;
                prev->second = start;
                if (prevEnd > end) g_committed[end] = prevEnd;
            }
        }
        it = g_committed.lower_bound(start);
        while (it != g_committed.end() && it->first < end)
        {
            const uintptr_t itEnd = it->second;
            it = g_committed.erase(it);
            if (itEnd > end) { g_committed[end] = itEnd; break; }
        }

        if (!commit) return;

        uintptr_t newStart = start, newEnd = end;
        it = g_committed.lower_bound(start);
        if (it != g_committed.begin())
        {
            auto prev = std::prev(it);
            if (prev->second == start) { newStart = prev->first; g_committed.erase(prev); }
        }
        it = g_committed.find(end);
        if (it != g_committed.end()) { newEnd = it->second; g_committed.erase(it); }
        g_committed[newStart] = newEnd;
    }

    int WinProtToPosix(DWORD flProtect)
    {
        switch (flProtect & 0xFFu)
        {
        case PAGE_NOACCESS:          return PROT_NONE;
        case PAGE_READONLY:          return PROT_READ;
        case PAGE_READWRITE:
        case PAGE_WRITECOPY:         return PROT_READ | PROT_WRITE;
        case PAGE_EXECUTE:           return PROT_EXEC;
        case PAGE_EXECUTE_READ:      return PROT_READ | PROT_EXEC;
        case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
        default:                     return PROT_READ | PROT_WRITE;
        }
    }

    DWORD MachProtToWin(vm_prot_t prot)
    {
        if ((prot & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) == 0)
            return PAGE_NOACCESS;
        if (prot & VM_PROT_EXECUTE)
        {
            if (prot & VM_PROT_WRITE) return PAGE_EXECUTE_READWRITE;
            if (prot & VM_PROT_READ)  return PAGE_EXECUTE_READ;
            return PAGE_EXECUTE;
        }
        if (prot & VM_PROT_WRITE) return PAGE_READWRITE;
        return PAGE_READONLY;
    }

    size_t HostPageSize()
    {
        static const size_t pageSize = static_cast<size_t>(getpagesize());
        return pageSize;
    }

    constexpr uintptr_t BorrowedThreadHandleTag = 0x4000000000000000ULL;
    constexpr uintptr_t OwnedThreadHandleTag    = 0x2000000000000000ULL;
    constexpr uintptr_t SnapshotHandleTag       = 0x1000000000000000ULL;
    constexpr uintptr_t SpecialHandleMask       = BorrowedThreadHandleTag | OwnedThreadHandleTag | SnapshotHandleTag;

    struct ToolhelpThreadSnapshot
    {
        std::vector<DWORD> threadIds;
        size_t index = 0;
    };

    int HandleToFd(HANDLE h)
    {
        if (h == nullptr || h == INVALID_HANDLE_VALUE) return -1;
        const uintptr_t raw = reinterpret_cast<uintptr_t>(h);
        if ((raw & SpecialHandleMask) != 0 || raw == 0 || raw > 0x100000u)
            return -1;
        return static_cast<int>(raw - 1);
    }

    HANDLE FdToHandle(int fd)
    {
        if (fd < 0) return INVALID_HANDLE_VALUE;
        return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(fd + 1));
    }

    HANDLE BorrowedThreadToHandle(mach_port_t port)
    {
        if (port == MACH_PORT_NULL) return nullptr;
        return reinterpret_cast<HANDLE>(BorrowedThreadHandleTag | static_cast<uintptr_t>(port));
    }

    HANDLE OwnedThreadToHandle(mach_port_t port)
    {
        if (port == MACH_PORT_NULL) return nullptr;
        return reinterpret_cast<HANDLE>(OwnedThreadHandleTag | static_cast<uintptr_t>(port));
    }

    mach_port_t HandleToMachPort(HANDLE h)
    {
        if (h == nullptr || h == INVALID_HANDLE_VALUE) return MACH_PORT_NULL;
        const uintptr_t raw = reinterpret_cast<uintptr_t>(h);
        if ((raw & (BorrowedThreadHandleTag | OwnedThreadHandleTag)) == 0)
            return MACH_PORT_NULL;
        return static_cast<mach_port_t>(raw & ~SpecialHandleMask);
    }

    DWORD MachPortToOsId(mach_port_t port)
    {
        if (port == MACH_PORT_NULL) return 0;
        if (pthread_t pt = pthread_from_mach_thread_np(port))
        {
            const DWORD id = GetThreadId(pt);
            if (id != 0) return id;
        }
        thread_identifier_info_data_t idInfo{};
        mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(port, THREAD_IDENTIFIER_INFO,
                        reinterpret_cast<thread_info_t>(&idInfo), &count) == KERN_SUCCESS)
        {
            return static_cast<DWORD>(idInfo.thread_id);
        }
        return static_cast<DWORD>(port);
    }
}

// --- Timing -----------------------------------------------------------------

BOOL QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency)
{
    if (!lpFrequency) return FALSE;
    // Report ticks in nanoseconds (1 GHz).
    lpFrequency->QuadPart = 1000000000LL;
    return TRUE;
}

BOOL QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount)
{
    if (!lpPerformanceCount) return FALSE;
    static const mach_timebase_info_data_t timebase = []() {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        if (info.denom == 0) { info.numer = 1; info.denom = 1; }
        return info;
    }();
    const uint64_t ticks = mach_absolute_time();
    const __uint128_t ns = (static_cast<__uint128_t>(ticks) * timebase.numer) / timebase.denom;
    lpPerformanceCount->QuadPart = static_cast<LONGLONG>(ns);
    return TRUE;
}

// --- Virtual Memory & Safe Probing ------------------------------------------

void* VirtualAlloc(void* lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect)
{
    if (dwSize == 0) return nullptr;
    const bool commit  = (flAllocationType & MEM_COMMIT) != 0;
    const bool reserve = (flAllocationType & MEM_RESERVE) != 0;
    if (!commit && !reserve) return nullptr;

    const size_t pageSize = HostPageSize();
    const int prot = commit ? WinProtToPosix(flProtect) : PROT_NONE;

    if (lpAddress == nullptr)
    {
        const size_t alignedSize = (dwSize + pageSize - 1) & ~(pageSize - 1);
        void* mapped = mmap(nullptr, alignedSize, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mapped == MAP_FAILED) return nullptr;
        {
            std::lock_guard<std::mutex> lock(g_allocMutex);
            const uintptr_t base = reinterpret_cast<uintptr_t>(mapped);
            g_virtualAllocations[base] = alignedSize;
            SetCommittedLocked(base, base + alignedSize, commit);
        }
        return mapped;
    }

    const uintptr_t start = reinterpret_cast<uintptr_t>(lpAddress);
    const uintptr_t end   = start + dwSize;
    const uintptr_t alignedStart = start & ~(pageSize - 1);
    const uintptr_t alignedEnd   = (end + pageSize - 1) & ~(pageSize - 1);
    const size_t alignedSize     = alignedEnd - alignedStart;

    if (reserve)
    {
        // A fresh explicit-address reservation. The address is only a hint to
        // mmap without MAP_FIXED, so nothing already mapped there is clobbered:
        // if the kernel puts it anywhere else the range was not free, and the
        // request fails as it does on Windows.
        {
            std::lock_guard<std::mutex> lock(g_allocMutex);
            uintptr_t base = 0;
            size_t size = 0;
            if (FindAllocationLocked(alignedStart, base, size)) return nullptr;
        }
        void* mapped = mmap(reinterpret_cast<void*>(alignedStart), alignedSize, prot,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mapped == MAP_FAILED) return nullptr;
        if (reinterpret_cast<uintptr_t>(mapped) != alignedStart)
        {
            munmap(mapped, alignedSize);
            return nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(g_allocMutex);
            g_virtualAllocations[alignedStart] = alignedSize;
            SetCommittedLocked(alignedStart, alignedEnd, commit);
        }
        return lpAddress;
    }

    // Commit only: the range is expected to be reserved already.
    if (mprotect(reinterpret_cast<void*>(alignedStart), alignedSize, prot) != 0)
        return nullptr;

    uintptr_t extBase = 0;
    size_t extSize = 0;
    if (InExternalReservation(alignedStart, extBase, extSize))
    {
        SetExternalCommitted(extBase, extSize, alignedStart, alignedEnd, true);
    }
    else
    {
        std::lock_guard<std::mutex> lock(g_allocMutex);
        SetCommittedLocked(alignedStart, alignedEnd, true);
    }
    return lpAddress;
}

BOOL VirtualProtect(void* lpAddress, SIZE_T dwSize, DWORD flNewProtect, DWORD* lpflOldProtect)
{
    if (!lpAddress || dwSize == 0) return FALSE;

    const size_t pageSize = HostPageSize();
    const uintptr_t start = reinterpret_cast<uintptr_t>(lpAddress);
    const uintptr_t end   = start + dwSize;
    const uintptr_t alignedStart = start & ~(pageSize - 1);
    const uintptr_t alignedEnd   = (end + pageSize - 1) & ~(pageSize - 1);
    const int prot = WinProtToPosix(flNewProtect);

    // Match Windows VirtualProtect semantics: changing protection on an
    // uncommitted (MEM_RESERVE) page inside a tracked reservation fails.
    uintptr_t extBase = 0;
    size_t extSize = 0;
    if (InExternalReservation(alignedStart, extBase, extSize))
    {
        if (!IsExternalRangeCommitted(extBase, extSize, alignedStart, alignedEnd))
        {
            errno = EINVAL;
            return FALSE;
        }
    }
    else
    {
        std::lock_guard<std::mutex> lock(g_allocMutex);
        uintptr_t allocBase = 0;
        size_t allocSize = 0;
        if (FindAllocationLocked(alignedStart, allocBase, allocSize) &&
            !IsRangeCommittedLocked(alignedStart, alignedEnd))
        {
            errno = EINVAL;
            return FALSE;
        }
    }

    if (lpflOldProtect)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(lpAddress, &mbi, sizeof(mbi)) != 0)
            *lpflOldProtect = mbi.Protect;
        else
            *lpflOldProtect = PAGE_READWRITE;
    }

    if (prot == PROT_NONE)
    {
        // On Apple Silicon the hardware page size is 16 KB while the guest uses
        // 4 KB small pages. When revoking access (PAGE_NOACCESS on freed heap
        // ranges), only revoke full hardware pages contained within [start, end)
        // so live neighbouring 4 KB allocations sharing a 16 KB page do not fault.
        const uintptr_t innerStart = (start + pageSize - 1) & ~(pageSize - 1);
        const uintptr_t innerEnd   = end & ~(pageSize - 1);
        if (innerEnd <= innerStart)
            return TRUE;
        return mprotect(reinterpret_cast<void*>(innerStart), innerEnd - innerStart, prot) == 0 ? TRUE : FALSE;
    }

    return mprotect(reinterpret_cast<void*>(alignedStart), alignedEnd - alignedStart, prot) == 0 ? TRUE : FALSE;
}

BOOL VirtualFree(void* lpAddress, SIZE_T dwSize, DWORD dwFreeType)
{
    if (!lpAddress) return FALSE;
    const size_t pageSize = HostPageSize();

    if (dwFreeType & MEM_RELEASE)
    {
        const uintptr_t start = reinterpret_cast<uintptr_t>(lpAddress) & ~(pageSize - 1);
        size_t releaseSize = 0;
        {
            std::lock_guard<std::mutex> lock(g_allocMutex);
            auto it = g_virtualAllocations.find(start);
            if (it != g_virtualAllocations.end())
            {
                releaseSize = it->second;
                g_virtualAllocations.erase(it);
            }
        }
        if (dwSize != 0)
            releaseSize = (dwSize + pageSize - 1) & ~(pageSize - 1);
        if (releaseSize == 0)
            return FALSE;
        {
            std::lock_guard<std::mutex> lock(g_allocMutex);
            SetCommittedLocked(start, start + releaseSize, false);
        }
        return munmap(reinterpret_cast<void*>(start), releaseSize) == 0 ? TRUE : FALSE;
    }

    if ((dwFreeType & MEM_DECOMMIT) && dwSize > 0)
    {
        const uintptr_t start = (reinterpret_cast<uintptr_t>(lpAddress) + pageSize - 1) & ~(pageSize - 1);
        const uintptr_t end   = (reinterpret_cast<uintptr_t>(lpAddress) + dwSize) & ~(pageSize - 1);
        if (end > start)
        {
            madvise(reinterpret_cast<void*>(start), end - start, MADV_DONTNEED);
            mprotect(reinterpret_cast<void*>(start), end - start, PROT_NONE);
            uintptr_t extBase = 0;
            size_t extSize = 0;
            if (InExternalReservation(start, extBase, extSize))
            {
                SetExternalCommitted(extBase, extSize, start, end, false);
            }
            else
            {
                std::lock_guard<std::mutex> lock(g_allocMutex);
                SetCommittedLocked(start, end, false);
            }
        }
        return TRUE;
    }
    return FALSE;
}

SIZE_T VirtualQuery(const void* lpAddress, MEMORY_BASIC_INFORMATION* lpBuffer, SIZE_T dwLength)
{
    if (!lpBuffer || dwLength < sizeof(MEMORY_BASIC_INFORMATION)) return 0;
    memset(lpBuffer, 0, sizeof(MEMORY_BASIC_INFORMATION));

    const size_t pageSize = HostPageSize();
    const mach_vm_address_t target = reinterpret_cast<mach_vm_address_t>(lpAddress);
    const mach_vm_address_t pageAlignedTarget = target & ~static_cast<mach_vm_address_t>(pageSize - 1);

    mach_vm_address_t regionAddr = pageAlignedTarget;
    mach_vm_size_t regionSize = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t infoCount = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t objectName = MACH_PORT_NULL;

    const kern_return_t kr = mach_vm_region(
        mach_task_self(),
        &regionAddr,
        &regionSize,
        VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&info),
        &infoCount,
        &objectName);

    if (objectName != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), objectName);

    if (kr != KERN_SUCCESS || regionAddr > pageAlignedTarget)
    {
        lpBuffer->BaseAddress = reinterpret_cast<void*>(pageAlignedTarget);
        lpBuffer->RegionSize  = (kr == KERN_SUCCESS && regionAddr > pageAlignedTarget)
            ? static_cast<SIZE_T>(regionAddr - pageAlignedTarget)
            : pageSize;
        lpBuffer->State       = MEM_FREE;
        lpBuffer->Protect     = PAGE_NOACCESS;
        return sizeof(MEMORY_BASIC_INFORMATION);
    }

    const DWORD winProt = MachProtToWin(info.protection);
    lpBuffer->BaseAddress       = reinterpret_cast<void*>(pageAlignedTarget);
    lpBuffer->AllocationBase    = reinterpret_cast<void*>(regionAddr);
    lpBuffer->AllocationProtect = winProt;
    lpBuffer->RegionSize        = static_cast<SIZE_T>((regionAddr + regionSize) - pageAlignedTarget);

    // Commitment is tracked separately from protection: PAGE_NOACCESS memory
    // can be committed, and changing protection never changes commitment.
    // Memory VirtualAlloc did not reserve falls back on its protection.
    DWORD state = (info.protection != VM_PROT_NONE) ? MEM_COMMIT : MEM_RESERVE;
    uintptr_t extBase = 0;
    size_t extSize = 0;
    if (InExternalReservation(pageAlignedTarget, extBase, extSize))
    {
        state = IsExternalPageCommitted(extBase, pageAlignedTarget) ? MEM_COMMIT : MEM_RESERVE;
        const uintptr_t limit = pageAlignedTarget + lpBuffer->RegionSize;
        const uintptr_t runEnd = ExternalStateRunEnd(extBase, extSize, pageAlignedTarget, limit);
        lpBuffer->RegionSize = static_cast<SIZE_T>(runEnd - pageAlignedTarget);
        lpBuffer->AllocationBase = reinterpret_cast<void*>(extBase);
    }
    else
    {
        std::lock_guard<std::mutex> lock(g_allocMutex);
        uintptr_t allocBase = 0;
        size_t allocSize = 0;
        if (FindAllocationLocked(pageAlignedTarget, allocBase, allocSize))
        {
            state = IsCommittedLocked(pageAlignedTarget) ? MEM_COMMIT : MEM_RESERVE;
            const uintptr_t limit = pageAlignedTarget + lpBuffer->RegionSize;
            const uintptr_t runEnd = StateRunEndLocked(pageAlignedTarget, limit);
            lpBuffer->RegionSize = static_cast<SIZE_T>(runEnd - pageAlignedTarget);
            lpBuffer->AllocationBase = reinterpret_cast<void*>(allocBase);
        }
    }
    lpBuffer->State             = state;
    lpBuffer->Protect           = winProt;
    lpBuffer->Type              = MEM_PRIVATE;
    return sizeof(MEMORY_BASIC_INFORMATION);
}

BOOL ReadProcessMemory(HANDLE, LPCVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesRead)
{
    if (lpNumberOfBytesRead) *lpNumberOfBytesRead = 0;
    if (!lpBaseAddress || !lpBuffer || nSize == 0) return FALSE;

    // Verify every page in the range is committed and readable so we never
    // trigger demand-commit or fault on PROT_NONE reservations.
    const size_t pageSize = HostPageSize();
    const uintptr_t start = reinterpret_cast<uintptr_t>(lpBaseAddress);
    const uintptr_t end   = start + nSize;
    for (uintptr_t addr = start & ~(pageSize - 1); addr < end; )
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) == 0)
            return FALSE;
        if (mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS)
            return FALSE;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        addr = (regionEnd > addr) ? regionEnd : (addr + pageSize);
    }

    mach_vm_size_t outSize = 0;
    const kern_return_t kr = mach_vm_read_overwrite(
        mach_task_self(),
        reinterpret_cast<mach_vm_address_t>(lpBaseAddress),
        static_cast<mach_vm_size_t>(nSize),
        reinterpret_cast<mach_vm_address_t>(lpBuffer),
        &outSize);

    if (kr != KERN_SUCCESS) return FALSE;
    if (lpNumberOfBytesRead) *lpNumberOfBytesRead = static_cast<SIZE_T>(outSize);
    return TRUE;
}

void GetCurrentThreadStackLimits(ULONG_PTR* LowLimit, ULONG_PTR* HighLimit)
{
    const pthread_t self = pthread_self();
    void* high = pthread_get_stackaddr_np(self);
    const size_t size = pthread_get_stacksize_np(self);
    const uintptr_t highAddr = reinterpret_cast<uintptr_t>(high);
    if (HighLimit) *HighLimit = highAddr;
    if (LowLimit)  *LowLimit  = (highAddr >= size) ? (highAddr - size) : 0;
}

// --- File & OS Helpers ------------------------------------------------------

HANDLE CreateFileW(const char* lpFileName, DWORD dwDesiredAccess, DWORD,
                   void*, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE)
{
    if (!lpFileName) return INVALID_HANDLE_VALUE;

    const bool read  = (dwDesiredAccess & GENERIC_READ) != 0;
    const bool write = (dwDesiredAccess & GENERIC_WRITE) != 0;
    int flags = O_CLOEXEC;
    if (read && write) flags |= O_RDWR;
    else if (write)    flags |= O_WRONLY;
    else               flags |= O_RDONLY;

    switch (dwCreationDisposition)
    {
    case CREATE_NEW:        flags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS:     flags |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING:     break;
    case OPEN_ALWAYS:       flags |= O_CREAT; break;
    case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
    default:                break;
    }

    if ((dwFlagsAndAttributes & FILE_FLAG_BACKUP_SEMANTICS) && !write)
    {
        struct stat st{};
        if (stat(lpFileName, &st) == 0 && S_ISDIR(st.st_mode))
            flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
    }

    const int fd = open(lpFileName, flags, 0644);
    return FdToHandle(fd);
}

HANDLE CreateFileW(const wchar_t* lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   void* lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
    if (!lpFileName) return INVALID_HANDLE_VALUE;
    const std::string narrow = NarrowFromWide(lpFileName);
    return CreateFileW(narrow.c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                       dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

BOOL CloseHandle(HANDLE hObject)
{
    if (hObject == nullptr || hObject == INVALID_HANDLE_VALUE)
        return FALSE;

    const uintptr_t raw = reinterpret_cast<uintptr_t>(hObject);
    if ((raw & BorrowedThreadHandleTag) != 0)
        return TRUE;

    if ((raw & OwnedThreadHandleTag) != 0)
    {
        const mach_port_t port = static_cast<mach_port_t>(raw & ~SpecialHandleMask);
        if (port != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), port);
        return TRUE;
    }

    if ((raw & SnapshotHandleTag) != 0)
    {
        auto* snap = reinterpret_cast<ToolhelpThreadSnapshot*>(raw & ~SpecialHandleMask);
        delete snap;
        return TRUE;
    }

    const int fd = HandleToFd(hObject);
    if (fd < 0) return FALSE;
    return close(fd) == 0 ? TRUE : FALSE;
}

BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
              DWORD* lpNumberOfBytesRead, void*)
{
    if (lpNumberOfBytesRead) *lpNumberOfBytesRead = 0;
    const int fd = HandleToFd(hFile);
    if (fd < 0 || !lpBuffer) return FALSE;
    const ssize_t got = read(fd, lpBuffer, nNumberOfBytesToRead);
    if (got < 0) return FALSE;
    if (lpNumberOfBytesRead) *lpNumberOfBytesRead = static_cast<DWORD>(got);
    return TRUE;
}

BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
               DWORD* lpNumberOfBytesWritten, void*)
{
    if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = 0;
    const int fd = HandleToFd(hFile);
    if (fd < 0 || !lpBuffer) return FALSE;
    const ssize_t put = write(fd, lpBuffer, nNumberOfBytesToWrite);
    if (put < 0) return FALSE;
    if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = static_cast<DWORD>(put);
    return TRUE;
}

DWORD SetFilePointer(HANDLE hFile, LONG lDistanceToMove, LONG* lpDistanceToMoveHigh, DWORD dwMoveMethod)
{
    LARGE_INTEGER dist{};
    dist.LowPart  = static_cast<DWORD>(lDistanceToMove);
    dist.HighPart = lpDistanceToMoveHigh ? *lpDistanceToMoveHigh : (lDistanceToMove < 0 ? -1 : 0);
    LARGE_INTEGER newPos{};
    if (!SetFilePointerEx(hFile, dist, &newPos, dwMoveMethod))
        return DWORD(-1);
    if (lpDistanceToMoveHigh) *lpDistanceToMoveHigh = newPos.HighPart;
    return newPos.LowPart;
}

BOOL SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove,
                      LARGE_INTEGER* lpNewFilePointer, DWORD dwMoveMethod)
{
    const int fd = HandleToFd(hFile);
    if (fd < 0) return FALSE;
    int whence = SEEK_SET;
    if (dwMoveMethod == FILE_CURRENT) whence = SEEK_CUR;
    else if (dwMoveMethod == FILE_END) whence = SEEK_END;
    const off_t pos = lseek(fd, static_cast<off_t>(liDistanceToMove.QuadPart), whence);
    if (pos == static_cast<off_t>(-1)) return FALSE;
    if (lpNewFilePointer) lpNewFilePointer->QuadPart = static_cast<LONGLONG>(pos);
    return TRUE;
}

BOOL GetFileSizeEx(HANDLE hFile, LARGE_INTEGER* lpFileSize)
{
    const int fd = HandleToFd(hFile);
    if (fd < 0 || !lpFileSize) return FALSE;
    struct stat st{};
    if (fstat(fd, &st) != 0) return FALSE;
    lpFileSize->QuadPart = S_ISDIR(st.st_mode) ? 0 : static_cast<LONGLONG>(st.st_size);
    return TRUE;
}

BOOL SetEndOfFile(HANDLE hFile)
{
    const int fd = HandleToFd(hFile);
    if (fd < 0) return FALSE;
    const off_t cur = lseek(fd, 0, SEEK_CUR);
    if (cur == static_cast<off_t>(-1)) return FALSE;
    return ftruncate(fd, cur) == 0 ? TRUE : FALSE;
}

BOOL FlushFileBuffers(HANDLE hFile)
{
    const int fd = HandleToFd(hFile);
    if (fd < 0) return FALSE;
    return fsync(fd) == 0 ? TRUE : FALSE;
}

BOOL MoveFileExA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, DWORD)
{
    if (!lpExistingFileName || !lpNewFileName) return FALSE;
    return rename(lpExistingFileName, lpNewFileName) == 0 ? TRUE : FALSE;
}

BOOL DeleteFileA(LPCSTR lpFileName)
{
    if (!lpFileName) return FALSE;
    return unlink(lpFileName) == 0 ? TRUE : FALSE;
}

// --- Thread & Toolhelp Helpers ----------------------------------------------

namespace
{
    struct PthreadStartInfo
    {
        LPTHREAD_START_ROUTINE startAddress = nullptr;
        LPVOID parameter = nullptr;
        bool createSuspended = false;
        std::mutex mutex;
        std::condition_variable cv;
        mach_port_t machPort = MACH_PORT_NULL;
        DWORD osId = 0;
        bool childReady = false;
        std::atomic<bool> parentDone{ false };
    };

    void* PthreadTrampoline(void* arg)
    {
        auto* holder = static_cast<std::shared_ptr<PthreadStartInfo>*>(arg);
        std::shared_ptr<PthreadStartInfo> info = std::move(*holder);
        delete holder;

        const mach_port_t selfPort = pthread_mach_thread_np(pthread_self());
        const DWORD osId = GetCurrentThreadId();
        const bool createSuspended = info->createSuspended;
        const LPTHREAD_START_ROUTINE startAddress = info->startAddress;
        LPVOID const parameter = info->parameter;

        {
            std::lock_guard<std::mutex> lock(info->mutex);
            info->machPort = selfPort;
            info->osId = osId;
            info->childReady = true;
            info->cv.notify_one();
        }

        if (createSuspended)
        {
            while (!info->parentDone.load(std::memory_order_acquire))
            {
#if defined(__aarch64__)
                __builtin_arm_yield();
#else
                sched_yield();
#endif
            }
        }

        info.reset();
        const DWORD exitCode = startAddress ? startAddress(parameter) : 0;
        return reinterpret_cast<void*>(static_cast<uintptr_t>(exitCode));
    }
}

HANDLE GetCurrentThread()
{
    return BorrowedThreadToHandle(pthread_mach_thread_np(pthread_self()));
}

BOOL DuplicateHandle(HANDLE, HANDLE hSourceHandle, HANDLE, HANDLE* lpTargetHandle,
                     DWORD, BOOL, DWORD)
{
    if (!lpTargetHandle) return FALSE;
    const mach_port_t port = HandleToMachPort(hSourceHandle);
    if (port == MACH_PORT_NULL)
    {
        *lpTargetHandle = nullptr;
        return FALSE;
    }
    *lpTargetHandle = BorrowedThreadToHandle(port);
    return TRUE;
}

HANDLE CreateThread(void*, size_t dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress,
                    LPVOID lpParameter, DWORD dwCreationFlags, DWORD* lpThreadId)
{
    if (!lpStartAddress) return nullptr;

    auto info = std::make_shared<PthreadStartInfo>();
    info->startAddress = lpStartAddress;
    info->parameter = lpParameter;
    info->createSuspended = (dwCreationFlags & CREATE_SUSPENDED) != 0;

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
        return nullptr;

    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    constexpr size_t MinHostStackSize = 8u << 20; // 8 MB minimum host stack
    const size_t pageSize = HostPageSize();
    size_t stackSize = dwStackSize > MinHostStackSize ? dwStackSize : MinHostStackSize;
    stackSize = (stackSize + pageSize - 1) & ~(pageSize - 1);
    pthread_attr_setstacksize(&attr, stackSize);

    auto* holder = new std::shared_ptr<PthreadStartInfo>(info);
    pthread_t pt{};
    const int rc = pthread_create(&pt, &attr, PthreadTrampoline, holder);
    pthread_attr_destroy(&attr);
    if (rc != 0)
    {
        delete holder;
        return nullptr;
    }

    {
        std::unique_lock<std::mutex> lock(info->mutex);
        info->cv.wait(lock, [&] { return info->childReady; });
    }

    if (info->createSuspended)
    {
        thread_suspend(info->machPort);
        info->parentDone.store(true, std::memory_order_release);
    }

    if (lpThreadId)
        *lpThreadId = info->osId;

    return BorrowedThreadToHandle(info->machPort);
}

DWORD SuspendThread(HANDLE hThread)
{
    const mach_port_t port = HandleToMachPort(hThread);
    if (port == MACH_PORT_NULL)
        return DWORD(-1);

    DWORD prevCount = 0;
    thread_basic_info_data_t basicInfo{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO,
                    reinterpret_cast<thread_info_t>(&basicInfo), &count) == KERN_SUCCESS)
    {
        prevCount = basicInfo.suspend_count > 0 ? static_cast<DWORD>(basicInfo.suspend_count) : 0;
    }

    if (thread_suspend(port) != KERN_SUCCESS)
        return DWORD(-1);

    return prevCount;
}

DWORD ResumeThread(HANDLE hThread)
{
    const mach_port_t port = HandleToMachPort(hThread);
    if (port == MACH_PORT_NULL)
        return DWORD(-1);

    // Win32 returns the suspend count from before the resume.
    DWORD prevCount = 0;
    thread_basic_info_data_t basicInfo{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO,
                    reinterpret_cast<thread_info_t>(&basicInfo), &count) == KERN_SUCCESS)
    {
        prevCount = basicInfo.suspend_count > 0 ? static_cast<DWORD>(basicInfo.suspend_count) : 0;
    }

    const kern_return_t kr = thread_resume(port);
    if (kr == KERN_SUCCESS)
        // A suspension that landed between the query and the resume still
        // means the thread was suspended at least once.
        return prevCount > 0 ? prevCount : 1;
    if (kr == KERN_FAILURE)
        return 0;
    return DWORD(-1);
}

BOOL SetThreadPriority(HANDLE hThread, int nPriority)
{
    const mach_port_t port = HandleToMachPort(hThread);
    if (port == MACH_PORT_NULL)
        return FALSE;

    thread_precedence_policy_data_t policy{};
    if (nPriority > 0)      policy.importance = 4;
    else if (nPriority < 0) policy.importance = -4;
    else                    policy.importance = 0;

    thread_policy_set(port, THREAD_PRECEDENCE_POLICY,
                      reinterpret_cast<thread_policy_t>(&policy),
                      THREAD_PRECEDENCE_POLICY_COUNT);
    return TRUE;
}

HANDLE OpenThread(DWORD, BOOL, DWORD dwThreadId)
{
    if (dwThreadId == 0) return nullptr;

    thread_act_array_t threadList = nullptr;
    mach_msg_type_number_t threadCount = 0;
    if (task_threads(mach_task_self(), &threadList, &threadCount) != KERN_SUCCESS)
        return nullptr;

    mach_port_t foundPort = MACH_PORT_NULL;
    for (mach_msg_type_number_t i = 0; i < threadCount; ++i)
    {
        const mach_port_t port = threadList[i];
        if (foundPort == MACH_PORT_NULL && MachPortToOsId(port) == dwThreadId)
        {
            foundPort = port;
        }
        else
        {
            mach_port_deallocate(mach_task_self(), port);
        }
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threadList),
                  threadCount * sizeof(thread_act_t));

    return OwnedThreadToHandle(foundPort);
}

BOOL GetThreadContext(HANDLE hThread, CONTEXT* lpContext)
{
    if (!lpContext) return FALSE;
    const mach_port_t port = HandleToMachPort(hThread);
    if (port == MACH_PORT_NULL) return FALSE;

#if defined(__aarch64__)
    // CONTEXT_FULL and CONTEXT_DEBUG_REGISTERS share the architecture bit, so
    // the requested parts are tested with it masked off.
    const DWORD requested = lpContext->ContextFlags & ~ContextArchitectureBit;
    if (lpContext->ContextFlags == 0 || (requested & ContextCoreMask) != 0)
    {
        arm_thread_state64_t state{};
        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(port, ARM_THREAD_STATE64,
                             reinterpret_cast<thread_state_t>(&state), &count) != KERN_SUCCESS)
        {
            return FALSE;
        }
        for (int i = 0; i < 29; ++i)
            lpContext->X[i] = state.__x[i];
        lpContext->Rbp = arm_thread_state64_get_fp(state);
        lpContext->Lr  = arm_thread_state64_get_lr(state);
        lpContext->Rsp = arm_thread_state64_get_sp(state);
        lpContext->Rip = arm_thread_state64_get_pc(state);
    }

    if (requested & ContextDebugMask)
    {
        arm_debug_state64_t dbg{};
        mach_msg_type_number_t count = ARM_DEBUG_STATE64_COUNT;
        if (thread_get_state(port, ARM_DEBUG_STATE64,
                             reinterpret_cast<thread_state_t>(&dbg), &count) == KERN_SUCCESS)
        {
            lpContext->Dr0 = dbg.__wvr[0];
            lpContext->Dr7 = dbg.__wcr[0];
        }
    }
    return TRUE;
#else
    return FALSE;
#endif
}

BOOL SetThreadContext(HANDLE hThread, const CONTEXT* lpContext)
{
    if (!lpContext) return FALSE;
    const mach_port_t port = HandleToMachPort(hThread);
    if (port == MACH_PORT_NULL) return FALSE;

#if defined(__aarch64__)
    const DWORD requested = lpContext->ContextFlags & ~ContextArchitectureBit;
    // Core-register writeback (PC, SP, general registers) is not implemented;
    // refuse it rather than report a success that changed nothing.
    if ((requested & ContextCoreMask) != 0)
        return FALSE;
    if (requested & ContextDebugMask)
    {
        arm_debug_state64_t dbg{};
        mach_msg_type_number_t count = ARM_DEBUG_STATE64_COUNT;
        thread_get_state(port, ARM_DEBUG_STATE64,
                         reinterpret_cast<thread_state_t>(&dbg), &count);
        dbg.__wvr[0] = lpContext->Dr0;
        if (lpContext->Dr7 != 0)
            dbg.__wcr[0] = (1u << 0) | (2u << 1) | (2u << 3) | (0xFu << 5);
        else
            dbg.__wcr[0] = 0;
        return thread_set_state(port, ARM_DEBUG_STATE64,
                                reinterpret_cast<thread_state_t>(&dbg),
                                ARM_DEBUG_STATE64_COUNT) == KERN_SUCCESS ? TRUE : FALSE;
    }
    return TRUE;
#else
    return FALSE;
#endif
}

BOOL GetThreadTimes(HANDLE hThread, FILETIME* lpCreationTime, FILETIME* lpExitTime,
                    FILETIME* lpKernelTime, FILETIME* lpUserTime)
{
    const mach_port_t port = HandleToMachPort(hThread);
    if (port == MACH_PORT_NULL) return FALSE;

    thread_basic_info_data_t basicInfo{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO,
                    reinterpret_cast<thread_info_t>(&basicInfo), &count) != KERN_SUCCESS)
    {
        return FALSE;
    }

    auto toFileTime = [](const time_value_t& tv, FILETIME* out) {
        if (!out) return;
        const uint64_t ticks = static_cast<uint64_t>(tv.seconds) * 10000000ULL +
                               static_cast<uint64_t>(tv.microseconds) * 10ULL;
        out->dwLowDateTime  = static_cast<DWORD>(ticks & 0xFFFFFFFFULL);
        out->dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    };

    if (lpCreationTime) { lpCreationTime->dwLowDateTime = 0; lpCreationTime->dwHighDateTime = 0; }
    if (lpExitTime)     { lpExitTime->dwLowDateTime = 0;     lpExitTime->dwHighDateTime = 0; }
    toFileTime(basicInfo.system_time, lpKernelTime);
    toFileTime(basicInfo.user_time, lpUserTime);
    return TRUE;
}

HANDLE CreateToolhelp32Snapshot(DWORD dwFlags, DWORD)
{
    if ((dwFlags & TH32CS_SNAPTHREAD) == 0)
        return INVALID_HANDLE_VALUE;

    thread_act_array_t threadList = nullptr;
    mach_msg_type_number_t threadCount = 0;
    if (task_threads(mach_task_self(), &threadList, &threadCount) != KERN_SUCCESS)
        return INVALID_HANDLE_VALUE;

    auto* snap = new ToolhelpThreadSnapshot();
    snap->threadIds.reserve(threadCount);
    for (mach_msg_type_number_t i = 0; i < threadCount; ++i)
    {
        const DWORD osId = MachPortToOsId(threadList[i]);
        if (osId != 0)
            snap->threadIds.push_back(osId);
        mach_port_deallocate(mach_task_self(), threadList[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threadList),
                  threadCount * sizeof(thread_act_t));

    return reinterpret_cast<HANDLE>(SnapshotHandleTag | reinterpret_cast<uintptr_t>(snap));
}

BOOL Thread32First(HANDLE hSnapshot, THREADENTRY32* lpte)
{
    if (!lpte || hSnapshot == nullptr || hSnapshot == INVALID_HANDLE_VALUE)
        return FALSE;
    const uintptr_t raw = reinterpret_cast<uintptr_t>(hSnapshot);
    if ((raw & SnapshotHandleTag) == 0)
        return FALSE;
    auto* snap = reinterpret_cast<ToolhelpThreadSnapshot*>(raw & ~SpecialHandleMask);
    snap->index = 0;
    if (snap->threadIds.empty())
        return FALSE;
    lpte->th32ThreadID = snap->threadIds[snap->index++];
    lpte->th32OwnerProcessID = GetCurrentProcessId();
    return TRUE;
}

BOOL Thread32Next(HANDLE hSnapshot, THREADENTRY32* lpte)
{
    if (!lpte || hSnapshot == nullptr || hSnapshot == INVALID_HANDLE_VALUE)
        return FALSE;
    const uintptr_t raw = reinterpret_cast<uintptr_t>(hSnapshot);
    if ((raw & SnapshotHandleTag) == 0)
        return FALSE;
    auto* snap = reinterpret_cast<ToolhelpThreadSnapshot*>(raw & ~SpecialHandleMask);
    if (snap->index >= snap->threadIds.size())
        return FALSE;
    lpte->th32ThreadID = snap->threadIds[snap->index++];
    lpte->th32OwnerProcessID = GetCurrentProcessId();
    return TRUE;
}

// --- Display & Keyboard Helpers ---------------------------------------------

int GetSystemMetrics(int nIndex)
{
    const CGDirectDisplayID mainDisplay = CGMainDisplayID();
    if (nIndex == SM_CXSCREEN)
    {
        const size_t w = CGDisplayPixelsWide(mainDisplay);
        return w > 0 ? static_cast<int>(w) : 1280;
    }
    if (nIndex == SM_CYSCREEN)
    {
        const size_t h = CGDisplayPixelsHigh(mainDisplay);
        return h > 0 ? static_cast<int>(h) : 720;
    }
    return 0;
}

BOOL EnumDisplaySettingsW(LPCWSTR, DWORD iModeNum, DEVMODEW* lpDevMode)
{
    if (!lpDevMode) return FALSE;
    static const std::vector<std::pair<DWORD, DWORD>> modes = []() {
        std::vector<std::pair<DWORD, DWORD>> result;
        CFArrayRef cgModes = CGDisplayCopyAllDisplayModes(CGMainDisplayID(), nullptr);
        if (cgModes)
        {
            const CFIndex count = CFArrayGetCount(cgModes);
            for (CFIndex i = 0; i < count; ++i)
            {
                auto* m = static_cast<CGDisplayModeRef>(
                    const_cast<void*>(CFArrayGetValueAtIndex(cgModes, i)));
                if (!m) continue;
                const DWORD w = static_cast<DWORD>(CGDisplayModeGetWidth(m));
                const DWORD h = static_cast<DWORD>(CGDisplayModeGetHeight(m));
                if (w > 0 && h > 0)
                    result.emplace_back(w, h);
            }
            CFRelease(cgModes);
        }
        if (result.empty())
        {
            result.emplace_back(1280, 720);
            result.emplace_back(1920, 1080);
            result.emplace_back(2560, 1440);
        }
        return result;
    }();

    if (iModeNum >= modes.size()) return FALSE;
    memset(lpDevMode, 0, sizeof(*lpDevMode));
    lpDevMode->dmSize       = sizeof(*lpDevMode);
    lpDevMode->dmPelsWidth  = modes[iModeNum].first;
    lpDevMode->dmPelsHeight = modes[iModeNum].second;
    lpDevMode->dmBitsPerPel = 32;
    lpDevMode->dmDisplayFrequency = 60;
    return TRUE;
}

UINT MapVirtualKeyW(UINT uCode, UINT)
{
    return uCode;
}

int GetKeyNameTextW(LONG lParam, LPWSTR lpString, int cchSize)
{
    if (!lpString || cchSize <= 0) return 0;
    const UINT vk = static_cast<UINT>((lParam >> 16) & 0xFFFF);
    const wchar_t* name = nullptr;
    switch (vk)
    {
    case VK_OEM_1:      name = L";"; break;
    case VK_OEM_PLUS:   name = L"="; break;
    case VK_OEM_COMMA:  name = L","; break;
    case VK_OEM_MINUS:  name = L"-"; break;
    case VK_OEM_PERIOD: name = L"."; break;
    case VK_OEM_2:      name = L"/"; break;
    case VK_OEM_3:      name = L"`"; break;
    case VK_OEM_4:      name = L"["; break;
    case VK_OEM_5:      name = L"\\"; break;
    case VK_OEM_6:      name = L"]"; break;
    case VK_OEM_7:      name = L"'"; break;
    case VK_INSERT:     name = L"Insert"; break;
    case VK_DELETE:     name = L"Delete"; break;
    case VK_HOME:       name = L"Home"; break;
    case VK_END:        name = L"End"; break;
    case VK_PRIOR:      name = L"Page Up"; break;
    case VK_NEXT:       name = L"Page Down"; break;
    default:            break;
    }
    if (!name) return 0;
    wcsncpy(lpString, name, static_cast<size_t>(cchSize - 1));
    lpString[cchSize - 1] = L'\0';
    return static_cast<int>(wcslen(lpString));
}

DWORD GetModuleFileNameW(HMODULE, LPWSTR lpFilename, DWORD nSize)
{
    if (!lpFilename || nSize == 0) return 0;
    uint32_t bufSize = 0;
    _NSGetExecutablePath(nullptr, &bufSize);
    std::vector<char> buf(bufSize > 0 ? bufSize : 1024, '\0');
    if (_NSGetExecutablePath(buf.data(), &bufSize) != 0) return 0;

    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(buf.data(), ec);
    const std::wstring wide = (ec ? std::filesystem::path(buf.data()) : resolved).wstring();
    const size_t toCopy = std::min(wide.size(), static_cast<size_t>(nSize - 1));
    memcpy(lpFilename, wide.data(), toCopy * sizeof(wchar_t));
    lpFilename[toCopy] = L'\0';
    return static_cast<DWORD>(toCopy);
}

// --- Platform Namespace -----------------------------------------------------

namespace Platform
{
    std::filesystem::path ExecutableDirectory()
    {
        uint32_t bufSize = 0;
        _NSGetExecutablePath(nullptr, &bufSize);
        std::vector<char> buf(bufSize > 0 ? bufSize : 1024, '\0');
        if (_NSGetExecutablePath(buf.data(), &bufSize) != 0)
            return std::filesystem::current_path();
        std::error_code ec;
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(buf.data(), ec);
        if (!ec && !canonical.empty())
            return canonical.parent_path();
        return std::filesystem::path(buf.data()).parent_path();
    }

    bool OpenFileDialog(bool wantFolder, const char* title, std::filesystem::path& chosen)
    {
        __block bool picked = false;
        __block std::string chosenUtf8;
        const std::string dialogTitle = title ? title : (wantFolder ? "Select Folder" : "Select File");

        auto runDialog = ^{
            @autoreleasepool
            {
                NSApplication* app = [NSApplication sharedApplication];
                if ([app activationPolicy] == NSApplicationActivationPolicyProhibited)
                    [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
                [app activateIgnoringOtherApps:YES];

                NSOpenPanel* panel = [NSOpenPanel openPanel];
                NSString* nsTitle = [NSString stringWithUTF8String:dialogTitle.c_str()];
                [panel setTitle:nsTitle];
                [panel setMessage:nsTitle];
                [panel setCanChooseDirectories:(wantFolder ? YES : NO)];
                [panel setCanChooseFiles:(wantFolder ? NO : YES)];
                [panel setAllowsMultipleSelection:NO];

                if (!wantFolder)
                {
                    #pragma clang diagnostic push
                    #pragma clang diagnostic ignored "-Wdeprecated-declarations"
                    [panel setAllowedFileTypes:@[@"iso", @"ISO"]];
                    [panel setAllowsOtherFileTypes:YES];
                    #pragma clang diagnostic pop
                }

                if ([panel runModal] == NSModalResponseOK)
                {
                    NSURL* url = [[panel URLs] firstObject];
                    if (url && [url isFileURL] && [url fileSystemRepresentation])
                    {
                        chosenUtf8 = [url fileSystemRepresentation];
                        picked = true;
                    }
                }
            }
        };

        if ([NSThread isMainThread])
            runDialog();
        else
            dispatch_sync(dispatch_get_main_queue(), runDialog);

        if (picked)
            chosen = std::filesystem::path(chosenUtf8);
        return picked;
    }

    namespace
    {
        __attribute__((noinline)) uintptr_t JumpBufferMungeToken()
        {
#if defined(__aarch64__)
            jmp_buf probe{};
            const uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
            (void)_setjmp(probe);
            const auto* words = reinterpret_cast<const uint64_t*>(&probe);
            // In Darwin ARM64 libsystem_platform _setjmp:
            // words[10] (offset 0x50) is x29 (fp) ^ ptr_munge_token.
            return words[10] ^ fp;
#else
            return 0;
#endif
        }
    }

    bool InspectJumpBuffer(const jmp_buf& buffer, uintptr_t& outSp, uintptr_t& outIp)
    {
#if defined(__aarch64__)
        static const uintptr_t token = JumpBufferMungeToken();
        const auto* words = reinterpret_cast<const uint64_t*>(&buffer);
        // words[10] = fp ^ token, words[11] = lr ^ token, words[12] = sp ^ token.
        // If the buffer is all zeros (never filled by host setjmp), report 0.
        if (words[10] == 0 && words[11] == 0 && words[12] == 0)
        {
            outSp = 0;
            outIp = 0;
            return false;
        }
        outIp = static_cast<uintptr_t>(words[11] ^ token);
        outSp = static_cast<uintptr_t>(words[12] ^ token);
        return true;
#else
        const auto* words = reinterpret_cast<const uint64_t*>(&buffer);
        outSp = static_cast<uintptr_t>(words[2]);
        outIp = static_cast<uintptr_t>(words[7]);
        return outSp != 0 && outIp != 0;
#endif
    }

    bool RelaunchProcess(const std::filesystem::path& launchDataFile)
    {
        uint32_t bufSize = 0;
        _NSGetExecutablePath(nullptr, &bufSize);
        std::vector<char> exePath(bufSize > 0 ? bufSize : 1024, '\0');
        if (_NSGetExecutablePath(exePath.data(), &bufSize) != 0)
            return false;

        std::vector<std::string> envStrings;
        if (char** envp = *_NSGetEnviron())
        {
            for (char** entry = envp; *entry != nullptr; ++entry)
            {
                if (strncasecmp(*entry, "COD3_MAP=", 9) == 0 ||
                    strncasecmp(*entry, "COD3_CMD=", 9) == 0 ||
                    strncasecmp(*entry, "COD3_PAD=", 9) == 0 ||
                    strncasecmp(*entry, "COD3_WIN=", 9) == 0 ||
                    strncasecmp(*entry, "COD3_LAUNCHDATA=", 16) == 0)
                {
                    continue;
                }
                envStrings.emplace_back(*entry);
            }
        }
        envStrings.emplace_back("COD3_LAUNCHDATA=" + launchDataFile.string());

        std::vector<char*> envPtrs;
        envPtrs.reserve(envStrings.size() + 1);
        for (std::string& s : envStrings)
            envPtrs.push_back(s.data());
        envPtrs.push_back(nullptr);

        posix_spawn_file_actions_t actions{};
        posix_spawn_file_actions_init(&actions);
        if (g_originalStdoutFd >= 0)
            posix_spawn_file_actions_adddup2(&actions, g_originalStdoutFd, STDOUT_FILENO);
        if (g_originalStderrFd >= 0)
            posix_spawn_file_actions_adddup2(&actions, g_originalStderrFd, STDERR_FILENO);

        char** argv = *_NSGetArgv();
        pid_t child = 0;
        const int rc = posix_spawn(&child, exePath.data(), &actions, nullptr, argv, envPtrs.data());
        posix_spawn_file_actions_destroy(&actions);
        return rc == 0;
    }

    void RegisterExternalReservation(void* base, size_t size, size_t committedOffset, size_t committedSize)
    {
        if (!base || size == 0) return;
        for (size_t i = 0; i < ExternalWords; ++i)
            g_externalCommitted[i].store(0, std::memory_order_relaxed);
        const uintptr_t extBase = reinterpret_cast<uintptr_t>(base);
        if (committedSize > 0 && committedOffset < size)
        {
            const size_t clampedSize = (committedOffset + committedSize <= size)
                ? committedSize
                : (size - committedOffset);
            SetExternalCommitted(extBase, size,
                                 extBase + committedOffset,
                                 extBase + committedOffset + clampedSize,
                                 true);
        }
        g_externalSize.store(size, std::memory_order_release);
        g_externalBase.store(extBase, std::memory_order_release);
    }

    void UnregisterExternalReservation(void* base)
    {
        if (!base) return;
        const uintptr_t extBase = reinterpret_cast<uintptr_t>(base);
        if (g_externalBase.load(std::memory_order_acquire) == extBase)
        {
            g_externalBase.store(0, std::memory_order_release);
            g_externalSize.store(0, std::memory_order_release);
            for (size_t i = 0; i < ExternalWords; ++i)
                g_externalCommitted[i].store(0, std::memory_order_relaxed);
        }
    }

    bool CommitExternalRange(void* lpAddress, size_t dwSize, DWORD flProtect, size_t& outNewlyCommittedBytes) noexcept
    {
        outNewlyCommittedBytes = 0;
        if (!lpAddress || dwSize == 0) return false;
        const size_t pageSize = HostPageSize();
        const uintptr_t start = reinterpret_cast<uintptr_t>(lpAddress);
        const uintptr_t end   = start + dwSize;
        const uintptr_t alignedStart = start & ~(pageSize - 1);
        const uintptr_t alignedEnd   = (end + pageSize - 1) & ~(pageSize - 1);
        const size_t alignedSize     = alignedEnd - alignedStart;

        uintptr_t extBase = 0;
        size_t extSize = 0;
        if (!InExternalReservation(alignedStart, extBase, extSize))
            return false;

        const int prot = WinProtToPosix(flProtect);
        if (mprotect(reinterpret_cast<void*>(alignedStart), alignedSize, prot) != 0)
            return false;

        outNewlyCommittedBytes = SetExternalCommitted(extBase, extSize, alignedStart, alignedEnd, true);
        return true;
    }

    bool IsExternalAddressCommitted(const void* lpAddress) noexcept
    {
        if (!lpAddress) return false;
        const uintptr_t addr = reinterpret_cast<uintptr_t>(lpAddress);
        uintptr_t extBase = 0;
        size_t extSize = 0;
        if (!InExternalReservation(addr, extBase, extSize))
            return false;
        return IsExternalPageCommitted(extBase, addr);
    }

    bool SafeReadMemory(const void* src, void* dst, size_t size) noexcept
    {
        if (!src || !dst || size == 0)
            return false;
        const uintptr_t addr = reinterpret_cast<uintptr_t>(src);
        if (addr < 0x10000u)
            return false;
        mach_vm_size_t outSize = 0;
        const kern_return_t kr = mach_vm_read_overwrite(
            mach_task_self(),
            static_cast<mach_vm_address_t>(addr),
            static_cast<mach_vm_size_t>(size),
            reinterpret_cast<mach_vm_address_t>(dst),
            &outSize);
        return kr == KERN_SUCCESS && outSize == size;
    }
}

// ============================================================================
// Weak fallback stubs for Component 5–7 subsystems until their translation
// units are enabled on macOS. Strong definitions in Component 5–7 automatically
// override these weak symbols at link time.
// ============================================================================
#include "window.h"
#include "input.h"
#include "overlay.h"
#include "audio_out.h"
#include "xma.h"
#include "render.h"
#include "render_internal.h"
#include "render_shaders.h"

#define COD3_WEAK __attribute__((weak))

namespace Window
{
    COD3_WEAK void Open() {}
    COD3_WEAK void Close() {}
    COD3_WEAK bool IsOpen() { return false; }
    COD3_WEAK bool HasFocus() { return false; }
    COD3_WEAK void* Handle() { return nullptr; }
    COD3_WEAK void SetCursorHidden(bool) {}
    COD3_WEAK void SetFrontBuffer(uint32_t, uint32_t, uint32_t) {}
    COD3_WEAK bool TakeRawMouse(long& x, long& y) { x = 0; y = 0; return false; }
    COD3_WEAK int TakeWheel() { return 0; }
}

namespace Input
{
    COD3_WEAK Pad State() { return Pad{}; }
    COD3_WEAK uint16_t Buttons() { return 0; }
    COD3_WEAK bool MouseHeld() { return false; }
    COD3_WEAK uint32_t PacketNumber() { return 0; }
    COD3_WEAK const char* KeyboardHelp() { return ""; }
}

namespace Overlay
{
    COD3_WEAK void NoteFrontEndText(FrontEndText) {}
}

namespace Audio
{
    COD3_WEAK bool Open() { return false; }
    COD3_WEAK void Close() {}
    COD3_WEAK bool SubmitFrame(uint32_t) { return true; }
    COD3_WEAK void Report() {}
}

namespace Xma
{
    struct Stream {};
    COD3_WEAK bool Available() { return false; }
    COD3_WEAK const char* Unavailable() { return "not yet enabled"; }
    COD3_WEAK Stream* Create(int, int) { return nullptr; }
    COD3_WEAK void Destroy(Stream*) {}
    COD3_WEAK bool Decode(Stream*, const uint8_t*, std::vector<int16_t>&) { return false; }
    COD3_WEAK void Reset(Stream*) {}
}

namespace Render
{
    COD3_WEAK bool Enabled() { return false; }
    COD3_WEAK void Draw(uint32_t, uint32_t, uint32_t) {}
    COD3_WEAK void Resolve() {}
    COD3_WEAK void ShaderLoaded(bool, uint32_t, uint32_t, uint32_t) {}
    COD3_WEAK void UseProgram(bool, uint32_t, uint32_t, uint64_t) {}
    COD3_WEAK void Swap(uint32_t, uint32_t, uint32_t) {}
    COD3_WEAK void SetWindow(void*, int, int) {}
    COD3_WEAK void PresentIdle() {}
    COD3_WEAK void MemoryFreed(uint32_t, uint32_t) {}
    COD3_WEAK uint64_t CurrentProgramHash(bool) { return 0; }
    COD3_WEAK void LogNextFrame() {}
    COD3_WEAK void Report() {}
}

namespace RenderInternal
{
    COD3_WEAK bool FrameLogged() { return false; }
    COD3_WEAK uint64_t Swaps() { return 0; }
    COD3_WEAK int DumpNumber() { return 0; }
}

namespace RenderShaders
{
    COD3_WEAK uint64_t HashMicrocode(const uint8_t*, uint32_t) { return 0; }
}

#endif // __APPLE__
