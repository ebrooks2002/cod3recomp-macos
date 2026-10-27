#pragma once

// Lightweight POSIX / macOS compatibility shim for CoD3Host.
//
// Roughly two dozen translation units in CoD3Host include <Windows.h> only for
// basic helpers such as GetCurrentThreadId(), GetTickCount64(), Sleep(), VK_*
// keycodes, QueryPerformanceCounter(), or VirtualAlloc(MEM_COMMIT). Including
// this header instead keeps those files free of #ifdef _WIN32 clutter while
// providing native Mach / POSIX / Cocoa implementations on macOS ARM64.

#include <chrono>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <timeapi.h>

#else // !_WIN32 (macOS / POSIX)

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <wchar.h>

#ifndef WINAPI
#define WINAPI
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef APIENTRY
#define APIENTRY
#endif

using BYTE      = uint8_t;
using WORD      = uint16_t;
using DWORD     = uint32_t;
using DWORD64   = uint64_t;
using ULONGLONG = uint64_t;
using LONGLONG  = int64_t;
using LONG      = int32_t;
using ULONG     = uint32_t;
using INT       = int32_t;
using UINT      = uint32_t;
using BOOL      = bool;
using BOOLEAN   = uint8_t;
using SIZE_T    = size_t;
using ULONG_PTR = uintptr_t;
using DWORD_PTR = uintptr_t;
using LONG_PTR  = intptr_t;
using HANDLE    = void*;
using HMODULE   = void*;
using HWND      = void*;
using HRESULT   = int32_t;
using LPVOID    = void*;
using LPCVOID   = const void*;
using LPSTR     = char*;
using LPCSTR    = const char*;
using LPWSTR    = wchar_t*;
using LPCWSTR   = const wchar_t*;
using PWSTR     = wchar_t*;
using WPARAM    = uintptr_t;
using LPARAM    = intptr_t;
using LRESULT   = intptr_t;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#ifndef MAX_PATH
#define MAX_PATH 1024
#endif

#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)))

inline constexpr DWORD ERROR_SUCCESS        = 0u;
inline constexpr DWORD ERROR_FILE_NOT_FOUND = 2u;
inline constexpr DWORD ERROR_ACCESS_DENIED  = 5u;

inline constexpr HRESULT S_OK   = 0;
inline constexpr HRESULT E_FAIL = static_cast<HRESULT>(0x80004005u);

#ifndef SUCCEEDED
#define SUCCEEDED(hr) (static_cast<HRESULT>(hr) >= 0)
#endif
#ifndef FAILED
#define FAILED(hr) (static_cast<HRESULT>(hr) < 0)
#endif

inline constexpr UINT CP_UTF8 = 65001u;

// --- Virtual memory constants -----------------------------------------------

inline constexpr DWORD MEM_COMMIT   = 0x00001000u;
inline constexpr DWORD MEM_RESERVE  = 0x00002000u;
inline constexpr DWORD MEM_DECOMMIT = 0x00004000u;
inline constexpr DWORD MEM_RELEASE  = 0x00008000u;
inline constexpr DWORD MEM_FREE     = 0x00010000u;
inline constexpr DWORD MEM_PRIVATE  = 0x00020000u;

inline constexpr DWORD PAGE_NOACCESS          = 0x01u;
inline constexpr DWORD PAGE_READONLY          = 0x02u;
inline constexpr DWORD PAGE_READWRITE         = 0x04u;
inline constexpr DWORD PAGE_WRITECOPY         = 0x08u;
inline constexpr DWORD PAGE_EXECUTE           = 0x10u;
inline constexpr DWORD PAGE_EXECUTE_READ      = 0x20u;
inline constexpr DWORD PAGE_EXECUTE_READWRITE = 0x40u;

// --- File I/O constants -----------------------------------------------------

inline constexpr DWORD GENERIC_READ  = 0x80000000u;
inline constexpr DWORD GENERIC_WRITE = 0x40000000u;

inline constexpr DWORD FILE_SHARE_READ   = 0x00000001u;
inline constexpr DWORD FILE_SHARE_WRITE  = 0x00000002u;
inline constexpr DWORD FILE_SHARE_DELETE = 0x00000004u;

