// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: <sys/mman.h> is glibc-only. The port maps and re-protects guest pages: page protection
// maps onto VirtualProtect, and the MAP_SHARED file mappings onto MapViewOfFile.
#pragma once

#ifdef _WIN32

#include "bbport_windows.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
#define BB_SHIM_INLINE inline
#else
#define BB_SHIM_INLINE static __inline
#endif

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED 0x10
#define MAP_FIXED_NOREPLACE 0x100000
/* Windows has no overcommit hint: every mapping commits what it reserves. Accepted and ignored. */
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0x4000
#endif

#define MAP_FAILED ((void*)-1)

BB_SHIM_INLINE DWORD BbPageProtect(int prot) {
    const int write = (prot & PROT_WRITE) != 0;
    const int exec = (prot & PROT_EXEC) != 0;
    if (exec && write) {
        return PAGE_EXECUTE_READWRITE;
    }
    if (exec) {
        return PAGE_EXECUTE_READ;
    }
    return write ? PAGE_READWRITE : PAGE_READONLY;
}

BB_SHIM_INLINE int mprotect(void* addr, size_t len, int prot) {
    DWORD old = 0;
    if (!VirtualProtect(addr, (SIZE_T)len, BbPageProtect(prot), &old)) {
        return -1;
    }
    return 0;
}

BB_SHIM_INLINE void* mmap(void* addr, size_t length, int prot, int flags, int fd,
                          int64_t offset) {
    if (flags & MAP_ANONYMOUS) {
        /* VirtualAlloc only takes a PAGE_* value at reserve time, and writable private memory
         * is the only combination MAP_ANONYMOUS + PROT_READ/WRITE is asked for here. Anything
         * stricter is applied afterwards by mprotect(). */
        DWORD protect = PAGE_READWRITE;
        if (prot == PROT_NONE) {
            protect = PAGE_NOACCESS;
        }
        return VirtualAlloc(addr, length, MEM_RESERVE | MEM_COMMIT, protect);
    }
    HANDLE handle = (HANDLE)(intptr_t)fd;
    if (handle == INVALID_HANDLE_VALUE) {
        return MAP_FAILED;
    }
    const DWORD access = (flags & MAP_SHARED) ? FILE_MAP_ALL_ACCESS : FILE_MAP_COPY;
    return MapViewOfFile(handle, access, (DWORD)((uint64_t)offset >> 32),
                         (DWORD)((uint64_t)offset & 0xffffffffu), (SIZE_T)length);
}

BB_SHIM_INLINE int munmap(void* addr, size_t length) {
    MEMORY_BASIC_INFORMATION mbi;
    (void)length;
    if (!VirtualQuery(addr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        mbi.Type == MEM_MAPPED) {
        return UnmapViewOfFile(addr) ? 0 : -1;
    }
    return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
}

#undef BB_SHIM_INLINE

#else

#include_next <sys/mman.h>

#endif