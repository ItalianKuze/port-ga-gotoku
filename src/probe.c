/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <cpuid.h>
#include "runtime.h"
#include "gpu/bbgpu.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include "ucontext.h" /* gpu/shim: glibc-layout context mapping for the VEH */
#else
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static uint64_t image_span; /* mapped bytes of the guest image, for the watchdog */
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
int vulkan_smoke(void);
#ifdef _WIN32
static PVOID veh_handle;
static HANDLE watchdog_thread;
static DWORD WINAPI watchdog_proc(LPVOID seconds);
#endif

uintptr_t import_trace_wrap(const char *nid, uintptr_t target);
static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }

/* The game's code was compiled for the PS4's CPU (AMD Jaguar) and runs as it is: AVX, BMI1
 * (andn/bextr/blsr/tzcnt), MOVBE, LZCNT and POPCNT, thousands of each in eboot.bin. A CPU without
 * them stops at the first one with SIGILL (exit code 132, issue #26), and lzcnt/tzcnt even run as
 * bsr/bsf there, with other results. Said before the game starts; BB_SKIP_CPU_CHECK=1 skips it. */
static void check_cpu(void) {
    const char *skip = getenv("BB_SKIP_CPU_CHECK");
    if (skip && !strcmp(skip, "1")) return;
    unsigned a, b, c, d, leaf1_c = 0, leaf7_b = 0, ext1_c = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) leaf1_c = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) leaf7_b = b;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d)) ext1_c = c;
    int avx = (leaf1_c & bit_AVX) && (leaf1_c & bit_OSXSAVE);
    if (avx) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        avx = (lo & 6) == 6; /* the OS saves SSE and AVX state */
    }
    const struct { int present; const char *name; } features[] = {
        {avx, "AVX"}, {(leaf7_b & bit_BMI) != 0, "BMI1"}, {(leaf1_c & bit_MOVBE) != 0, "MOVBE"},
        {(ext1_c & bit_LZCNT) != 0, "LZCNT"}, {(leaf1_c & bit_POPCNT) != 0, "POPCNT"},
        {(leaf1_c & bit_SSE4_2) != 0, "SSE4.2"},
    };
    char missing[64] = "";
    for (unsigned i = 0; i < sizeof(features) / sizeof(features[0]); ++i) {
        if (features[i].present) continue;
        if (missing[0]) strcat(missing, ", ");
        strcat(missing, features[i].name);
    }
    if (!missing[0]) return;
    fprintf(stderr,
            "ERROR: this CPU lacks %s. Bloodborne's code was compiled for the PS4's CPU and uses "
            "these instructions directly: it needs an Intel Haswell (4th generation Core, 2013) or "
            "newer, or an AMD Ryzen. BB_SKIP_CPU_CHECK=1 starts anyway.\n", missing);
    exit(1);
}
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
#ifndef _WIN32
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
#endif
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)image;
    if (entered_game)
        printf("Guest eboot executed after entry transfer; import call at +0x%" PRIxPTR "\n",caller);
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,m==0 ? "libc.prx" : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
#ifdef _WIN32
    /* Not _exit: it goes through ExitProcess, whose DLL detach runs while GPU, audio and guest
     * threads still hold driver and loader state -- the process stayed listed, exited but stuck
     * in the kernel with ~1.9 GB, and could not be killed. TerminateProcess ends it at once, as
     * the watchdog does. */
    TerminateProcess(GetCurrentProcess(), 20);
