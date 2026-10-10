#ifndef BB_WIN_COMPAT_H
#define BB_WIN_COMPAT_H
#include <windows.h>
#include <time.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <errno.h>
#include <ctype.h>
#include <bcrypt.h>

/* ---- open() flags the guest's file layer passes but the CRT does not have ---- */
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef O_SYNC
#define O_SYNC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#define DT_REG 8
#define DT_DIR 4
#define DT_LNK 10
#endif

/* POSIX mkdir takes a mode; the CRT's does not. */
static inline int bb_mkdir(const char *path, int mode) { (void)mode; return _mkdir(path); }
#define mkdir(path, mode) bb_mkdir(path, mode)

/* MinGW's struct stat has st_atime/st_mtime/st_ctime as plain __time64_t and no block fields, so
 * runtime_file.c's convert_stat() fills the guest's block/time fields per platform instead. */

static inline char *bb_realpath(const char *path, char *out) { return _fullpath(out, path, 4096); }
#define realpath bb_realpath
/* The guest TCB blocks are allocated once per thread and live for the thread's lifetime, so they
   are never handed back to free() and _aligned_malloc is a safe stand-in. */
static inline void *bb_aligned_alloc(size_t align, size_t size) { return _aligned_malloc(size ? size : 1, align); }
#define aligned_alloc(align, size) bb_aligned_alloc(align, size)
static inline int bb_ftruncate(int fd, int64_t size) { return _chsize_s(_get_osfhandle(fd), size); }
#define ftruncate bb_ftruncate
static inline int bb_truncate(const char *path, int64_t size) {
    int fd = _open(path, _O_WRONLY | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) return -1;
    int r = _chsize_s(_get_osfhandle(fd), size);
    _close(fd);
    return r;
}
#define truncate bb_truncate

/* gettid()/sysconf()/setenv() come from gpu/shim/unistd.h, which src/ also has on its include
   path; defining them here as well would collide. */

#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#endif

static inline char *strcasestr(const char *haystack, const char *needle) {
    if (!needle[0]) return (char *)haystack;
    for (; *haystack; ++haystack) {
        const char *h = haystack, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { ++h; ++n; }
        if (!*n) return (char *)haystack;
    }
    return NULL;
}

static inline struct tm *localtime_r(const time_t *timep, struct tm *result) {
    return localtime_s(result, timep) ? NULL : result;
}

static inline ssize_t getrandom(void *buf, size_t buflen, unsigned flags) {
    (void)flags;
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)buflen, BCRYPT_USE_SYSTEM_PREFERRED_RNG) ? (ssize_t)buflen : -1;
}

#ifndef RUSAGE_SELF
#define RUSAGE_SELF 0
#define RUSAGE_THREAD 1
#endif
#ifndef BB_RUSAGE_DEFINED
#define BB_RUSAGE_DEFINED
struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
};
#endif
static inline int getrusage(int who, struct rusage *r) {
    (void)who;
    if (!r) return -1;
    memset(r, 0, sizeof(*r));
    return 0;
}

/* Positioned I/O on CRT descriptors: leaves the file offset unchanged. */
static inline ssize_t bb_pread(int fd, void *buffer, size_t size, int64_t offset) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (offset < 0) { errno = EINVAL; return -1; }
    LARGE_INTEGER zero = {0}, saved;
    if (!SetFilePointerEx(h, zero, &saved, FILE_CURRENT)) return -1;
    size_t done = 0;
    while (done < size) {
        DWORD chunk = size - done > 0x40000000u ? 0x40000000u : (DWORD)(size - done), n = 0;
        OVERLAPPED at = {0};
        uint64_t position = (uint64_t)offset + done;
        at.Offset = (DWORD)position; at.OffsetHigh = (DWORD)(position >> 32);
        if (!ReadFile(h, (char *)buffer + done, chunk, &n, &at)) {
            DWORD e = GetLastError();
            if (e == ERROR_HANDLE_EOF) break;
            SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
            if (done) return (ssize_t)done;
            return -1;
        }
        done += n;
        if (n < chunk) break;
    }
    SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
    return (ssize_t)done;
}
static inline ssize_t bb_pwrite(int fd, const void *buffer, size_t size, int64_t offset) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (offset < 0) { errno = EINVAL; return -1; }
    LARGE_INTEGER zero = {0}, saved;
    if (!SetFilePointerEx(h, zero, &saved, FILE_CURRENT)) return -1;
    size_t done = 0;
    while (done < size) {
        DWORD chunk = size - done > 0x40000000u ? 0x40000000u : (DWORD)(size - done), n = 0;
        OVERLAPPED at = {0};
        uint64_t position = (uint64_t)offset + done;
        at.Offset = (DWORD)position; at.OffsetHigh = (DWORD)(position >> 32);
        if (!WriteFile(h, (const char *)buffer + done, chunk, &n, &at)) {
            SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
            if (done) return (ssize_t)done;
            return -1;
        }
        done += n;
        if (n < chunk) break;
    }
    SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
    return (ssize_t)done;
}
#define pread bb_pread
#define pwrite bb_pwrite

static inline int bb_fsync(int fd) { return _commit(fd); }
#define fsync bb_fsync

static inline long bb_utc_offset(time_t t) {
    struct tm local, utc;
    if (localtime_s(&local, &t) || gmtime_s(&utc, &t)) return 0;
    utc.tm_isdst = local.tm_isdst;
    time_t as_local = mktime(&utc);
    return as_local == (time_t)-1 ? 0 : (long)(t - as_local);
}

static inline void bb_sleep_ns(uint64_t ns) {
    if (!ns) { SwitchToThread(); return; }
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)((ns + 99) / 100);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(timer, INFINITE);
    else
        Sleep((DWORD)((ns + 999999) / 1000000));
    if (timer) CloseHandle(timer);
}

#endif