inline constexpr DWORD CREATE_NEW        = 1u;
inline constexpr DWORD CREATE_ALWAYS     = 2u;
inline constexpr DWORD OPEN_EXISTING     = 3u;
inline constexpr DWORD OPEN_ALWAYS       = 4u;
inline constexpr DWORD TRUNCATE_EXISTING = 5u;

inline constexpr DWORD FILE_ATTRIBUTE_NORMAL      = 0x00000080u;
inline constexpr DWORD FILE_FLAG_BACKUP_SEMANTICS = 0x02000000u;
inline constexpr DWORD FILE_FLAG_SEQUENTIAL_SCAN  = 0x08000000u;

inline constexpr DWORD FILE_BEGIN   = 0u;
inline constexpr DWORD FILE_CURRENT = 1u;
inline constexpr DWORD FILE_END     = 2u;

inline constexpr DWORD MOVEFILE_REPLACE_EXISTING = 0x00000001u;

inline constexpr DWORD FORMAT_MESSAGE_IGNORE_INSERTS = 0x00000200u;
inline constexpr DWORD FORMAT_MESSAGE_FROM_SYSTEM    = 0x00001000u;

// --- Display & input constants ----------------------------------------------

inline constexpr int SM_CXSCREEN = 0;
inline constexpr int SM_CYSCREEN = 1;

inline constexpr UINT MAPVK_VK_TO_VSC = 0u;

inline constexpr int VK_LBUTTON    = 0x01;
inline constexpr int VK_RBUTTON    = 0x02;
inline constexpr int VK_CANCEL     = 0x03;
inline constexpr int VK_MBUTTON    = 0x04;
inline constexpr int VK_XBUTTON1   = 0x05;
inline constexpr int VK_XBUTTON2   = 0x06;
inline constexpr int VK_BACK       = 0x08;
inline constexpr int VK_TAB        = 0x09;
inline constexpr int VK_CLEAR      = 0x0C;
inline constexpr int VK_RETURN     = 0x0D;
inline constexpr int VK_SHIFT      = 0x10;
inline constexpr int VK_CONTROL    = 0x11;
inline constexpr int VK_MENU       = 0x12;
inline constexpr int VK_PAUSE      = 0x13;
inline constexpr int VK_CAPITAL    = 0x14;
inline constexpr int VK_ESCAPE     = 0x1B;
inline constexpr int VK_SPACE      = 0x20;
inline constexpr int VK_PRIOR      = 0x21;
inline constexpr int VK_NEXT       = 0x22;
inline constexpr int VK_END        = 0x23;
inline constexpr int VK_HOME       = 0x24;
inline constexpr int VK_LEFT       = 0x25;
inline constexpr int VK_UP         = 0x26;
inline constexpr int VK_RIGHT      = 0x27;
inline constexpr int VK_DOWN       = 0x28;
inline constexpr int VK_SELECT     = 0x29;
inline constexpr int VK_PRINT      = 0x2A;
inline constexpr int VK_EXECUTE    = 0x2B;
inline constexpr int VK_SNAPSHOT   = 0x2C;
inline constexpr int VK_INSERT     = 0x2D;
inline constexpr int VK_DELETE     = 0x2E;
inline constexpr int VK_HELP       = 0x2F;
inline constexpr int VK_NUMPAD0    = 0x60;
inline constexpr int VK_NUMPAD1    = 0x61;
inline constexpr int VK_NUMPAD2    = 0x62;
inline constexpr int VK_NUMPAD3    = 0x63;
inline constexpr int VK_NUMPAD4    = 0x64;
inline constexpr int VK_NUMPAD5    = 0x65;
inline constexpr int VK_NUMPAD6    = 0x66;
inline constexpr int VK_NUMPAD7    = 0x67;
inline constexpr int VK_NUMPAD8    = 0x68;
inline constexpr int VK_NUMPAD9    = 0x69;
inline constexpr int VK_MULTIPLY   = 0x6A;
inline constexpr int VK_ADD        = 0x6B;
inline constexpr int VK_SEPARATOR  = 0x6C;
inline constexpr int VK_SUBTRACT   = 0x6D;
inline constexpr int VK_DECIMAL    = 0x6E;
inline constexpr int VK_DIVIDE     = 0x6F;
inline constexpr int VK_F1         = 0x70;
inline constexpr int VK_F2         = 0x71;
inline constexpr int VK_F3         = 0x72;
inline constexpr int VK_F4         = 0x73;
inline constexpr int VK_F5         = 0x74;
inline constexpr int VK_F6         = 0x75;
inline constexpr int VK_F7         = 0x76;
inline constexpr int VK_F8         = 0x77;
inline constexpr int VK_F9         = 0x78;
inline constexpr int VK_F10        = 0x79;
inline constexpr int VK_F11        = 0x7A;
inline constexpr int VK_F12        = 0x7B;
inline constexpr int VK_F13        = 0x7C;
inline constexpr int VK_F14        = 0x7D;
inline constexpr int VK_F15        = 0x7E;
inline constexpr int VK_F16        = 0x7F;
inline constexpr int VK_F17        = 0x80;
inline constexpr int VK_F18        = 0x81;
inline constexpr int VK_F19        = 0x82;
inline constexpr int VK_F20        = 0x83;
inline constexpr int VK_F21        = 0x84;
inline constexpr int VK_F22        = 0x85;
inline constexpr int VK_F23        = 0x86;
inline constexpr int VK_F24        = 0x87;
inline constexpr int VK_NUMLOCK    = 0x90;
inline constexpr int VK_SCROLL     = 0x91;
inline constexpr int VK_LSHIFT     = 0xA0;
inline constexpr int VK_RSHIFT     = 0xA1;
inline constexpr int VK_LCONTROL   = 0xA2;
inline constexpr int VK_RCONTROL   = 0xA3;
inline constexpr int VK_LMENU      = 0xA4;
inline constexpr int VK_RMENU      = 0xA5;
inline constexpr int VK_OEM_1      = 0xBA;
inline constexpr int VK_OEM_PLUS   = 0xBB;
inline constexpr int VK_OEM_COMMA  = 0xBC;
inline constexpr int VK_OEM_MINUS  = 0xBD;
inline constexpr int VK_OEM_PERIOD = 0xBE;
inline constexpr int VK_OEM_2      = 0xBF;
inline constexpr int VK_OEM_3      = 0xC0;
inline constexpr int VK_OEM_4      = 0xDB;
inline constexpr int VK_OEM_5      = 0xDC;
inline constexpr int VK_OEM_6      = 0xDD;
inline constexpr int VK_OEM_7      = 0xDE;
inline constexpr int VK_OEM_8      = 0xDF;
inline constexpr int VK_OEM_102    = 0xE2;

