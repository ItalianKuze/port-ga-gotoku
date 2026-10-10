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
 *   runtime_fault_recover  VEH recovery point; a CONTEXT on Windows, a sigjmp_buf elsewhere
 *
 * The guest-memory primitives themselves are the ones from Supermedo's Windows port: the PS4 user
 * range is reserved once with a VirtualAlloc2 placeholder and carved with MapViewOfFile3, so a
 * physical range maps to a fixed guest address the game can store in its 40-bit pointers.
 */
#define _WIN32_WINNT 0x0A00 /* VirtualAlloc2 / MapViewOfFile3 / CreateFileMapping2 (Win10 2004+) */
#include <windows.h>

#include "runtime.h"

/* Speculative readers of guest memory (GPU draw-preparation workers) register a recovery point
 * (probe.c's VEH): a fault on that thread restores the stored CONTEXT instead of terminating. */
__thread CONTEXT *runtime_fault_recover;
/* BB_EXIT_TRACE=1: when the process ends through exit(), _exit() or ExitProcess(), print the
 * terminating thread's stack -- host frames as module+offset, then every stack word that points
 * into a loaded image -- so an exit that leaves no message of its own can be traced to its caller.
 * TerminateProcess from another thread does not come through here. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    const char *trace = getenv("BB_EXIT_TRACE");
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
