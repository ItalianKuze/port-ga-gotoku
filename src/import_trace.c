/* PGG_IMPORT_TRACE=<file>: every HLE import call made by the game's main thread, with its first
 * four arguments and its return value, to compare two builds call by call.
 *
 * Each resolved function import is bound to a 20-byte stub instead (mov r11d, index; jmp common).
 * The common thunk saves the System V argument registers, moves the guest's return address to a
 * per-thread shadow stack (so stack arguments stay where the callee expects them), calls the real
 * function and logs it on return. Every thread goes through the thunk; only the thread that bound
 * the imports (the one that enters the game) is logged. Imports that do not return normally
 * (setjmp/longjmp, exit) keep their direct binding. */
#define _GNU_SOURCE
#include "runtime.h"
#include "../gpu/shim/pgg_env.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

#define TRACE_ABI __attribute__((sysv_abi))
enum { MAX_IMPORTS = 8192, STUB = 32, DEPTH = 512 };
typedef struct { uintptr_t target; const char *nid, *symbol; int string_arg; } Import;
typedef struct { uintptr_t ret; uint32_t index; uint64_t args[4]; } Frame;
static Import imports[MAX_IMPORTS];
static uint32_t import_count;
static unsigned char *stubs;
static FILE *out;
static long main_tid;
static uint64_t sequence;
static __thread Frame frames[DEPTH];
static __thread int depth;

static long trace_tid(void) {
#ifdef _WIN32
    return (long)GetCurrentThreadId();
#else
    return (long)syscall(SYS_gettid);
#endif
}
/* Copies a guest string without faulting on a bad pointer. */
static void safe_string(uint64_t address, char *buffer, size_t size) {
    buffer[0] = 0;
    if (address < 0x10000) return;
#ifdef _WIN32
    SIZE_T got = 0;
    ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)address, buffer, size - 1, &got);
    buffer[got] = 0;
#else
    struct iovec local = {buffer, size - 1}, remote = {(void *)(uintptr_t)address, size - 1};
    ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    buffer[got > 0 ? got : 0] = 0;
#endif
    for (char *p = buffer; *p; ++p) if (*p < 32 || *p > 126) { *p = 0; break; }
}

TRACE_ABI uintptr_t import_trace_enter(uint64_t index, uintptr_t ret, const uint64_t *regs) {
    /* regs: r11, r9, r8, rcx, rdx, rsi, rdi, rax as pushed by the thunk. */
    if (depth >= DEPTH) { fputs("STOP: import trace shadow stack overflow\n", stderr); _exit(21); }
    Frame *f = &frames[depth++];
    f->ret = ret; f->index = (uint32_t)index;
    f->args[0] = regs[6]; f->args[1] = regs[5]; f->args[2] = regs[4]; f->args[3] = regs[3];
    return imports[index].target;
}
TRACE_ABI uintptr_t import_trace_exit(uint64_t rax) {
    Frame *f = &frames[--depth];
    if (out && trace_tid() == main_tid) {
        const Import *im = &imports[f->index];
        char text[96];
        safe_string(im->string_arg ? f->args[0] : 0, text, sizeof(text));
        fprintf(out, "%llu %s(%#llx, %#llx, %#llx, %#llx)%s%s%s = %#llx\n", (unsigned long long)++sequence,
                im->symbol ? im->symbol : im->nid,
                (unsigned long long)f->args[0], (unsigned long long)f->args[1],
                (unsigned long long)f->args[2], (unsigned long long)f->args[3],
                text[0] ? " \"" : "", text, text[0] ? "\"" : "", (unsigned long long)rax);
    }
    return f->ret;
}

