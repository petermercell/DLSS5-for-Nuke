#pragma once
// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
// Platform compatibility layer for the DLSS5Live Nuke node.
//
// On Windows this header is a thin passthrough to <windows.h>, so the existing
// Win32 code in DLSS5Live.cpp / WorkerBridge.cpp compiles byte-identically.
//
// On Linux it provides the small subset of Win32 that the *node* actually uses
// (file attribute queries, SRW locks, condition variables) mapped onto POSIX,
// plus a portable way to find the directory this shared object was loaded from.
//
// This header deliberately does NOT try to emulate D3D12 or process creation.
// Process creation lives in WorkerBridge_posix.cpp.
// -----------------------------------------------------------------------------

#ifdef _WIN32

#include <windows.h>

#else // ---------------------------------- POSIX ----------------------------------

#include <pthread.h>
#include <sys/stat.h>
#include <climits>
#include <cstdint>
#include <cstring>
#include <string>

#ifndef MAX_PATH
#define MAX_PATH 4096
#endif

#ifndef INFINITE
#define INFINITE 0xFFFFFFFFu
#endif

typedef uint32_t DWORD;
typedef int      BOOL;

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

// ---- File attributes -------------------------------------------------------
#define INVALID_FILE_ATTRIBUTES   ((DWORD)0xFFFFFFFF)
#define FILE_ATTRIBUTE_DIRECTORY  ((DWORD)0x00000010)
#define FILE_ATTRIBUTE_NORMAL     ((DWORD)0x00000080)

inline DWORD GetFileAttributesA(const char* path) {
    if (!path || !*path) return INVALID_FILE_ATTRIBUTES;
    struct stat st;
    if (::stat(path, &st) != 0) return INVALID_FILE_ATTRIBUTES;
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
}

// ---- SRW locks -------------------------------------------------------------
// The node uses SRW locks purely to avoid the MSVC std:: ABI in the Nuke
// process. On Linux that concern does not exist, but keeping the same names
// keeps the diff against upstream minimal.
typedef pthread_mutex_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER

inline void AcquireSRWLockExclusive(SRWLOCK* l) { pthread_mutex_lock(l); }
inline void ReleaseSRWLockExclusive(SRWLOCK* l) { pthread_mutex_unlock(l); }
// No reader/writer split on the POSIX path: a plain mutex is correct, just
// slightly less parallel for the isRunning() probe.
inline void AcquireSRWLockShared(SRWLOCK* l)    { pthread_mutex_lock(l); }
inline void ReleaseSRWLockShared(SRWLOCK* l)    { pthread_mutex_unlock(l); }

// ---- Condition variables ---------------------------------------------------
typedef pthread_cond_t CONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT PTHREAD_COND_INITIALIZER

inline void WakeAllConditionVariable(CONDITION_VARIABLE* cv) { pthread_cond_broadcast(cv); }
inline void WakeConditionVariable(CONDITION_VARIABLE* cv)    { pthread_cond_signal(cv); }

inline BOOL SleepConditionVariableSRW(CONDITION_VARIABLE* cv, SRWLOCK* l,
                                      DWORD ms, unsigned long flags) {
    (void)flags;
    if (ms == INFINITE) {
        return pthread_cond_wait(cv, l) == 0 ? TRUE : FALSE;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += (time_t)(ms / 1000u);
    ts.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(cv, l, &ts) == 0 ? TRUE : FALSE;
}

#endif // _WIN32

// -----------------------------------------------------------------------------
// Cross-platform helpers (available on both platforms)
// -----------------------------------------------------------------------------
namespace dlss5 {

// Absolute directory containing the DLSS5Live shared library itself, with
// forward slashes and no trailing slash. Empty string if it cannot be resolved.
// Pass the address of any function defined inside the plug-in.
std::string moduleDir(const void* addr_in_module);

// User home directory ($HOME on Linux, %USERPROFILE% on Windows), forward
// slashes, no trailing slash. Empty if unset.
std::string homeDir();

// Resolve a possibly-relative path to an absolute one. Returns the input
// unchanged if resolution fails.
std::string absolutePath(const std::string& path);

} // namespace dlss5
