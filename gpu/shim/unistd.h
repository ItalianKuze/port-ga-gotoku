// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: MinGW's <unistd.h> is a stub without gettid()/sysconf(), which the port uses to label
// per-thread statistics and to convert page counts into bytes.
#pragma once

#if defined(__cplusplus)
#define BB_SHIM_INLINE inline
#else
#define BB_SHIM_INLINE static __inline
#endif

#ifdef _WIN32

#include "bbport_windows.h"

#include <process.h>
#include <stdlib.h>
#include <unistd.h>

BB_SHIM_INLINE int gettid(void) { return (int)GetCurrentThreadId(); }

#define _SC_PAGESIZE 30
#define _SC_NPROCESSORS_ONLN 84

BB_SHIM_INLINE long sysconf(int name) {
    switch (name) {
    case _SC_PAGESIZE: {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return (long)si.dwPageSize;
    }
    case _SC_NPROCESSORS_ONLN:
        return (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    default:
        return -1;
    }
}

/// overwrite == 0 keeps an existing value, matching setenv's POSIX behaviour.
BB_SHIM_INLINE int setenv(const char* name, const char* value, int overwrite) {
    if (!overwrite && getenv(name)) {
        return 0;
    }
    return _putenv_s(name, value ? value : "");
}

BB_SHIM_INLINE int unsetenv(const char* name) { return _putenv_s(name, ""); }

#undef BB_SHIM_INLINE

#else

#undef BB_SHIM_INLINE

#include_next <unistd.h>

#endif