// --- Structures -------------------------------------------------------------

union LARGE_INTEGER
{
    struct
    {
        DWORD LowPart;
        LONG  HighPart;
    };
    struct
    {
        DWORD LowPart;
        LONG  HighPart;
    } u;
    LONGLONG QuadPart;
};

union ULARGE_INTEGER
{
    struct
    {
        DWORD LowPart;
        DWORD HighPart;
    };
    struct
    {
        DWORD LowPart;
        DWORD HighPart;
    } u;
    ULONGLONG QuadPart;
};

struct FILETIME
{
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
};

struct SYSTEMTIME
{
    WORD wYear;
    WORD wMonth;
    WORD wDayOfWeek;
    WORD wDay;
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
    WORD wMilliseconds;
};

struct POINT
{
    LONG x;
    LONG y;
};

struct RECT
{
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
};

struct MEMORY_BASIC_INFORMATION
{
    void*  BaseAddress;
    void*  AllocationBase;
    DWORD  AllocationProtect;
    SIZE_T RegionSize;
    DWORD  State;
    DWORD  Protect;
    DWORD  Type;
};

struct DEVMODEW
{
    DWORD dmSize;
    DWORD dmPelsWidth;
    DWORD dmPelsHeight;
    DWORD dmBitsPerPel;
    DWORD dmDisplayFrequency;
};

struct CONTEXT
{
    DWORD ContextFlags;
    uint64_t Rip;   // PC on ARM64 / x86_64
    uint64_t Rsp;   // SP on ARM64 / x86_64
    uint64_t Rbp;   // FP (x29) on ARM64 / RBP on x86_64
    uint64_t Lr;    // LR (x30) on ARM64
    uint64_t X[31];
};

inline constexpr DWORD CONTEXT_FULL            = 0x00100007u;
inline constexpr DWORD CONTEXT_DEBUG_REGISTERS = 0x00100010u;

// --- Thread & process helpers -----------------------------------------------