#endif
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
#ifndef _WIN32
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
#ifndef _WIN32
static void dump_fault_mapping(uintptr_t address, const char *label) {
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[512];
    int found = 0;
    while (maps && fgets(line, sizeof(line), maps)) {
        unsigned long from, to;
        if (sscanf(line, "%lx-%lx", &from, &to) == 2 && address >= from && address < to) {
            fprintf(stderr, "  %s mapping: %s", label, line);
            found = 1;
            break;
        }
    }
    if (maps) fclose(maps);
    if (!found) fprintf(stderr, "  %s mapping: none\n", label);
}
static void fault(int sig, siginfo_t *info, void *context) {
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (gpu_enabled && sig == SIGSEGV) {
        ucontext_t *signal_context = context;
        greg_t before[NGREG];
        memcpy(before, signal_context->uc_mcontext.gregs, sizeof(before));
        if (bbgpu_handle_fault(context, info->si_addr)) return;
        fputs(memcmp(before, signal_context->uc_mcontext.gregs, sizeof(before)) == 0
                  ? "  GPU fault dispatch returned unhandled; signal registers unchanged\n"
                  : "  GPU fault dispatch returned unhandled after changing signal registers\n",
              stderr);
    }
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *uc = context;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    char line[512];
    Dl_info where;
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (rip - (uintptr_t)image < 0x10000000) {
        snprintf(line, sizeof(line),
                 "  si_code=%d page_error=%#llx trap=%#llx rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx r8=%#llx r9=%#llx r14=%#llx rsp=%#llx rbp=%#llx\n",
                 info->si_code,
                 (unsigned long long)uc->uc_mcontext.gregs[REG_ERR],
                 (unsigned long long)uc->uc_mcontext.gregs[REG_TRAPNO],
                 (unsigned long long)uc->uc_mcontext.gregs[REG_RAX],
                 (unsigned long long)uc->uc_mcontext.gregs[REG_RBX],
                 (unsigned long long)uc->uc_mcontext.gregs[REG_RCX],
                 (unsigned long long)uc->uc_mcontext.gregs[REG_RDX],
                 (unsigned long long)uc->uc_mcontext.gregs[REG_RSI],
                 (unsigned long long)uc->uc_mcontext.gregs[REG_RDI],
                 (unsigned long long)uc->uc_mcontext.gregs[REG_R8],
                 (unsigned long long)uc->uc_mcontext.gregs[REG_R9],
                 (unsigned long long)uc->uc_mcontext.gregs[REG_R14],
                 (unsigned long long)uc->uc_mcontext.gregs[REG_RSP],
                 (unsigned long long)uc->uc_mcontext.gregs[REG_RBP]);
        { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
        unsigned char code[16];
        struct iovec local = {code, sizeof(code)}, remote = {(void *)rip, sizeof(code)};
        const ssize_t code_size = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
        if (code_size > 0) {
            char bytes[16 * 3 + 1];
            size_t used = 0;
            for (ssize_t i = 0; i < code_size && used + 4 < sizeof(bytes); ++i)
                used += (size_t)snprintf(bytes + used, sizeof(bytes) - used, "%02x%s", code[i],
                                         i + 1 == code_size ? "" : " ");
            bytes[used] = '\0';
            fprintf(stderr, "  bytes at RIP: %s\n", bytes);
        } else {
            fprintf(stderr, "  bytes at RIP: unreadable\n");
        }
        dump_fault_mapping(rip, "RIP");
        dump_fault_mapping((uintptr_t)info->si_addr, "si_addr");
    }
    if (gpu_enabled) bbgpu_dump_guest_writes(context);
    /* Outside the image and any shared object (generated code, a freed mapping): the mapping
     * from /proc/self/maps, and the thread. */
    if (rip - (uintptr_t)image >= 0x10000000 && !(dladdr((void *)rip, &where) && where.dli_fname)) {
        char thread[32] = "?";
        pthread_getname_np(pthread_self(), thread, sizeof(thread));
        snprintf(line, sizeof(line), "  thread %s; mapping of RIP: ", thread);
        { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
        FILE *maps = fopen("/proc/self/maps", "r");
        int found = 0;
        while (maps && fgets(line, sizeof(line), maps)) {
            unsigned long from, to;
            if (sscanf(line, "%lx-%lx", &from, &to) == 2 && rip >= from && rip < to) {
                ssize_t written_=write(2, line, strlen(line)); (void)written_;
                found = 1;
                break;
            }
        }
        if (maps) fclose(maps);
        if (!found) { ssize_t written_=write(2, "none\n", 5); (void)written_; }
    }
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    _exit(128 + sig);
}
#else
/* Windows: the guest and the host share one address space, so a guest fault is an ordinary
 * exception. The VEH mirrors fault()'s Linux chain, in the same order:
 *   1. GPU page tracking (bbgpu_handle_fault) resolves the fault if it owns the page;
 *   2. a speculative guest read unwinds to its recovery point (runtime_fault_recover);
 *   3. otherwise the fault is fatal and is reported before exiting.
 * Handlers rewrite registers through a glibc-layout ucontext; the changes are copied back into
 * the CONTEXT so EXCEPTION_CONTINUE_EXECUTION resumes with them. */
static int veh_reporting;
static volatile LONG veh_recoveries;
/* IsBadReadPtr deliberately faults on inaccessible memory and catches the access violation with
 * SEH; a VEH runs before that handler. Host window hooks (e.g. DRM) call it during window
 * creation, so a fault inside it is left to Windows instead of being reported as fatal. The
 * function's extent comes from its unwind data; without it nothing is exempted. As in the
 * Mrsuss60 Windows port (69b954f, src/win32_exception.h). */
static int fault_in_read_probe(uintptr_t rip) {
    HMODULE kernel = GetModuleHandleA("kernel32.dll");
    FARPROC probe = kernel ? GetProcAddress(kernel, "IsBadReadPtr") : NULL;
    if (!probe) return 0;
    DWORD64 base = 0;
    PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry((DWORD64)(uintptr_t)probe, &base, NULL);
    return entry && rip >= base + entry->BeginAddress && rip < base + entry->EndAddress;
}

static LONG WINAPI veh(EXCEPTION_POINTERS *ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT *ctx = ep->ContextRecord;
    void *address = (void *)(uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
    ucontext_t uc;
    BbUcontextFromWindows(ctx, ep->ExceptionRecord, &uc);
    const greg_t before[23];
    memcpy(before, uc.uc_mcontext.gregs, sizeof(before));

    if (gpu_enabled && code == EXCEPTION_ACCESS_VIOLATION) {
        if (bbgpu_handle_fault(&uc, address)) {
            BbUcontextToWindows(&uc, ctx);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (!veh_reporting) {
            veh_reporting = 1;
            fputs(memcmp(before, uc.uc_mcontext.gregs, sizeof(before)) == 0
                      ? "  GPU fault dispatch returned unhandled; context registers unchanged\n"
                      : "  GPU fault dispatch returned unhandled after changing registers\n",
                  stderr);
            veh_reporting = 0;
        }
    }
    /* Speculative guest read (gpu/shim/bbport_recover.h): resume at its recovery point, as if
     * runtime_setjmp returned 1 -- the saved callee-saved registers, stack and return address go
     * into the CONTEXT; the OS restores it on EXCEPTION_CONTINUE_EXECUTION (the Windows
     * equivalent of siglongjmp from the Linux signal handler). */
    BbRecoverBuf *recover = runtime_recover_take();
    if (recover) {
        const unsigned char *r = recover->registers;
        memcpy(&ctx->Rbx, r + 0, 8); memcpy(&ctx->Rbp, r + 8, 8);
        memcpy(&ctx->Rdi, r + 16, 8); memcpy(&ctx->Rsi, r + 24, 8);
        memcpy(&ctx->R12, r + 32, 8); memcpy(&ctx->R13, r + 40, 8);
        memcpy(&ctx->R14, r + 48, 8); memcpy(&ctx->R15, r + 56, 8);
        memcpy(&ctx->Rsp, r + 64, 8); memcpy(&ctx->Rip, r + 72, 8);
        M128A *xmm = &ctx->Xmm6;
        for (int i = 0; i < 10; ++i) memcpy(&xmm[i], r + 80 + 16 * i, 16);
        memcpy(&ctx->MxCsr, r + 240, 4); ctx->FltSave.MxCsr = ctx->MxCsr;
        memcpy(&ctx->FltSave.ControlWord, r + 244, 2);
        ctx->Rax = 1;
        const LONG n = InterlockedIncrement(&veh_recoveries);
        if (n <= 5 || n % 1000 == 0) {
            char line[200];
            snprintf(line, sizeof(line), "Runtime: speculative read fault recovered (#%ld): RIP %p, address %p\n",
                     (long)n, (void *)(uintptr_t)ep->ExceptionRecord->ExceptionAddress, address);
            DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)strlen(line), &w, NULL);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION && fault_in_read_probe((uintptr_t)ctx->Rip))
        return EXCEPTION_CONTINUE_SEARCH; /* IsBadReadPtr's own SEH handles it */
    if (veh_reporting) return EXCEPTION_CONTINUE_SEARCH; /* fault inside the reporter */
    veh_reporting = 1;

    uintptr_t rip = (uintptr_t)ctx->Rip;
    char line[512];
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (exception 0x%lx) at guest offset 0x%lx, address %p\n",
                 (unsigned long)code, (unsigned long)(rip - (uintptr_t)image), address);
    else
        snprintf(line, sizeof(line), "Host fault (exception 0x%lx) at RIP %p, address %p\n",
                 (unsigned long)code, (void *)rip, address);
    { DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)strlen(line), &w, NULL); }
    if (rip - (uintptr_t)image < 0x10000000) {
        snprintf(line, sizeof(line),
                 "  rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx r8=%#llx r9=%#llx "
                 "r14=%#llx rsp=%#llx rbp=%#llx\n",
                 (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx,
                 (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx,
                 (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi,
                 (unsigned long long)ctx->R8, (unsigned long long)ctx->R9,
                 (unsigned long long)ctx->R14, (unsigned long long)ctx->Rsp,
                 (unsigned long long)ctx->Rbp);
        { DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)strlen(line), &w, NULL); }
        unsigned char code_bytes[16];
        SIZE_T got = 0;
        if (ReadProcessMemory(GetCurrentProcess(), (void *)rip, code_bytes, sizeof(code_bytes), &got) &&
            got > 0) {
            char bytes[16 * 3 + 1];
            size_t used = 0;
            for (SIZE_T i = 0; i < got && used + 4 < sizeof(bytes); ++i)
                used += (size_t)snprintf(bytes + used, sizeof(bytes) - used, "%02x%s",
                                         code_bytes[i], i + 1 == got ? "" : " ");
            bytes[used] = '\0';
            fprintf(stderr, "  bytes at RIP: %s\n", bytes);
        }
    }
    /* Host fault: name the module the faulting RIP belongs to and find the guest frames below it,
     * so a crash inside a system DLL can be traced back to the guest call that reached it. */
    if (rip - (uintptr_t)image >= 0x10000000) {
        HMODULE owner = NULL;
        { /* RtlPcToFileHeader resolves the image base for an address without walking the PEB. */
            typedef PVOID (WINAPI *RtlPcToFileHeaderProc)(PVOID, PVOID *);
            static RtlPcToFileHeaderProc pc_to_file;
            if (!pc_to_file) {
                HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
                if (ntdll) pc_to_file = (RtlPcToFileHeaderProc)GetProcAddress(ntdll, "RtlPcToFileHeader");
            }
            if (pc_to_file) pc_to_file((void *)rip, (PVOID *)&owner);
        }
        char name[MAX_PATH] = "?";
        if (owner) {
            char shortname[MAX_PATH];
            if (GetModuleFileNameA((HMODULE)owner, shortname, MAX_PATH)) {
                char *slash = strrchr(shortname, '\\');
                snprintf(name, sizeof(name), "%s", slash ? slash + 1 : shortname);
            }
            snprintf(line, sizeof(line), "  in %s+0x%lx (image base %p)\n",
                     name, (unsigned long)(rip - (uintptr_t)owner), (void *)owner);
        } else {
            snprintf(line, sizeof(line), "  not inside any loaded module (image base %p)\n", (void *)owner);
        }
        { DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)strlen(line), &w, NULL); }

        snprintf(line, sizeof(line),
                 "  rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx r8=%#llx r9=%#llx "
                 "r14=%#llx rsp=%#llx rbp=%#llx\n",
                 (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx,
                 (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx,
                 (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi,
                 (unsigned long long)ctx->R8, (unsigned long long)ctx->R9,
                 (unsigned long long)ctx->R14, (unsigned long long)ctx->Rsp,
                 (unsigned long long)ctx->Rbp);
        { DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)strlen(line), &w, NULL); }
    }
    /* Guest return addresses still on the stack: the call chain that got here, for host and guest
     * faults alike. The loader treats "below image + 0x10000000" as inside the guest image. */
    {
        const uintptr_t *sp = (const uintptr_t *)(uintptr_t)ctx->Rsp;
        int shown = 0;
        line[0] = '\0';
        snprintf(line, sizeof(line), "  guest frames on stack:");
        for (int i = 0; i < 512 && (uintptr_t)(sp + i + 1) < (uintptr_t)ctx->Rsp + 0x8000; ++i) {
            uintptr_t v = sp[i];
            if (v - (uintptr_t)image < 0x10000000) {
                char b[32];
                snprintf(b, sizeof(b), " +0x%lx,", (unsigned long)(v - (uintptr_t)image));
                strncat(line, b, sizeof(line) - strlen(line) - 1);
                if (++shown == 12) break;
            }
        }
        if (shown) strncat(line, "  (guest offsets, most recent first)", sizeof(line) - strlen(line) - 1);
        strncat(line, "\n", sizeof(line) - strlen(line) - 1);
        { DWORD w; WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)strlen(line), &w, NULL); }
    }
    if (gpu_enabled) bbgpu_dump_guest_writes(&uc);
    fflush(NULL);
    /* Not _exit: ExitProcess runs DLL detach while GPU, audio and guest threads still hold driver
     * and loader state, and the process stayed listed, exited but stuck in the kernel (2026-10-11,
     * one per crash). TerminateProcess ends it at once, as the STOP path and the watchdog do. */
    TerminateProcess(GetCurrentProcess(), 128 + (UINT)code);
    _exit(128 + (int)code);
}

