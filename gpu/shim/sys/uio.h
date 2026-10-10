// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: <sys/uio.h> is glibc-only. The port uses it for process_vm_readv against itself, which
// reads this process's own address space; ReadProcessMemory on the pseudo-handle is the direct
// Windows equivalent.
#pragma once

#if defined(__cplusplus)
#define BB_SHIM_INLINE inline
#else
#define BB_SHIM_INLINE static __inline
#endif

#ifdef _WIN32

#include "bbport_windows.h"

#include <stddef.h>
#include <sys/types.h>

struct iovec {
    void* iov_base;
    size_t iov_len;
};

BB_SHIM_INLINE ssize_t process_vm_readv(int pid, const struct iovec* local, unsigned long liovcnt,
                                        const struct iovec* remote, unsigned long riovcnt,
                                        unsigned long long flags) {
    ssize_t total = 0;
    unsigned long i;
    (void)pid;
    (void)flags;
    for (i = 0; i < liovcnt && i < riovcnt; ++i) {
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), remote[i].iov_base, local[i].iov_base,
                               (SIZE_T)local[i].iov_len, &got)) {
            /* Partial results are how the callers detect a fault mid-range. */
            break;
        }
        total += (ssize_t)got;
        if (got != (SIZE_T)local[i].iov_len) {
            break;
        }
    }
    return total;
}

BB_SHIM_INLINE ssize_t process_vm_writev(int pid, const struct iovec* local, unsigned long liovcnt,
                                         const struct iovec* remote, unsigned long riovcnt,
                                         unsigned long long flags) {
    ssize_t total = 0;
    unsigned long i;
    (void)pid;
    (void)flags;
    for (i = 0; i < liovcnt && i < riovcnt; ++i) {
        SIZE_T put = 0;
        if (!WriteProcessMemory(GetCurrentProcess(), remote[i].iov_base, local[i].iov_base,
                                (SIZE_T)local[i].iov_len, &put)) {
            break;
        }
        total += (ssize_t)put;
        if (put != (SIZE_T)local[i].iov_len) {
            break;
        }
    }
    return total;
}

#undef BB_SHIM_INLINE

#else

#undef BB_SHIM_INLINE

#include_next <sys/uio.h>

#endif