inline DWORD GetCurrentThreadId()
{
    thread_local uint32_t cached = 0;
    if (cached == 0)
    {
        uint64_t tid = 0;
        pthread_threadid_np(nullptr, &tid);
        cached = static_cast<uint32_t>(tid);
        if (cached == 0) cached = 1;
    }
    return cached;
}

inline DWORD GetThreadId(pthread_t thread)
{
    if (!thread) return 0;
    uint64_t tid = 0;
    if (pthread_threadid_np(thread, &tid) != 0) return 0;
    return static_cast<DWORD>(tid);
}

inline DWORD GetCurrentProcessId()
{
    return static_cast<DWORD>(getpid());
}

inline HANDLE GetCurrentProcess()
{
    return reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1));
}

inline void Sleep(DWORD milliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

inline BOOL SwitchToThread()
{
    sched_yield();
    return TRUE;
}

inline void YieldProcessor()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    __builtin_arm_yield();
#elif defined(__x86_64__) || defined(_M_X64)
    __builtin_ia32_pause();
#endif
}

#if !defined(__rdtsc) && !defined(PPC_CONTEXT_H_INCLUDED)
inline uint64_t __rdtsc()
{
#if defined(__aarch64__) || defined(_M_ARM64)
    uint64_t val = 0;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#elif defined(__x86_64__) || defined(_M_X64)
    uint32_t lo = 0, hi = 0;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#else
    return 0;
#endif
}
#define __rdtsc __rdtsc
#endif

[[noreturn]] inline void ExitProcess(UINT exitCode)
{
    _exit(static_cast<int>(exitCode));
}

inline void RtlCaptureContext(CONTEXT* ctx)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
#if defined(__aarch64__) || defined(_M_ARM64)
    uint64_t fp = 0, lr = 0, sp = 0;
    asm volatile(
        "mov %0, x29\n"
        "mov %1, x30\n"
        "mov %2, sp\n"
        : "=r"(fp), "=r"(lr), "=r"(sp));
    ctx->Rbp = fp;
    ctx->Lr  = lr;
    ctx->Rip = reinterpret_cast<uint64_t>(__builtin_return_address(0));
    ctx->Rsp = sp;
    ctx->X[29] = fp;
    ctx->X[30] = lr;
#else
    ctx->Rbp = reinterpret_cast<uint64_t>(__builtin_frame_address(0));
    ctx->Rip = reinterpret_cast<uint64_t>(__builtin_return_address(0));
#endif
}

// --- Timing helpers ---------------------------------------------------------

inline ULONGLONG GetTickCount64()
{
    static const auto origin = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::steady_clock::now() - origin;
    return static_cast<ULONGLONG>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

inline DWORD GetTickCount()
{
    return static_cast<DWORD>(GetTickCount64());
}

BOOL QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* lpFrequency);

inline DWORD timeBeginPeriod(UINT) { return 0; }
inline DWORD timeEndPeriod(UINT)   { return 0; }

inline void GetSystemTimeAsFileTime(FILETIME* ft)
{
    if (!ft) return;
    // Windows FILETIME epoch is 1601-01-01; Unix epoch is 1970-01-01.
    constexpr uint64_t EpochDifference100ns = 116444736000000000ULL;
    const auto now = std::chrono::system_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    const uint64_t ticks = static_cast<uint64_t>(ns / 100) + EpochDifference100ns;
    ft->dwLowDateTime  = static_cast<DWORD>(ticks & 0xFFFFFFFFULL);
    ft->dwHighDateTime = static_cast<DWORD>(ticks >> 32);
}

inline void GetSystemTimePreciseAsFileTime(FILETIME* ft)
{
    GetSystemTimeAsFileTime(ft);
}