/* Watchdog thread: on timeout, report where every other thread is and exit. The Linux build
 * signals each thread into its own stack dumper; here the threads are suspended and their
 * contexts read directly, which gives the same information for a hang. */
/* Everything below runs while other threads may be stopped holding the loader lock, the heap
 * lock or a stdio lock, so it takes none: output is formatted by hand into a static buffer and
 * written with WriteFile, memory is read with ReadProcessMemory (no fault on bad pointers),
 * modules come from the PEB loader list read the same way, thread names from
 * NtQueryInformationThread into a static buffer. */
static HANDLE wd_err;
static char wd_line[512];
static size_t wd_len;
static void wd_str(const char *s) { while (*s && wd_len < sizeof(wd_line) - 1) wd_line[wd_len++] = *s++; }
static void wd_hex(uint64_t v) {
    char b[19]; int n = 0;
    do { b[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    wd_str("0x");
    while (n && wd_len < sizeof(wd_line) - 1) wd_line[wd_len++] = b[--n];
}
static void wd_dec(uint64_t v) {
    char b[21]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && wd_len < sizeof(wd_line) - 1) wd_line[wd_len++] = b[--n];
}
static void wd_flush(void) { DWORD w; wd_line[wd_len++] = '\n'; WriteFile(wd_err, wd_line, (DWORD)wd_len, &w, NULL); wd_len = 0; }
static int wd_read(uintptr_t address, void *out, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)address, out, size, &got) && got == size;
}
typedef struct { uintptr_t base, size; char name[48]; } WdModule;
static WdModule wd_modules[256];
static size_t wd_module_count;
/* Walks PEB->Ldr->InLoadOrderModuleList (x64 layout) and prints modules not seen before. */
static void wd_scan_modules(void) {
    uintptr_t peb = 0, ldr = 0, head, link;
    if (!wd_read((uintptr_t)NtCurrentTeb() + 0x60, &peb, 8) || !wd_read(peb + 0x18, &ldr, 8)) return;
    head = ldr + 0x10;
    if (!wd_read(head, &link, 8)) return;
    for (int guard = 0; link != head && guard < 1024 && wd_module_count < 256; ++guard) {
        uintptr_t base = 0, next = 0, buffer = 0; uint32_t size = 0; uint16_t length = 0;
        if (!wd_read(link, &next, 8) || !wd_read(link + 0x30, &base, 8) || !wd_read(link + 0x40, &size, 4) ||
            !wd_read(link + 0x58, &length, 2) || !wd_read(link + 0x60, &buffer, 8))
            break;
        int known = 0;
        for (size_t i = 0; i < wd_module_count; ++i) if (wd_modules[i].base == base) known = 1;
        if (!known) {
            WdModule *m = &wd_modules[wd_module_count++];
            uint16_t wide[47] = {0};
            size_t chars = length / 2 < 47 ? length / 2 : 47;
            wd_read(buffer, wide, chars * 2);
            for (size_t i = 0; i < chars; ++i) m->name[i] = wide[i] < 128 ? (char)wide[i] : '?';
            m->name[chars] = 0; m->base = base; m->size = size;
            wd_str("  module "); wd_hex(base); wd_str(" +"); wd_hex(size); wd_str(" "); wd_str(m->name); wd_flush();
        }
        link = next;
    }
}
/* guest+offset, module+offset or the bare address. */
static void wd_where(uintptr_t a) {
    if (image && a - (uintptr_t)image < image_span) { wd_str("guest+"); wd_hex(a - (uintptr_t)image); return; }
    for (size_t i = 0; i < wd_module_count; ++i)
        if (a - wd_modules[i].base < wd_modules[i].size) { wd_str(wd_modules[i].name); wd_str("+"); wd_hex(a - wd_modules[i].base); return; }
    wd_hex(a);
}
static int wd_is_code(uintptr_t a) {
    MEMORY_BASIC_INFORMATION info;
    return a > 0x10000 && VirtualQuery((void *)a, &info, sizeof(info)) && info.State == MEM_COMMIT &&
           (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
static uint64_t wd_cpu(HANDLE t) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(t, &c, &e, &k, &u)) return 0;
    return ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime);
}
static DWORD WINAPI watchdog_proc(LPVOID seconds) {
    wd_err = GetStdHandle(STD_ERROR_HANDLE);
    typedef LONG (NTAPI *NtQueryInformationThreadFn)(HANDLE, int, PVOID, ULONG, PULONG);
    NtQueryInformationThreadFn query_thread = (NtQueryInformationThreadFn)(void *)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    wd_str("Watchdog: armed for "); wd_dec(*(const unsigned *)seconds); wd_str(" s; modules now:"); wd_flush();
    wd_scan_modules();
    Sleep(*(const unsigned *)seconds * 1000u); /* seconds points at timeout_seconds */
    wd_str("STOP: watchdog timeout; thread stacks:"); wd_flush();
    wd_str("  modules loaded since:"); wd_flush();
    wd_scan_modules();

    /* Every thread of this process: CPU used over 1 s (busy loop or wait), RIP/RSP/RBP, the first
     * argument registers (rdi: guest System V; rcx/rdx: host Win64 -- the object a thread waits
     * on), the rbp chain (guest code keeps frames), then code addresses found on the stack. */
    enum { MAX_THREADS = 512, WORDS = 2048, FRAMES = 24 };
    static DWORD tids[MAX_THREADS];
    static uint64_t cpu_before[MAX_THREADS];
    static uint64_t words[WORDS];
    static CONTEXT ctx __attribute__((aligned(16)));
    static unsigned char name_buffer[256] __attribute__((aligned(8)));
    size_t thread_count = 0;
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        if (Thread32First(snapshot, &te)) {
            do {
                if (te.th32OwnerProcessID == pid && te.th32ThreadID != self && thread_count < MAX_THREADS)
                    tids[thread_count++] = te.th32ThreadID;
            } while (Thread32Next(snapshot, &te));
        }
        CloseHandle(snapshot);
    }
    for (size_t k = 0; k < thread_count; ++k) {
        HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tids[k]);
        cpu_before[k] = t ? wd_cpu(t) : 0;
        if (t) CloseHandle(t);
    }
    Sleep(1000);
    for (size_t k = 0; k < thread_count; ++k) {
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                              FALSE, tids[k]);
        if (!t) continue;
        uintptr_t chain[FRAMES];
        size_t chain_count = 0;
        SIZE_T got = 0;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        int have = 0;
        uint64_t cpu_after = 0;
        if (SuspendThread(t) != (DWORD)-1) {
            if (GetThreadContext(t, &ctx)) {
                have = 1;
                ReadProcessMemory(GetCurrentProcess(), (void *)(uintptr_t)ctx.Rsp, words, sizeof(words), &got);
                uintptr_t rbp = (uintptr_t)ctx.Rbp, frame[2];
                while (chain_count < FRAMES && rbp && wd_read(rbp, frame, sizeof(frame)) && frame[0] > rbp) {
                    chain[chain_count++] = frame[1];
                    rbp = frame[0];
                }
            }
            cpu_after = wd_cpu(t);
            ResumeThread(t);
        }
        /* ThreadNameInformation (38): a UNICODE_STRING followed by the name. */
        char name[64] = "";
        ULONG returned = 0;
        if (query_thread && query_thread(t, 38, name_buffer, sizeof(name_buffer), &returned) >= 0) {
            const uint16_t length = *(const uint16_t *)name_buffer;
            const uint16_t *wide = *(uint16_t *const *)(name_buffer + 8);
            if (wide && (const unsigned char *)wide >= name_buffer && (const unsigned char *)wide < name_buffer + sizeof(name_buffer)) {
                size_t chars = length / 2 < 63 ? length / 2 : 63;
                for (size_t i = 0; i < chars; ++i) name[i] = wide[i] < 128 ? (char)wide[i] : '?';
                name[chars] = 0;
            }
        }
        CloseHandle(t);
        if (!have) continue;
        wd_str("  tid "); wd_dec(tids[k]); wd_str(" '"); wd_str(name); wd_str("' cpu=");
        wd_dec(cpu_after > cpu_before[k] ? (cpu_after - cpu_before[k]) / 10000 : 0); wd_str("ms/1s");
        wd_str(" rsp="); wd_hex(ctx.Rsp); wd_str(" rbp="); wd_hex(ctx.Rbp);
        wd_str(" rdi="); wd_hex(ctx.Rdi); wd_str(" rcx="); wd_hex(ctx.Rcx); wd_str(" rdx="); wd_hex(ctx.Rdx);
        wd_flush();
        wd_str("     rip   "); wd_where((uintptr_t)ctx.Rip); wd_flush();
        for (size_t i = 0; i < chain_count; ++i) { wd_str("     frame "); wd_where(chain[i]); wd_flush(); }
        int scanned = 0;
        for (size_t i = 0; i < got / 8 && scanned < 20; ++i) {
            if (!wd_is_code((uintptr_t)words[i])) continue; /* stack words that are not code addresses */
            wd_str("     stack "); wd_where((uintptr_t)words[i]); wd_flush();
            ++scanned;
        }
    }
    wd_str("none reported"); wd_flush();
    /* Not fflush/_exit: ExitProcess runs DLL detach under the loader lock and stdio takes stream
     * locks, either of which a hung thread may hold. The streams are unbuffered (bbport_main). */
    TerminateProcess(GetCurrentProcess(), 128 + 14); /* SIGALRM */
    return 0;
}
#endif
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t exe_base;
#ifndef _WIN32
/* POSIX-only frame walking and signal-driven watchdog. Windows uses watchdog_proc() and the VEH above. */
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)uc->uc_mcontext.gregs[REG_RIP], rbp=(uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,(uint64_t)uc->uc_mcontext.gregs[REG_RDI]);
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        /* Resolve the frame to a library+symbol so a stalled run names the
           blocking call (e.g. a GPU condition_variable.wait). dladdr only
           reads the link map of already-loaded objects; the interrupted
           thread is parked in a condvar, not inside the loader. */
        Dl_info where;
        if (dladdr((void *)rip, &where) && where.dli_fname) {
            char sym[192];
            int n=snprintf(sym, sizeof(sym), "     -> %s+0x%lx (%s)\n", where.dli_fname,
                           (unsigned long)(rip-(uintptr_t)where.dli_fbase),
                           where.dli_sname ? where.dli_sname : "?");
            if (n>0) { ssize_t w=write(2,sym,(size_t)n); (void)w; }
        }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
    dump_frames(context);
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
    usleep(100000);
    _exit(128 + sig);
}
#endif /* !_WIN32 */
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
/* BBPATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"BBPATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
volatile int runtime_restarting;
void runtime_restart(void) {
    fflush(NULL);
    puts("Runtime: restarting through run.sh");
    /* The GPU threads still run until exec: their Vulkan calls fail once the device fd is
     * closed below, and an assertion there must not end the process (exit 23) before exec. */
    runtime_restarting = 1;
    __sync_synchronize();
