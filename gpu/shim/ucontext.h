// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: <ucontext.h> is glibc-only. The guest fault paths (bbgpu_handle_fault, the free-check
// trap, page_manager's write tracking) read and rewrite registers through uc_mcontext.gregs in
// the glibc layout. Windows delivers faults through a VEH with a CONTEXT instead, so this maps
// between the two: build a glibc-layout context to hand the handlers, then copy any register
// they changed back so returning EXCEPTION_CONTINUE_EXECUTION resumes with them.
//
// The gregs index order below is the glibc x86-64 layout (REG_R8..REG_CR2).
#pragma once

#ifdef _WIN32

#include "bbport_windows.h"

#include <stdint.h>

#if defined(__cplusplus)
#define BB_SHIM_INLINE inline
#else
#define BB_SHIM_INLINE static __inline
#endif

typedef long long greg_t;

enum {
    REG_R8 = 0,
    REG_R9,
    REG_R10,
    REG_R11,
    REG_R12,
    REG_R13,
    REG_R14,
    REG_R15,
    REG_RDI,
    REG_RSI,
    REG_RBP,
    REG_RBX,
    REG_RDX,
    REG_RAX,
    REG_RCX,
    REG_RSP,
    REG_RIP,
    REG_EFL,
    REG_CSGSFS,
    REG_ERR,
    REG_TRAPNO,
    REG_OLDMASK,
    REG_CR2
};

struct ucontext_mcontext {
    greg_t gregs[23];
};

struct ucontext_s {
    unsigned long uc_flags;
    struct ucontext_mcontext uc_mcontext;
};

typedef struct ucontext_s ucontext_t;

/// Builds the glibc-layout view of a Windows exception context. REG_ERR and REG_TRAPNO get what
/// the Linux kernel puts there for a page fault, so handlers test `gregs[REG_ERR] & 2` for a write
/// on both systems: an access violation's ExceptionInformation[0] is 0 for a read, 1 for a write
/// and 8 for an instruction fetch (DEP), which are x86 error-code bits 1 (W) and 4 (I).
BB_SHIM_INLINE void BbUcontextFromWindows(const CONTEXT* in, const EXCEPTION_RECORD* record,
                                          ucontext_t* out) {
    greg_t* g = out->uc_mcontext.gregs;
    int i;
    for (i = 0; i < 23; ++i) {
        g[i] = 0;
    }
    g[REG_R8] = (greg_t)in->R8;
    g[REG_R9] = (greg_t)in->R9;
    g[REG_R10] = (greg_t)in->R10;
    g[REG_R11] = (greg_t)in->R11;
    g[REG_R12] = (greg_t)in->R12;
    g[REG_R13] = (greg_t)in->R13;
    g[REG_R14] = (greg_t)in->R14;
    g[REG_R15] = (greg_t)in->R15;
    g[REG_RDI] = (greg_t)in->Rdi;
    g[REG_RSI] = (greg_t)in->Rsi;
    g[REG_RBP] = (greg_t)in->Rbp;
    g[REG_RBX] = (greg_t)in->Rbx;
    g[REG_RDX] = (greg_t)in->Rdx;
    g[REG_RAX] = (greg_t)in->Rax;
    g[REG_RCX] = (greg_t)in->Rcx;
    g[REG_RSP] = (greg_t)in->Rsp;
    g[REG_RIP] = (greg_t)in->Rip;
    g[REG_EFL] = (greg_t)in->EFlags;
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
        record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) {
        const ULONG_PTR kind = record->NumberParameters ? record->ExceptionInformation[0] : 0;
        g[REG_ERR] = 0x4 | (kind == 1 ? 0x2 : 0) | (kind == 8 ? 0x10 : 0); /* user | W | I */
        g[REG_TRAPNO] = 14;                                                 /* #PF */
    }
}

/// Copies back the registers a handler rewrote; only the architectural ones are restored.
BB_SHIM_INLINE void BbUcontextToWindows(const ucontext_t* in, CONTEXT* out) {
    const greg_t* g = in->uc_mcontext.gregs;
    out->R8 = (DWORD64)g[REG_R8];
    out->R9 = (DWORD64)g[REG_R9];
    out->R10 = (DWORD64)g[REG_R10];
    out->R11 = (DWORD64)g[REG_R11];
    out->R12 = (DWORD64)g[REG_R12];
    out->R13 = (DWORD64)g[REG_R13];
    out->R14 = (DWORD64)g[REG_R14];
    out->R15 = (DWORD64)g[REG_R15];
    out->Rdi = (DWORD64)g[REG_RDI];
    out->Rsi = (DWORD64)g[REG_RSI];
    out->Rbp = (DWORD64)g[REG_RBP];
    out->Rbx = (DWORD64)g[REG_RBX];
    out->Rdx = (DWORD64)g[REG_RDX];
    out->Rax = (DWORD64)g[REG_RAX];
    out->Rcx = (DWORD64)g[REG_RCX];
    out->Rsp = (DWORD64)g[REG_RSP];
    out->Rip = (DWORD64)g[REG_RIP];
    out->EFlags = (DWORD)g[REG_EFL];
}

#undef BB_SHIM_INLINE

#else

#include_next <ucontext.h>

#endif