inline BOOL FileTimeToSystemTime(const FILETIME* ft, SYSTEMTIME* st)
{
    if (!ft || !st) return FALSE;
    const uint64_t ticks = (static_cast<uint64_t>(ft->dwHighDateTime) << 32) | ft->dwLowDateTime;
    if (ticks >= 0x8000000000000000ULL) return FALSE;
    constexpr int64_t EpochDifference100ns = 116444736000000000LL;
    constexpr int64_t TicksPerSecond       = 10000000LL;
    const int64_t signed100ns = static_cast<int64_t>(ticks) - EpochDifference100ns;
    int64_t secs64  = signed100ns / TicksPerSecond;
    int64_t rem100ns = signed100ns % TicksPerSecond;
    if (rem100ns < 0)
    {
        secs64 -= 1;
        rem100ns += TicksPerSecond;
    }
    const time_t secs = static_cast<time_t>(secs64);
    const WORD millis = static_cast<WORD>((rem100ns / 10000LL) % 1000LL);
    struct tm utc{};
    if (!gmtime_r(&secs, &utc)) return FALSE;
    st->wYear         = static_cast<WORD>(utc.tm_year + 1900);
    st->wMonth        = static_cast<WORD>(utc.tm_mon + 1);
    st->wDayOfWeek    = static_cast<WORD>(utc.tm_wday);
    st->wDay          = static_cast<WORD>(utc.tm_mday);
    st->wHour         = static_cast<WORD>(utc.tm_hour);
    st->wMinute       = static_cast<WORD>(utc.tm_min);
    st->wSecond       = static_cast<WORD>(utc.tm_sec);
    st->wMilliseconds = millis;
    return TRUE;
}

inline BOOL SystemTimeToFileTime(const SYSTEMTIME* st, FILETIME* ft)
{
    if (!st || !ft) return FALSE;
    if (st->wYear < 1601 || st->wYear > 30827 ||
        st->wMonth < 1 || st->wMonth > 12 ||
        st->wDay < 1 || st->wDay > 31 ||
        st->wHour > 23 || st->wMinute > 59 ||
        st->wSecond > 59 || st->wMilliseconds > 999)
    {
        return FALSE;
    }
    struct tm utc{};
    utc.tm_year = int(st->wYear) - 1900;
    utc.tm_mon  = int(st->wMonth) - 1;
    utc.tm_mday = int(st->wDay);
    utc.tm_hour = int(st->wHour);
    utc.tm_min  = int(st->wMinute);
    utc.tm_sec  = int(st->wSecond);
    const time_t secs = timegm(&utc);
    struct tm check{};
    if (!gmtime_r(&secs, &check)) return FALSE;
    if (check.tm_year != int(st->wYear) - 1900 ||
        check.tm_mon  != int(st->wMonth) - 1 ||
        check.tm_mday != int(st->wDay) ||
        check.tm_hour != int(st->wHour) ||
        check.tm_min  != int(st->wMinute) ||
        check.tm_sec  != int(st->wSecond))
    {
        return FALSE;
    }
    constexpr int64_t EpochDifference100ns = 116444736000000000LL;
    const int64_t signedTicks = EpochDifference100ns
        + static_cast<int64_t>(secs) * 10000000LL
        + static_cast<int64_t>(st->wMilliseconds) * 10000LL;
    if (signedTicks < 0) return FALSE;
    const uint64_t ticks = static_cast<uint64_t>(signedTicks);
    ft->dwLowDateTime  = static_cast<DWORD>(ticks & 0xFFFFFFFFULL);
    ft->dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    return TRUE;
}

// --- Error helpers ----------------------------------------------------------

inline constexpr DWORD ERROR_IO_DEVICE = 1117u;

inline DWORD GetLastError()
{
    if (errno == 0) return ERROR_SUCCESS;
    if (errno == EACCES || errno == EPERM) return ERROR_ACCESS_DENIED;
    if (errno == ENOENT) return ERROR_FILE_NOT_FOUND;
    if (errno == EIO) return ERROR_IO_DEVICE;
    return static_cast<DWORD>(errno);
}

inline void SetLastError(DWORD err)
{
    if (err == ERROR_SUCCESS)            errno = 0;
    else if (err == ERROR_ACCESS_DENIED) errno = EACCES;
    else if (err == ERROR_FILE_NOT_FOUND) errno = ENOENT;
    else if (err == ERROR_IO_DEVICE)     errno = EIO;
    else                                 errno = static_cast<int>(err);
}