#ifndef _WIN32
    syscall(SYS_close_range, 3u, ~0u, 0u);
    execlp("bash", "bash", "run.sh", (char *)NULL);
    perror("runtime_restart: exec");
    _exit(1);
#endif
}

/* On Linux this is the process entry point. On Windows the loader lives in bbport_runtime.dll
 * (libbbgpu.dll imports the runtime_* API from it), so bb-probe.exe calls this by name. */
#ifdef _WIN32
__declspec(dllexport) int bbport_main(int argc, char **argv) {
#else
int main(int argc, char **argv) {
#endif
    /* Unbuffered: a hang ended by the watchdog (_exit) must not lose the last lines. On Windows
     * the runtime, libbbgpu and bb-probe share ucrtbase.dll, so this covers all of them. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#ifndef _WIN32
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
#ifdef _WIN32
    /* The PS4 user range must be reserved before anything lands in it: the guest image is mapped
     * at an address inside [USER_MIN,USER_MAX), and Vulkan drivers take their allocations from
     * anywhere else. Reserving first turns the range into a placeholder the memory backend carves
     * up (see runtime_memory.c), instead of colliding with what is already there. */
    runtime_memory_reserve();
#endif
    check_cpu();
    int cpu_only = 0, strict_imports = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--timeout seconds] | --vulkan-only\n", argv[0]);
        return 1;
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"BBCONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    {
        /* Every folder this run reads or writes, so a log shows nothing comes from elsewhere. */
        char cwd[1024] = "";
        if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
        const char *xdg = getenv("XDG_DATA_HOME");
        printf("Runtime: folders: boot image %s; app0 %s; saves/user %s; data (XDG_DATA_HOME) %s; cwd %s\n",
               argv[1], app0 ? app0 : "(none)", user_dir ? user_dir : "user", xdg ? xdg : "(unset)", cwd);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        char sfo[4096], id[16]="";
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        int attributes_valid=sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        runtime_kernel_configure(attributes,attributes_valid);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
    } else {
        runtime_kernel_configure(0,0);
    }
    bbgpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    /* Register the fault chain above the image is mapped; AddVectoredExceptionHandler is
     * consulted before the SEH chain, so guest faults reach it before anything unwinds. */
    veh_handle = AddVectoredExceptionHandler(1, veh);
    if (!veh_handle) fail("AddVectoredExceptionHandler failed");
    if (timeout_seconds) watchdog_thread = CreateThread(NULL, 0, watchdog_proc, &timeout_seconds, 0, NULL);
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
#endif
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "BBPROBE1", 8) && memcmp(magic, "BBPROBE2", 8) && memcmp(magic,"BBPROBE3",8) && memcmp(magic,"BBPROBE4",8) && memcmp(magic,"BBPROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "BBPROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"BBPROBE5",8);
    int linked=!memcmp(magic,"BBPROBE3",8) || !memcmp(magic,"BBPROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* BBPROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"BBPROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    image = allocate(round_page(size));
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (!cpu_only) {
        char title[128]="Bloodborne", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        BbGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (bbgpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,bbgpu_symbol_count());
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        uintptr_t handler = (uintptr_t)unresolved;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    /* BB_IMPORT_REPORT=<file>: how every referenced import was bound (runtime HLE, the game's own
     * libc/module export, or nothing -- an unresolved import stops the game when it is called). */
    unsigned char *bound = calloc(import_count ? import_count : 1, 1); /* 0 unused, 1 none, 2 runtime, 3 native */
    unsigned char *bound_kind = calloc(import_count ? import_count : 1, 1);
    if (!bound || !bound_kind) fail("allocation failed");
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            /* BB_IMPORT_TRACE: function imports go through a logging stub (import_trace.c). */
            if (resolved && relocs[i].kind == 1 && !relocs[i].addend) resolved = import_trace_wrap(names[relocs[i].value], resolved);
            uint32_t index = (uint32_t)relocs[i].value;
            bound_kind[index] = (unsigned char)relocs[i].kind;
            if (resolved) { value = resolved + relocs[i].addend; bound[index] = 2; }
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
                if (bound[index] != 2) bound[index] = 3;
            } else if (!bound[index]) bound[index] = 1;
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    const char *report_path = getenv("BB_IMPORT_REPORT");
    if (report_path && *report_path) {
        FILE *report = fopen(report_path, "w");
        if (!report) fprintf(stderr, "Import report: cannot open %s\n", report_path);
        else {
            static const char *how[] = {"unused", "UNRESOLVED", "runtime", "native"};
            unsigned counts[4] = {0};
            for (uint64_t i = 0; i < import_count; ++i) {
                ++counts[bound[i]];
                if (bound[i])
                    fprintf(report, "%s\t%s\t%s\t%s\n", names[i], bound_kind[i] == 2 ? "data" : "function",
                            how[bound[i]], runtime_import_name(names[i]));
            }
            fclose(report);
            printf("Import report: %s (%u runtime, %u native, %u unresolved)\n", report_path, counts[2], counts[3], counts[1]);
        }
    }
    free(bound); free(bound_kind);
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    if (!cpu_only) bbgpu_patch_image(image, size);
    protect(traps, round_page((import_count + 1) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    image_span = round_page(size);
    fprintf(stderr, "Probe regions: image [%p,%p), trampolines [%p,%p), data traps [%p,%p) PROT_NONE\n",
            image, image + round_page(size), traps,
            traps + round_page((import_count + 1) * 32), data_traps,
            data_traps + (import_count + 1) * page_size);
    for (uint64_t i = 0; i < ns; ++i)
        fprintf(stderr, "  guest segment +%#" PRIx64 "..+%#" PRIx64 " flags=%#" PRIx64 " host [%p,%p)\n",
                segments[i].address, segments[i].address + segments[i].size, segments[i].flags,
                image + segments[i].address, image + segments[i].address + segments[i].size);
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
#ifdef _WIN32
    /* link_modules.py rewrote the eboot's initial-exec `mov rax, fs:[0]` to `mov rax, gs:[0]`
     * so the guest TCB is not read from the host's FS. On Linux the runtime then points GS at
     * the TCB with arch_prctl; Windows x64 cannot move GS at all, so the runtime stores the TCB
     * in a TEB TLS slot instead (runtime_thread.c) and the displacement has to be that slot.
     * The slot index is only known now, so stamp it into every rewritten instruction: same 9
     * bytes, `65 48 8b 04 25 <disp32>`, pointing at TEB.TlsSlots[slot]. */
    {
        const uint32_t disp = runtime_thread_tcb_offset();
        static const uint8_t gs_load[9] = { 0x65, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0 };
        uint64_t patched = 0;
        for (uint64_t i = 0; i < ns; ++i) {
            if (!(segments[i].flags & 1)) continue;   /* executable segments only */
            unsigned char *a = image + segments[i].address, *b = a + segments[i].size;
            /* The guest image is mapped R+X (no write), so the displacement bytes cannot be
             * stamped in place; make the segment writable for the pass and restore it after. */
            DWORD old_protect = 0;
            if (!VirtualProtect(a, segments[i].size, PAGE_EXECUTE_READWRITE, &old_protect))
                fail("making a guest segment writable to bind its TLS displacement");
            for (unsigned char *p = a; p + 9 <= b; ++p) {
                if (memcmp(p, gs_load, 5)) continue;
                p[5] = (unsigned char)(disp);       p[6] = (unsigned char)(disp >> 8);
                p[7] = (unsigned char)(disp >> 16); p[8] = (unsigned char)(disp >> 24);
                ++patched;
            }
            DWORD ignored = 0;
            VirtualProtect(a, segments[i].size, old_protect, &ignored);
        }
        printf("Guest TCB: TEB TLS slot displacement 0x%" PRIx32 "; rewrote %" PRIu64 " gs-relative TLS loads\n",
               disp, patched);
        if (!patched) fail("no guest TLS loads found to bind to the Windows TEB TLS slot");
    }
#endif
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
#ifdef _WIN32
    typedef void (ABI *Entry)(void *, void (ABI *)(void));
    ((Entry)(image + entry))(&params, guest_exit);
#else
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-64);
#endif
    fail("entry unexpectedly returned");
}
