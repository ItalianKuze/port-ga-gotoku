#ifndef BB_RUNTIME_H
#define BB_RUNTIME_H
#include <stdint.h>
#include <stddef.h>
#ifdef _WIN32
#include <windows.h>
#include "win_compat.h"
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
#else
#include <time.h>
#include <setjmp.h>
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
#endif
/* Broken-down UTC time back to time_t: POSIX timegm(), _mkgmtime() in the Windows CRT. The local
 * UTC offset is bb_timegm(localtime(t)) - t (DST included), which replaces tm_gmtoff, a glibc/BSD
 * field the Windows CRT does not have. */
#include <time.h>
#ifndef _WIN32
/* Declared here so it does not depend on each file's feature-test macros (glibc hides it otherwise). */
time_t timegm(struct tm *tm);
#endif
static inline time_t bb_timegm(struct tm *t) {
#ifdef _WIN32
    return _mkgmtime(t);
#else
    return timegm(t);
#endif
}
/* Recovery point for speculative guest memory reads, shared with libbbgpu (one declaration). */
#include "../gpu/shim/bbport_recover.h"
#ifdef _WIN32
/* The VEH (probe.c) takes this thread's armed recovery point, or NULL; and counts recoveries. */
BbRecoverBuf *runtime_recover_take(void);
#endif
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
/* Ends the process for a normal quit (the window closed): waits up to 5 s for save data writes
 * (open /savedataN files, slots mounted for writing) to finish, prints the report, then exits
 * with code without running DLL detach (TerminateProcess on Windows). */
__attribute__((noreturn)) void runtime_quit(int code, const char *why);
/* Open guest files under /savedataN: their number; the first path goes to first. */
int runtime_file_open_saves(char *first, size_t size);
/* Save slots mounted for writing: their number; the first dirName goes to first. */
int runtime_savedata_writable_mounts(char *first, size_t size);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
#ifndef _WIN32
static inline void runtime_memory_reserve(void) {}
#else
/* Reserves the fixed PS4 user range before anything maps inside it (probe.c). */
void runtime_memory_reserve(void);
#endif
/* The CPU is about to write the range outside guest code (a file read): tells the GPU side. */
void runtime_memory_note_write(uintptr_t address, uint64_t size);
void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size);
/* bbport (frame stats): a guest thread was blocked `ns` in the runtime (0 cond, 1 mutex, 2 sema, 3 sleep). */
void runtime_wait_note(int kind, uint64_t ns);
void runtime_wait_report(double frames);
void runtime_guest_call_sites(uint64_t out[3]);
/* bbport: times the game's heap asked for more memory (posix_mmap): a sign it leaks. */
uint64_t runtime_heap_growths(void);
#ifndef _WIN32
static inline uint64_t runtime_wait_clock(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec; }
#else
/* QueryPerformanceCounter: the Windows monotonic clock behind runtime_process_time_us. */
static inline uint64_t runtime_wait_clock(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart;
}
#endif
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
#ifndef _WIN32
static inline uint32_t runtime_thread_tcb_offset(void) { return 0; }
#else
/* TEB TLS slot holding the guest TCB, as a gs-relative displacement. The loader needs it to
 * rewrite the guest's `mov rax, gs:[0]`, so the slot is allocated before the image is patched. */
uint32_t runtime_thread_tcb_offset(void);
#endif
void runtime_thread_attach_main(void);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
void runtime_sema_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
/* Exit code when Windows cannot commit the guest memory pool (launcher/pgg_game.py EXIT_NO_MEMORY). */
#define RUNTIME_EXIT_NO_MEMORY 24
/* What this runtime expects of a prepared boot image (bb-probe --image-format; the launcher keeps it in
 * prepared.json and prepares again when it changes). Raise it when the image an unchanged set of
 * preparation scripts produces no longer fits the runtime. 1: imports named NID#X#Y (letters of the
 * eboot), up to 0.1.2; 2: imports named NID#library. */
#define RUNTIME_IMAGE_FORMAT 2
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
/* 0x8002002d (ENOTSUP) for the guest; logs the import name the first time it happens. */
int32_t runtime_note_enotsup(const char *import);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
void runtime_kernel_configure(uint32_t app_attributes, int valid);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
uintptr_t runtime_zlib_resolve(const char *name);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
void runtime_savedata_configure(const char *title);
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
/* PGG_SAVE_TRACE=1: every sceSaveData* call and every file operation on /savedataN is logged. */
int runtime_save_trace(void);
/* For the save data dialog: 1 when save slot <dir> of <user>/<title> (NULL: the game's) exists with
 * its param.bin; copies the OrbisSaveDataParam (1328 bytes) to param and its last write to mtime. */
int runtime_savedata_slot(int32_t user, const char *title, const char *dir, void *param, int64_t *mtime);
void runtime_thread_attach_host(const char *name);
#endif