inline DWORD FormatMessageA(DWORD, LPCVOID, DWORD dwMessageId, DWORD, LPSTR lpBuffer, DWORD nSize, va_list*)
{
    if (!lpBuffer || nSize == 0) return 0;
    int posixErr = static_cast<int>(dwMessageId);
    if (dwMessageId == ERROR_ACCESS_DENIED)       posixErr = EACCES;
    else if (dwMessageId == ERROR_FILE_NOT_FOUND) posixErr = ENOENT;
    else if (dwMessageId == ERROR_IO_DEVICE)      posixErr = EIO;
    const char* msg = (dwMessageId == ERROR_SUCCESS) ? "Success" : strerror(posixErr);
    if (!msg) msg = "Unknown error";
    snprintf(lpBuffer, nSize, "%s", msg);
    return static_cast<DWORD>(strlen(lpBuffer));
}

// --- CRT & string compatibility helpers -------------------------------------

inline int _stricmp(const char* a, const char* b)
{
    return strcasecmp(a, b);
}

inline int _strnicmp(const char* a, const char* b, size_t n)
{
    return strncasecmp(a, b, n);
}

inline int _wcsnicmp(const wchar_t* a, const wchar_t* b, size_t n)
{
    return wcsncasecmp(a, b, n);
}

inline int gmtime_s(struct tm* result, const time_t* time)
{
    if (!result || !time) return EINVAL;
    return gmtime_r(time, result) ? 0 : errno;
}

inline int localtime_s(struct tm* result, const time_t* time)
{
    if (!result || !time) return EINVAL;
    return localtime_r(time, result) ? 0 : errno;
}

inline std::string NarrowFromWide(const wchar_t* wide, int count = -1)
{
    if (!wide) return {};
    const size_t len = (count < 0) ? wcslen(wide) : static_cast<size_t>(count);
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i)
    {
        const uint32_t cp = static_cast<uint32_t>(wide[i]);
        if (cp == 0 && count < 0) break;
        if (cp < 0x80)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

inline int WideCharToMultiByte(UINT, DWORD, LPCWSTR lpWideCharStr, int cchWideChar,
                               LPSTR lpMultiByteStr, int cbMultiByte, LPCSTR, BOOL*)
{
    if (!lpWideCharStr) return 0;
    const bool nullTerminated = (cchWideChar < 0);
    const std::string utf8 = NarrowFromWide(lpWideCharStr, cchWideChar);
    const int needed = static_cast<int>(utf8.size()) + (nullTerminated ? 1 : 0);
    if (!lpMultiByteStr || cbMultiByte <= 0) return needed;
    const int toCopy = std::min(static_cast<int>(utf8.size()),
                                nullTerminated ? (cbMultiByte - 1) : cbMultiByte);
    if (toCopy > 0) memcpy(lpMultiByteStr, utf8.data(), static_cast<size_t>(toCopy));
    if (nullTerminated) lpMultiByteStr[toCopy] = '\0';
    return nullTerminated ? (toCopy + 1) : toCopy;
}

// Overloaded for both const char* (std::filesystem::path::c_str() on POSIX)
// and const wchar_t* so call sites using path.c_str() compile unmodified.
inline FILE* _wfopen(const char* path, const wchar_t* mode)
{
    const std::string m = NarrowFromWide(mode);
    return fopen(path, m.c_str());
}

inline FILE* _wfopen(const wchar_t* path, const wchar_t* mode)
{
    const std::string p = NarrowFromWide(path);
    const std::string m = NarrowFromWide(mode);
    return fopen(p.c_str(), m.c_str());
}

inline int _wfopen_s(FILE** pFile, const char* path, const wchar_t* mode)
{
    if (!pFile) return EINVAL;
    *pFile = _wfopen(path, mode);
    return *pFile ? 0 : (errno ? errno : EIO);
}

inline int _wfopen_s(FILE** pFile, const wchar_t* path, const wchar_t* mode)
{
    if (!pFile) return EINVAL;
    *pFile = _wfopen(path, mode);
    return *pFile ? 0 : (errno ? errno : EIO);
}

// --- Memory & VM helpers (implemented in platform_macos.mm) -----------------

void*  VirtualAlloc(void* lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
BOOL   VirtualProtect(void* lpAddress, SIZE_T dwSize, DWORD flNewProtect, DWORD* lpflOldProtect);
BOOL   VirtualFree(void* lpAddress, SIZE_T dwSize, DWORD dwFreeType);
SIZE_T VirtualQuery(const void* lpAddress, MEMORY_BASIC_INFORMATION* lpBuffer, SIZE_T dwLength);
BOOL   ReadProcessMemory(HANDLE hProcess, LPCVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesRead);
void   GetCurrentThreadStackLimits(ULONG_PTR* LowLimit, ULONG_PTR* HighLimit);

// --- File & OS helpers (implemented in platform_macos.mm) -------------------

HANDLE CreateFileW(const char* lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   void* lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
HANDLE CreateFileW(const wchar_t* lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   void* lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
BOOL   CloseHandle(HANDLE hObject);
BOOL   ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
                DWORD* lpNumberOfBytesRead, void* lpOverlapped);
BOOL   WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
                 DWORD* lpNumberOfBytesWritten, void* lpOverlapped);
DWORD  SetFilePointer(HANDLE hFile, LONG lDistanceToMove, LONG* lpDistanceToMoveHigh, DWORD dwMoveMethod);
BOOL   SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove,
                        LARGE_INTEGER* lpNewFilePointer, DWORD dwMoveMethod);
BOOL   GetFileSizeEx(HANDLE hFile, LARGE_INTEGER* lpFileSize);
BOOL   SetEndOfFile(HANDLE hFile);
BOOL   FlushFileBuffers(HANDLE hFile);
BOOL   MoveFileExA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, DWORD dwFlags);
BOOL   DeleteFileA(LPCSTR lpFileName);

