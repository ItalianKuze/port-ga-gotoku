// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: <sys/resource.h> is glibc-only. The port reads two things from it: per-process minor
// faults and per-thread CPU time / context switches. Windows exposes CPU time through
// GetProcessTimes/GetThreadTimes and page faults through GetProcessMemoryInfo; context-switch
// counters are not exposed per thread and stay zero.
#pragma once

#if defined(__cplusplus)
#define BB_SHIM_INLINE inline
#else
#define BB_SHIM_INLINE static __inline
#endif

#ifdef _WIN32

#include "bbport_windows.h"

#include <psapi.h>

#include <time.h>

#define RUSAGE_SELF 0
#define RUSAGE_THREAD 1

struct timeval_ {
    long tv_sec;
    long tv_usec;
};

struct rusage {
    struct timeval_ ru_utime, ru_stime;
    long ru_maxrss;
    long ru_ixrss, ru_idrss, ru_isrss;
    long ru_minflt, ru_majflt;
    long ru_nswap, ru_inblock, ru_oublock;
    long ru_msgsnd, ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw, ru_nivcsw;
};

BB_SHIM_INLINE void BbFillTimes(const FILETIME& kernel, const FILETIME& user, struct rusage* usage) {
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    usage->ru_utime.tv_sec = (long)(u.QuadPart / 10000000ull);
    usage->ru_utime.tv_usec = (long)((u.QuadPart / 10ull) % 1000000ull);
    usage->ru_stime.tv_sec = (long)(k.QuadPart / 10000000ull);
    usage->ru_stime.tv_usec = (long)((k.QuadPart / 10ull) % 1000000ull);
}

BB_SHIM_INLINE int getrusage(int who, struct rusage* usage) {
    FILETIME creation, exit_, kernel, user;
    if (who == RUSAGE_THREAD) {
        if (!GetThreadTimes(GetCurrentThread(), &creation, &exit_, &kernel, &user)) {
            return -1;
        }
    } else if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_, &kernel, &user)) {
        return -1;
    }
    BbFillTimes(kernel, user, usage);
    if (who == RUSAGE_SELF) {
        PROCESS_MEMORY_COUNTERS pmc;
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            usage->ru_minflt = (long)pmc.PageFaultCount;
            usage->ru_majflt = (long)pmc.PageFaultCount;
        }
    }
    return 0;
}

#undef BB_SHIM_INLINE

#else

#undef BB_SHIM_INLINE

#include_next <sys/resource.h>

#endif