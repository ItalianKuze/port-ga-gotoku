/* Narrow, explicit PS4 libc contracts. No automatic success stubs. */
#define _CRT_RAND_S
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

static uint64_t capabilities;
static uint64_t stack_canary;
/* Dynamic TLS of linked modules: module IDs 2..7 (1 is the eboot's static TLS). */
#define TLS_MODULES 8
static struct { const void *data; uint64_t filesz, memsz; } tls_modules[TLS_MODULES];
static _Thread_local unsigned char *tls_blocks[TLS_MODULES];
static void *process_param;
void runtime_set_procparam(void *param) {
    process_param=param;
    /* bbport: the game's libc heap settings (SceProcParam +0x30: SceLibcParam; its +0x10.. pointers to
     * the heap size, delayed and extended allocation flags, initial size). */
    const uint64_t *p=(const uint64_t *)param;
    if (!p || p[0]<0x38 || !p[6]) return;
    const uint64_t *libc=(const uint64_t *)p[6];
    printf("Runtime: libc param size %#llx, entries %u", (unsigned long long)libc[0], (unsigned)libc[1]);
    static const char *names[]={"heap size","delayed alloc","extended alloc","initial size"};
    for (int i=0;i<4 && (uint64_t)(2+i)*8<libc[0];++i) {
        const void *field=(const void *)libc[2+i];
        if (!field) { printf(", %s -",names[i]); continue; }
        if (i==0 || i==3) printf(", %s %#llx",names[i],(unsigned long long)*(const uint64_t *)field);
        else printf(", %s %u",names[i],*(const uint32_t *)field);
    }
    printf("\n");
}
static ABI void *guest_procparam(void) { return process_param; }
static void **application_heap_api;
static ABI void guest_set_heap_api(void **api) {
    application_heap_api=api;
    puts("Runtime: application heap API registered");
}
void **runtime_application_heap_api(void) { return application_heap_api; }
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz) {
    if (module<2 || module>=TLS_MODULES) { fputs("STOP: unsupported TLS module id\n",stderr); exit(21); }
    tls_modules[module].data=data; tls_modules[module].filesz=filesz; tls_modules[module].memsz=memsz;
}
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz) { runtime_set_module_tls(2,data,filesz,memsz); }
static ABI void *guest_tls_get_addr(const uint64_t *index) {
    if (!index || index[0]<2 || index[0]>=TLS_MODULES || !tls_modules[index[0]].data || index[1]>=tls_modules[index[0]].memsz) {
        fputs("STOP: unsupported TLS module/offset\n",stderr); exit(21);
    }
    unsigned char **block=&tls_blocks[index[0]];
    if (!*block) {
        *block=calloc(1,tls_modules[index[0]].memsz);
        if (!*block) { fputs("Cannot allocate module TLS\n",stderr); exit(1); }
        memcpy(*block,tls_modules[index[0]].data,tls_modules[index[0]].filesz);
    }
    return *block+index[1];
}