// --- Display & keyboard helpers (implemented in platform_macos.mm) ----------

int   GetSystemMetrics(int nIndex);
BOOL  EnumDisplaySettingsW(LPCWSTR lpszDeviceName, DWORD iModeNum, DEVMODEW* lpDevMode);
UINT  MapVirtualKeyW(UINT uCode, UINT uMapType);
int   GetKeyNameTextW(LONG lParam, LPWSTR lpString, int cchSize);
DWORD GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize);

#endif // _WIN32

namespace Platform
{
#ifdef _WIN32
    inline std::filesystem::path ExecutableDirectory()
    {
        wchar_t buffer[MAX_PATH * 4] = {};
        const DWORD length = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
        if (length == 0) return std::filesystem::current_path();
        return std::filesystem::path(buffer).parent_path();
    }

    inline bool InspectJumpBuffer(const jmp_buf& buffer, uintptr_t& outSp, uintptr_t& outIp)
    {
        const auto* frame = reinterpret_cast<const _JUMP_BUFFER*>(&buffer);
        outSp = static_cast<uintptr_t>(frame->Rsp);
        outIp = static_cast<uintptr_t>(frame->Rip);
        return true;
    }
#else
    // Directory containing the running CoD3 host executable.
    std::filesystem::path ExecutableDirectory();

    // Native file/folder picker (NSOpenPanel on macOS).
    bool OpenFileDialog(bool wantFolder, const char* title, std::filesystem::path& chosen);

    // Extracts the stack pointer and instruction pointer from a host jmp_buf
    // (unmunging on macOS ARM64) so Mmio::LongJump can validate the target frame.
    bool InspectJumpBuffer(const jmp_buf& buffer, uintptr_t& outSp, uintptr_t& outIp);

    // Relaunches the current executable with COD3_LAUNCHDATA pointing to launchDataFile.
    bool RelaunchProcess(const std::filesystem::path& launchDataFile);

    // Registers an externally mapped guest address space reservation [base, base + size)
    // and marks [base + committedOffset, base + committedOffset + committedSize)
    // as initially committed in a lock-free, async-signal-safe bitmap.
    void RegisterExternalReservation(void* base, size_t size, size_t committedOffset, size_t committedSize);
    void UnregisterExternalReservation(void* base);

    // Commits [lpAddress, lpAddress + dwSize) inside the registered external
    // reservation and returns true if mprotect succeeded. outNewlyCommittedBytes
    // receives only the bytes that atomically transitioned from uncommitted to
    // committed on this call, preventing concurrent fault handlers from
    // double-counting overlapping demand commits.
    bool CommitExternalRange(void* lpAddress, size_t dwSize, DWORD flProtect, size_t& outNewlyCommittedBytes) noexcept;
    bool IsExternalAddressCommitted(const void* lpAddress) noexcept;

    inline int g_originalStdoutFd = -1;
    inline int g_originalStderrFd = -1;

    inline void RecordOriginalStdioFds(int stdoutFd, int stderrFd)
    {
        g_originalStdoutFd = stdoutFd;
        g_originalStderrFd = stderrFd;
    }
#endif
}
