/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The Windows-only state the HLE runtime needs.
 *
 * Everything else -- the guest memory backend (direct pool, flexible memory, reservations,
 * protection, the PS4 address range reservation), the per-service import resolvers, guest errno
 * and the GPU memory hooks -- is the same code as the Linux build; it lives in runtime_memory.c
 * and friends behind small #ifdef _WIN32 branches. This file holds only what has no portable
 * equivalent:
 *
 *   runtime_setjmp, runtime_recover_arm  recovery points for speculative guest reads (VEH)
 *
 * The guest-memory primitives themselves are the ones from Supermedo's Windows port: the PS4 user
 * range is reserved once with a VirtualAlloc2 placeholder and carved with MapViewOfFile3, so a
 * physical range maps to a fixed guest address the game can store in its 40-bit pointers.
 */
#define _WIN32_WINNT 0x0A00 /* VirtualAlloc2 / MapViewOfFile3 / CreateFileMapping2 (Win10 2004+) */
#include <windows.h>

#include "runtime.h"

/* Recovery points for speculative reads of guest memory (gpu/shim/bbport_recover.h). The layout of
 * BbRecoverBuf and runtime_setjmp are the Mrsuss60 Windows port's (runtime_host.c): rbx rbp rdi rsi
 * r12-r15 at 0..56, rsp after the return at 64, the return address at 72, xmm6-xmm15 at 80..224,
 * MXCSR at 240, the x87 control word at 244. The VEH restores them into the CONTEXT. */
_Static_assert(sizeof(BbRecoverBuf) == 256 && _Alignof(BbRecoverBuf) == 16, "BbRecoverBuf layout");
__asm__(".text\n"
        ".globl runtime_setjmp\n"
        ".def runtime_setjmp; .scl 2; .type 32; .endef\n"
        ".p2align 4\n"
        "runtime_setjmp:\n"
        "  movq %rbx, 0(%rcx)\n  movq %rbp, 8(%rcx)\n  movq %rdi, 16(%rcx)\n  movq %rsi, 24(%rcx)\n"
        "  movq %r12, 32(%rcx)\n  movq %r13, 40(%rcx)\n  movq %r14, 48(%rcx)\n  movq %r15, 56(%rcx)\n"
        "  leaq 8(%rsp), %rdx\n  movq %rdx, 64(%rcx)\n  movq (%rsp), %rdx\n  movq %rdx, 72(%rcx)\n"
        "  movups %xmm6, 80(%rcx)\n  movups %xmm7, 96(%rcx)\n  movups %xmm8, 112(%rcx)\n"
        "  movups %xmm9, 128(%rcx)\n  movups %xmm10, 144(%rcx)\n  movups %xmm11, 160(%rcx)\n"
        "  movups %xmm12, 176(%rcx)\n  movups %xmm13, 192(%rcx)\n  movups %xmm14, 208(%rcx)\n"
        "  movups %xmm15, 224(%rcx)\n  stmxcsr 240(%rcx)\n  fnstcw 244(%rcx)\n"
        "  xorl %eax, %eax\n  ret\n");
static __thread BbRecoverBuf *recover_point;
void runtime_recover_arm(BbRecoverBuf *buf) { recover_point = buf; }
BbRecoverBuf *runtime_recover_take(void) {
    BbRecoverBuf *buf = recover_point;
    recover_point = NULL;
    return buf;
}
/* PGG_EXIT_TRACE=1: when the process ends through exit(), _exit() or ExitProcess(), print the
 * terminating thread's stack -- host frames as module+offset, then every stack word that points
 * into a loaded image -- so an exit that leaves no message of its own can be traced to its caller.
 * TerminateProcess from another thread does not come through here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gpu/shim/pgg_env.h"
static void exit_trace_address(const char *what, void *address) {
    HMODULE module = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)address, &module) && module) {
        GetModuleFileNameA(module, name, sizeof name);
        const char *base = strrchr(name, '\\');
        fprintf(stderr, "  %s %p %s+0x%llx\n", what, address, base ? base + 1 : name,
                (unsigned long long)((uintptr_t)address - (uintptr_t)module));
    } else {
        fprintf(stderr, "  %s %p (no module: guest code or JIT)\n", what, address);
    }
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    if (reason != DLL_PROCESS_DETACH || !reserved) return TRUE; /* reserved != NULL: process exit */
    const char *trace = pgg_getenv("EXIT_TRACE");
    if (!trace || strcmp(trace, "1")) return TRUE;
    fprintf(stderr, "Exit trace: process ending on thread %lu\n", GetCurrentThreadId());
    void *frames[48];
    USHORT n = CaptureStackBackTrace(0, 48, frames, NULL);
    for (USHORT i = 0; i < n; ++i) exit_trace_address("frame", frames[i]);
    /* Guest frames have no unwind data: scan the raw stack for code addresses instead. */
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    uintptr_t *sp = (uintptr_t *)&frames, *top = (uintptr_t *)tib->StackBase;
    int shown = 0;
    for (; sp < top && shown < 40; ++sp) {
        MEMORY_BASIC_INFORMATION m;
        if (*sp < 0x10000 || !VirtualQuery((void *)*sp, &m, sizeof m)) continue;
        if (m.State != MEM_COMMIT || !(m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) continue;
        exit_trace_address("stack", (void *)*sp);
        ++shown;
    }
    fflush(stderr);
    return TRUE;
}
