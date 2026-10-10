// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: <dlfcn.h> is glibc-only and MinGW-w64 ships neither the header nor dladdr. The port
// uses dladdr only to attribute an address to a module in diagnostics, so map it onto the
// Windows module APIs. Symbol names stay null (no DbgHelp dependency at load time).
#pragma once

#if defined(__cplusplus)
#define BB_SHIM_INLINE inline
#else
#define BB_SHIM_INLINE static __inline
#endif

#ifdef _WIN32

#include "bbport_windows.h"

struct Dl_info {
    const char* dli_fname;
    void* dli_fbase;
    const char* dli_sname;
    void* dli_saddr;
};

BB_SHIM_INLINE int dladdr(const void* addr, struct Dl_info* info) {
    HMODULE module = NULL;
    /* dli_fname needs static storage; the callers read it right after the call. */
    static __thread char path[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (const char*)addr, &module)) {
        return 0;
    }
    if (!GetModuleFileNameA(module, path, MAX_PATH)) {
        return 0;
    }
    info->dli_fname = path;
    info->dli_fbase = (void*)module;
    info->dli_sname = NULL;
    info->dli_saddr = NULL;
    return 1;
}

#undef BB_SHIM_INLINE

#else

#undef BB_SHIM_INLINE

#include_next <dlfcn.h>

#endif