__asm__(
    ".text\n"
    ".globl import_trace_common\n"
    "import_trace_common:\n"
    "    popq %r10\n"                      /* guest return address */
    "    pushq %rax\n    pushq %rdi\n    pushq %rsi\n    pushq %rdx\n"
    "    pushq %rcx\n    pushq %r8\n     pushq %r9\n     pushq %r11\n"
    "    subq $128, %rsp\n"
    "    movdqu %xmm0, 0(%rsp)\n   movdqu %xmm1, 16(%rsp)\n  movdqu %xmm2, 32(%rsp)\n  movdqu %xmm3, 48(%rsp)\n"
    "    movdqu %xmm4, 64(%rsp)\n  movdqu %xmm5, 80(%rsp)\n  movdqu %xmm6, 96(%rsp)\n  movdqu %xmm7, 112(%rsp)\n"
    "    movq %r11, %rdi\n"
    "    movq %r10, %rsi\n"
    "    leaq 128(%rsp), %rdx\n"
    "    call import_trace_enter\n"
    "    movq %rax, %r10\n"
    "    movdqu 0(%rsp), %xmm0\n   movdqu 16(%rsp), %xmm1\n  movdqu 32(%rsp), %xmm2\n  movdqu 48(%rsp), %xmm3\n"
    "    movdqu 64(%rsp), %xmm4\n  movdqu 80(%rsp), %xmm5\n  movdqu 96(%rsp), %xmm6\n  movdqu 112(%rsp), %xmm7\n"
    "    addq $128, %rsp\n"
    "    popq %r11\n    popq %r9\n     popq %r8\n     popq %rcx\n"
    "    popq %rdx\n    popq %rsi\n    popq %rdi\n    popq %rax\n"
    "    callq *%r10\n"                    /* the callee sees the caller's stack arguments unchanged */
    "    pushq %rax\n    pushq %rdx\n"
    "    subq $16, %rsp\n"
    "    movdqu %xmm0, 0(%rsp)\n"
    "    movq %rax, %rdi\n"
    "    call import_trace_exit\n"
    "    movq %rax, %r10\n"
    "    movdqu 0(%rsp), %xmm0\n"
    "    addq $16, %rsp\n"
    "    popq %rdx\n    popq %rax\n"
    "    pushq %r10\n"
    "    ret\n");
extern char import_trace_common[];

static int bypass(const char *symbol) {
    return symbol && (strstr(symbol, "setjmp") || strstr(symbol, "longjmp") || strstr(symbol, "exit") ||
                      strstr(symbol, "Exit"));
}
static int wants_string(const char *symbol) {
    return symbol && (strstr(symbol, "Open") || strstr(symbol, "open") || strstr(symbol, "Stat") ||
                      strstr(symbol, "stat") || strstr(symbol, "Unlink") || strstr(symbol, "Mkdir") ||
                      strstr(symbol, "Prx") || strstr(symbol, "Dlsym") || strstr(symbol, "access"));
}
/* The address to bind a function import to: a logging stub, or target itself when tracing is
 * off or the import cannot go through the thunk. */
uintptr_t import_trace_wrap(const char *nid, uintptr_t target) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *path = pgg_getenv("IMPORT_TRACE");
        enabled = path && *path;
        if (!enabled) return target;
        out = fopen(path, "w");
        if (!out) { fprintf(stderr, "Import trace: cannot open %s\n", path); enabled = 0; return target; }
        setvbuf(out, NULL, _IOLBF, 0);
        main_tid = trace_tid();
#ifdef _WIN32
        stubs = VirtualAlloc(NULL, (size_t)MAX_IMPORTS * STUB, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
        stubs = mmap(NULL, (size_t)MAX_IMPORTS * STUB, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stubs == MAP_FAILED) stubs = NULL;
#endif
        if (!stubs) { fputs("Import trace: no executable memory\n", stderr); enabled = 0; return target; }
        fprintf(stderr, "Import trace: main thread %ld, writing %s\n", main_tid, path);
    }
    if (!enabled || !target) return target;
    const char *symbol = runtime_symbol(nid);
    if (bypass(symbol)) return target;
    for (uint32_t i = 0; i < import_count; ++i)
        if (imports[i].target == target && !strcmp(imports[i].nid, nid)) return (uintptr_t)(stubs + (size_t)i * STUB);
    if (import_count == MAX_IMPORTS) return target;
    const uint32_t i = import_count++;
    imports[i] = (Import){target, strdup(nid), symbol, wants_string(symbol)};
    unsigned char *s = stubs + (size_t)i * STUB;
    const uint64_t common = (uint64_t)(uintptr_t)import_trace_common;
    s[0] = 0x41; s[1] = 0xbb; memcpy(s + 2, &i, 4);              /* mov r11d, i */
    s[6] = 0xff; s[7] = 0x25; memset(s + 8, 0, 4);               /* jmp [rip+0] */
    memcpy(s + 12, &common, 8);
    return (uintptr_t)s;
}