void runtime_start(uint64_t flags) {
    capabilities = flags;
    if (!(flags & 1)) return;
#ifdef _WIN32
    unsigned int halves[2];
    if (rand_s(&halves[0]) || rand_s(&halves[1])) { fputs("Cannot initialize stack canary\n", stderr); exit(1); }
    memcpy(&stack_canary, halves, sizeof(stack_canary));
#else
    FILE *random = fopen("/dev/urandom", "rb");
    if (!random || fread(&stack_canary, sizeof(stack_canary), 1, random) != 1) {
        fputs("Cannot initialize stack canary\n", stderr); exit(1);
    }
    fclose(random);
#endif
    stack_canary &= ~UINT64_C(255);
}
void runtime_quit(int code, const char *why) {
    printf("Runtime: quitting (%s)\n", why);
    /* A save in progress (the game mounts a slot for writing, writes its files, unmounts it) is
     * allowed to finish: at most 5 s, polled every 50 ms. */
    for (int waited=0;;waited+=50) {
        char file[512], slot[40];
        const int files=runtime_file_open_saves(file,sizeof(file)), slots=runtime_savedata_writable_mounts(slot,sizeof(slot));
        if (!files && !slots) {
            if (waited) printf("Runtime: save data writes finished after %d ms\n", waited);
            break;
        }
        if (!waited)
            printf("Runtime: waiting for save data writes before quitting: %d open file(s)%s%s, %d slot(s) mounted for writing%s%s\n",
                   files, files ? ", first " : "", files ? file : "", slots, slots ? ", first " : "", slots ? slot : "");
        if (waited>=5000) {
            printf("Runtime: WARNING: quitting with save data writes still in progress (%d open file(s), %d slot(s) mounted for writing); the save may be incomplete\n",
                   files, slots);
            break;
        }
#ifdef _WIN32
        Sleep(50);
#else
        struct timespec nap={0,50000000}; nanosleep(&nap,NULL);
#endif
    }
    runtime_report();
    fflush(NULL);
#ifdef _WIN32
    /* Not ExitProcess: its DLL detach runs while GPU, audio and guest threads still hold driver
     * and loader state, and the game hung on closing its window (2026-10-11). */
    TerminateProcess(GetCurrentProcess(), (UINT)code);
#endif
    _exit(code);
}
void runtime_report(void) {
    runtime_thread_report();
    runtime_mutex_report();
    runtime_rwlock_report();
    runtime_sema_report();
    runtime_content_report();
    runtime_memory_report();
    runtime_ajm_report();
    runtime_audio_report();
    runtime_pad_report();
    runtime_savedata_report();
    runtime_file_report();
}
static ABI __attribute__((noreturn)) void stack_fail(void) {
    fputs("STOP: guest stack protector detected corruption\n", stderr);
    exit(22);
}
uintptr_t runtime_resolve(const char *name, int is_data) {
    if (!(capabilities & 1)) return 0;
    if (is_data) {
        static int32_t need_libc_internal = 1; /* SDK marker variable referenced by Fios2 */
        if (!strcmp(name, "f7uOxY9mM1U#libkernel"))
            return (uintptr_t)&stack_canary;
        if (!strcmp(name, "ZT4ODD2Ts9o#libSceLibcInternal")) return (uintptr_t)&need_libc_internal;
        return 0;
    }
    if (!strcmp(name, "Ou3iL1abvng#libkernel")) return (uintptr_t)stack_fail;
    if (!strcmp(name, "vNe1w4diLCs#libkernel")) return (uintptr_t)guest_tls_get_addr;
    if (!strcmp(name, "959qrazPIrg#libkernel"))
        return (uintptr_t)guest_procparam;
    if (!strcmp(name, "p5EcQeEeJAE#libkernel"))
        return (uintptr_t)guest_set_heap_api;
    uintptr_t mutex = runtime_mutex_resolve(name);
    if (mutex) return mutex;
    uintptr_t thread = runtime_thread_resolve(name);
    if (thread) return thread;
    uintptr_t content=runtime_content_resolve(name);
    if (content) return content;
    uintptr_t timer = runtime_time_resolve(name);
    if (timer) return timer;
    uintptr_t sema = runtime_sema_resolve(name);
    if (sema) return sema;
    uintptr_t rwlock = runtime_rwlock_resolve(name);
    if (rwlock) return rwlock;
    uintptr_t memory = runtime_memory_resolve(name);
    if (memory) return memory;
    uintptr_t kernel = runtime_kernel_resolve(name);
    if (kernel) return kernel;
    uintptr_t services = runtime_services_resolve(name);
    if (services) return services;
    uintptr_t zlib = runtime_zlib_resolve(name);
    if (zlib) return zlib;
    uintptr_t ajm = runtime_ajm_resolve(name);
    if (ajm) return ajm;
    uintptr_t audio = runtime_audio_resolve(name);
    if (audio) return audio;
    uintptr_t pad = runtime_pad_resolve(name);
    if (pad) return pad;
    uintptr_t rtc = runtime_rtc_resolve(name);
    if (rtc) return rtc;
    uintptr_t save = runtime_savedata_resolve(name);
    if (save) return save;
    uintptr_t file = runtime_file_resolve(name);
    if (file) return file;
    return bbgpu_resolve(name);
}
static const struct { const char *nid, *symbol; } import_names[]={
#include "import_names.inc"
};
/* Symbol name for a scoped NID, or NULL when this eboot/libc never imports it. */
/* An option the guest asked for that this host cannot provide is the kind of difference that made
 * Hokuto skip its CRI initialization on Windows (scePthreadMutexInit, PRIO_INHERIT): say so once
 * per import, so the next case shows up in the log at once. */
int32_t runtime_note_enotsup(const char *import) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static const char *seen[64];
    static size_t count;
    int first = 1;
    pthread_mutex_lock(&lock);
    for (size_t i = 0; i < count; ++i) if (!strcmp(seen[i], import)) first = 0;
    if (first && count < 64) seen[count++] = import;
    pthread_mutex_unlock(&lock);
    if (first) fprintf(stderr, "Runtime: %s returned ENOTSUP (0x8002002d) to the guest (logged once)\n", import);
    return (int32_t)UINT32_C(0x8002002d);
}
const char *runtime_symbol(const char *nid) {
    for (size_t i=0;i<sizeof(import_names)/sizeof(*import_names);++i)
        if (!strcmp(nid,import_names[i].nid)) return import_names[i].symbol;
    return NULL;
}
/* Same symbol name means same contract whether imported from libkernel or
 * libScePosix, so newer modules resolve by name through runtime_symbol. */
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *nid) {
    const char *symbol=runtime_symbol(nid);
    if (!symbol) return 0;
    for (size_t i=0;i<count;++i) if (!strcmp(symbol,table[i].name)) return (uintptr_t)table[i].function;
    return 0;
}
const char *runtime_import_name(const char *name) {
    const char *symbol=runtime_symbol(name);
    return symbol ? symbol : "name not resolved; see analysis.json import_name_hints";
}
