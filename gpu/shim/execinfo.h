// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: <execinfo.h> is glibc-only. The port uses backtrace() purely to label stack samples in
// diagnostics; RtlCaptureStackBackTrace is the ntdll call MinGW exposes the same way.
#pragma once

#if defined(__cplusplus)
#define BB_SHIM_INLINE inline
#else
#define BB_SHIM_INLINE static __inline
#endif

#ifdef _WIN32

#include "bbport_windows.h"

#include <stdio.h>

/// Fills up to size frames with return addresses; returns how many were captured.
BB_SHIM_INLINE int backtrace(void** buffer, int size) {
    if (size <= 0) {
        return 0;
    }
    /* Skip this wrapper itself so the first frame is the caller. RtlCaptureStackBackTrace is
       declared by winnt.h (a macro alias of CaptureStackBackTrace). */
    return (int)RtlCaptureStackBackTrace(1, (DWORD)size, (PVOID*)buffer, NULL);
}

/// glibc resolves each frame to "module(symbol+offset)"; without a symbolizer print the raw
/// addresses so the scheduler's contention report still identifies the stack depth.
BB_SHIM_INLINE void backtrace_symbols_fd(void* const* buffer, int size, int fd) {
    int i;
    (void)fd;
    fprintf(stderr, "backtrace (%d frames):", size);
    for (i = 0; i < size; ++i) {
        fprintf(stderr, " %p", buffer[i]);
    }
    fprintf(stderr, "\n");
}

#undef BB_SHIM_INLINE

#else

#undef BB_SHIM_INLINE

#include_next <execinfo.h>

#endif