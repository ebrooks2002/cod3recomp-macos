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

namespace
{
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

    int HandleToFd(HANDLE h)
    {
        if (h == nullptr || h == INVALID_HANDLE_VALUE) return -1;
        return static_cast<int>(reinterpret_cast<intptr_t>(h) - 1);
    }

    HANDLE FdToHandle(int fd)
    {
        if (fd < 0) return INVALID_HANDLE_VALUE;
        return reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd) + 1);
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
    const size_t pageSize = HostPageSize();
    const int prot = (flAllocationType & MEM_COMMIT) ? WinProtToPosix(flProtect) : PROT_NONE;

    if (lpAddress == nullptr)
    {
        const size_t alignedSize = (dwSize + pageSize - 1) & ~(pageSize - 1);
        void* mapped = mmap(nullptr, alignedSize, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mapped == MAP_FAILED) return nullptr;
        return mapped;
    }

    const uintptr_t start = reinterpret_cast<uintptr_t>(lpAddress);
    const uintptr_t end   = start + dwSize;
    const uintptr_t alignedStart = start & ~(pageSize - 1);
    const uintptr_t alignedEnd   = (end + pageSize - 1) & ~(pageSize - 1);
    const size_t alignedSize     = alignedEnd - alignedStart;

    if (flAllocationType & MEM_COMMIT)
    {
        if (mprotect(reinterpret_cast<void*>(alignedStart), alignedSize, prot) != 0)
            return nullptr;
    }
    return lpAddress;
}

BOOL VirtualProtect(void* lpAddress, SIZE_T dwSize, DWORD flNewProtect, DWORD* lpflOldProtect)
{
    if (!lpAddress || dwSize == 0) return FALSE;

    if (lpflOldProtect)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(lpAddress, &mbi, sizeof(mbi)) != 0)
            *lpflOldProtect = mbi.Protect;
        else
            *lpflOldProtect = PAGE_READWRITE;
    }

    const size_t pageSize = HostPageSize();
    const uintptr_t start = reinterpret_cast<uintptr_t>(lpAddress);
    const uintptr_t end   = start + dwSize;
    const int prot = WinProtToPosix(flNewProtect);

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

    const uintptr_t alignedStart = start & ~(pageSize - 1);
    const uintptr_t alignedEnd   = (end + pageSize - 1) & ~(pageSize - 1);
    return mprotect(reinterpret_cast<void*>(alignedStart), alignedEnd - alignedStart, prot) == 0 ? TRUE : FALSE;
}

BOOL VirtualFree(void* lpAddress, SIZE_T dwSize, DWORD dwFreeType)
{
    if (!lpAddress) return FALSE;
    const size_t pageSize = HostPageSize();

    if (dwFreeType & MEM_RELEASE)
    {
        if (dwSize == 0) return TRUE;
        const uintptr_t start = reinterpret_cast<uintptr_t>(lpAddress) & ~(pageSize - 1);
        const size_t alignedSize = (dwSize + pageSize - 1) & ~(pageSize - 1);
        return munmap(reinterpret_cast<void*>(start), alignedSize) == 0 ? TRUE : FALSE;
    }

    if ((dwFreeType & MEM_DECOMMIT) && dwSize > 0)
    {
        const uintptr_t start = (reinterpret_cast<uintptr_t>(lpAddress) + pageSize - 1) & ~(pageSize - 1);
        const uintptr_t end   = (reinterpret_cast<uintptr_t>(lpAddress) + dwSize) & ~(pageSize - 1);
        if (end > start)
        {
            madvise(reinterpret_cast<void*>(start), end - start, MADV_DONTNEED);
            mprotect(reinterpret_cast<void*>(start), end - start, PROT_NONE);
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
    // Pages reserved with PROT_NONE report MEM_RESERVE; pages committed with
    // read/write/exec permissions report MEM_COMMIT.
    lpBuffer->State             = (info.protection != VM_PROT_NONE) ? MEM_COMMIT : MEM_RESERVE;
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
    int flags = 0;
    if (read && write) flags = O_RDWR;
    else if (write)    flags = O_WRONLY;
    else               flags = O_RDONLY;

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
            flags = O_RDONLY | O_DIRECTORY;
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

        char** argv = *_NSGetArgv();
        pid_t child = 0;
        return posix_spawn(&child, exePath.data(), nullptr, nullptr, argv, envPtrs.data()) == 0;
    }
}

#endif // __APPLE__
