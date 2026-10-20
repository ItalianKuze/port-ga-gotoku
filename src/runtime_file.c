/* Guest file system: PS4 mount points mapped onto host directories.
 *   /app0, /hostapp  -> game package root (read-only by convention)
 *   /temp0, /download0, /data, and mounts added by SaveData -> user directory
 * Guest descriptors are small integers in our own table; stdio 0-2 pass through.
 * Paths containing ".." components are rejected rather than normalized. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define MAX_FILES 1024
#define MAX_MOUNTS 16

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct {
    uint32_t dev, ino;
    uint16_t mode, nlink;
    uint32_t uid, gid, rdev;
    GuestTimespec atime, mtime, ctime;
    int64_t size, blocks;
    uint32_t blksize, flags, gen;
    int32_t lspare;
    GuestTimespec birthtime;
} GuestStat;
_Static_assert(sizeof(GuestStat)==120,"FreeBSD stat layout");

typedef struct { char *names; size_t count, *offsets; unsigned char *types; } Listing;
/* commit: a save file (/savedataN) opened for writing is written to a temporary copy (temp) in
 * the same folder, which replaces the file (commit) in one step when closed: a crash or a kill
 * mid-save leaves the old file. dirty: written since it was opened. */
typedef struct { int used, host, dirty; Listing *dir; size_t position; char path[512]; char *commit, *temp; } File;
typedef struct { char guest[64]; char host[512]; } Mount;
static File files[MAX_FILES];
static Mount mounts[MAX_MOUNTS];
static size_t mount_count, opens, reads, writes, missing;
static uint64_t bytes_read;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;

static int temp_name(const char *name) {
    size_t n=strlen(name);
    return n>6 && !strcmp(name+n-6,".bbtmp");
}
/* Copies left by a crash (none of them open): removed when their save directory is mounted. */
static void remove_stale_temps(const char *dir,int depth) {
    DIR *d=opendir(dir);
    if (!d) return;
    for (struct dirent *e; (e=readdir(d));) {
        if (e->d_name[0]=='.') continue;
        char path[1024]; struct stat st;
        if ((size_t)snprintf(path,sizeof(path),"%s/%s",dir,e->d_name)>=sizeof(path)) continue;
        if (temp_name(e->d_name)) {
            int open_now=0;
            for (int i=3;i<MAX_FILES;++i) if (files[i].used && files[i].temp && !strcmp(files[i].temp,path)) open_now=1;
            if (!open_now && !unlink(path)) printf("Runtime: removed %s (a save interrupted earlier; the file it would have replaced is intact)\n",path);
        } else if (depth<4 && !stat(path,&st) && S_ISDIR(st.st_mode)) remove_stale_temps(path,depth+1);
    }
    closedir(d);
}

int runtime_file_mount(const char *guest,const char *host) {
    pthread_mutex_lock(&lock);
    for (size_t i=0;i<mount_count;++i) if (!strcmp(mounts[i].guest,guest)) {
        snprintf(mounts[i].host,sizeof(mounts[i].host),"%s",host);
        pthread_mutex_unlock(&lock); return 0;
    }
    if (mount_count==MAX_MOUNTS || strlen(guest)>=64 || strlen(host)>=512) { pthread_mutex_unlock(&lock); return -1; }
    snprintf(mounts[mount_count].guest,64,"%s",guest);
    snprintf(mounts[mount_count].host,512,"%s",host);
    ++mount_count;
    if (!strncmp(guest,"/savedata",9)) remove_stale_temps(host,0);
    pthread_mutex_unlock(&lock);
    return 0;
}
void runtime_file_unmount(const char *guest) {
    pthread_mutex_lock(&lock);
    for (size_t i=0;i<mount_count;++i) if (!strcmp(mounts[i].guest,guest)) {
        mounts[i]=mounts[--mount_count]; break;
    }
    pthread_mutex_unlock(&lock);
}
static char user_root[512]="user";
const char *runtime_file_user_dir(void) { return user_root; }
void runtime_file_configure(const char *app0,const char *user) {
    char path[600];
    snprintf(user_root,sizeof(user_root),"%s",user);
    runtime_file_mount("/app0",app0);
    runtime_file_mount("/hostapp",app0);
    const char *writable[]={"temp0","download0","data"};
    mkdir(user,0755);
    for (int i=0;i<3;++i) {
        snprintf(path,sizeof(path),"%s/%s",user,writable[i]);
        mkdir(path,0755);
        char guest[32]; snprintf(guest,sizeof(guest),"/%s",writable[i]);
        runtime_file_mount(guest,path);
    }
}
/* Resolve a guest path to a host path; returns 0 or a host errno. */
static int translate(const char *guest,char *out,size_t size) {
    if (!guest || !*guest) return ENOENT;
    char buffer[1024];
    if (guest[0]!='/') snprintf(buffer,sizeof(buffer),"/app0/%s",guest);
    else snprintf(buffer,sizeof(buffer),"%s",guest);
    for (const char *p=buffer;(p=strstr(p,".."));p+=2)
        if ((p==buffer || p[-1]=='/') && (p[2]==0 || p[2]=='/')) return EACCES;
    pthread_mutex_lock(&lock);
    size_t best=0; const Mount *m=NULL;
    for (size_t i=0;i<mount_count;++i) {
        size_t n=strlen(mounts[i].guest);
        if (!strncmp(buffer,mounts[i].guest,n) && (buffer[n]=='/' || !buffer[n]) && n>best) { best=n; m=&mounts[i]; }
    }
    int result=0;
    if (!m) result=ENOENT;
    else if ((size_t)snprintf(out,size,"%s%s",m->host,buffer+best)>=size) result=ENAMETOOLONG;
    pthread_mutex_unlock(&lock);
    if (result==ENOENT) fprintf(stderr,"Runtime: no mount for guest path %s\n",guest);
    return result;
}
static int host_flags(int flags) {
    int r;
    switch (flags&3) { case 0: r=O_RDONLY; break; case 1: r=O_WRONLY; break; default: r=O_RDWR; }
    if (flags&0x4) r|=O_NONBLOCK;
    if (flags&0x8) r|=O_APPEND;
    if (flags&0x80) r|=O_SYNC;
    if (flags&0x200) r|=O_CREAT;
    if (flags&0x400) r|=O_TRUNC;
    if (flags&0x800) r|=O_EXCL;
    if (flags&0x20000) r|=O_DIRECTORY;
#ifdef O_BINARY
    /* Windows CRT: without O_BINARY a file opens in text mode, where reads turn CRLF into LF and
     * stop at the first 0x1A byte -- Hokuto's data/shader/sh_devil_ps4.par has one at 82317. */
    r|=O_BINARY;
#endif
    return r|O_CLOEXEC;
}
static void free_listing(Listing *l) { if (l) { free(l->names); free(l->offsets); free(l->types); free(l); } }
static Listing *list_directory(const char *path) {
    DIR *d=opendir(path);
    if (!d) return NULL;
    Listing *l=calloc(1,sizeof(*l));
    size_t capacity=0,bytes=0,cap_names=0;
    struct dirent *e;
    while (l && (e=readdir(d))) {
        if (temp_name(e->d_name)) continue; /* a save being written: see File */
        size_t n=strlen(e->d_name)+1;
        if (l->count==capacity) {
            capacity=capacity ? capacity*2 : 64;
            l->offsets=realloc(l->offsets,capacity*sizeof(size_t));
            l->types=realloc(l->types,capacity);
        }
        if (bytes+n>cap_names) { cap_names=(bytes+n)*2; l->names=realloc(l->names,cap_names); }
        if (!l->offsets || !l->types || !l->names) { fputs("Out of memory listing directory\n",stderr); exit(1); }
        memcpy(l->names+bytes,e->d_name,n);
        l->offsets[l->count]=bytes;
        /* MinGW's dirent has no d_type, so ask the filesystem what the entry is. */
        unsigned char type=DT_UNKNOWN;
        {
            char full[4096];
            struct stat entry;
            snprintf(full,sizeof(full),"%s/%s",path,e->d_name);
            if (!stat(full,&entry))
                type = S_ISDIR(entry.st_mode) ? DT_DIR : S_ISREG(entry.st_mode) ? DT_REG : DT_UNKNOWN;
        }
        l->types[l->count]=type==DT_DIR ? 4 : type==DT_REG ? 8 : type==DT_LNK ? 10 : 0;
        ++l->count; bytes+=n;
    }
    closedir(d);
    return l;
}
static void convert_stat(const struct stat *s,GuestStat *g) {
    memset(g,0,sizeof(*g));
    g->dev=(uint32_t)s->st_dev; g->ino=(uint32_t)s->st_ino;
    g->mode=(uint16_t)s->st_mode; g->nlink=(uint16_t)s->st_nlink;
    g->size=s->st_size;
#ifdef _WIN32
    /* MinGW's struct stat has no block fields and its timestamps are plain __time64_t. */
    g->blocks=0; g->blksize=4096;
    g->atime=(GuestTimespec){(int64_t)s->st_atime,0};
    g->mtime=(GuestTimespec){(int64_t)s->st_mtime,0};
    g->ctime=(GuestTimespec){(int64_t)s->st_ctime,0};
#else
    g->blocks=s->st_blocks; g->blksize=(uint32_t)s->st_blksize;
    g->atime=(GuestTimespec){s->st_atim.tv_sec,s->st_atim.tv_nsec};
    g->mtime=(GuestTimespec){s->st_mtim.tv_sec,s->st_mtim.tv_nsec};
    g->ctime=(GuestTimespec){s->st_ctim.tv_sec,s->st_ctim.tv_nsec};
#endif
    g->birthtime=g->ctime;
}
static File *get(int fd) {
    if (fd<3 || fd>=MAX_FILES || !files[fd].used) return NULL;
    return &files[fd];
}
/* BB_AUDIO_TRACE=1: sound file opens and failed reads (missing game sounds). */
static int audio_trace(void) { static int v=-1; if (v<0) { const char *e=getenv("BB_AUDIO_TRACE"); v=e && e[0]=='1'; } return v; }
/* BB_FILE_TRACE=1: every open/stat/access with its guest path, host path and result
 * (fd or -errno), to compare file access between builds. */
static int file_trace(void) { static int v=-1; if (v<0) { const char *e=getenv("BB_FILE_TRACE"); v=e && e[0]=='1'; } return v; }
static int save_path(const char *guest) { return guest && !strncmp(guest,"/savedata",9); }
static int64_t traced(const char *op,const char *guest,const char *host,int64_t result) {
    if (file_trace() || (runtime_save_trace() && save_path(guest))) printf("FileTrace: %s %s -> %s = %lld\n",op,guest ? guest : "(null)",host ? host : "-",(long long)result);
    return result;
}
/* Game mounts (including linked mod overlays) are read-only. Saves use other mounts. */
static int game_path(const char *p) {
    if (!p || !*p) return 0;
    if (*p!='/') return 1;
    return (!strncmp(p,"/app0",5) && (!p[5] || p[5]=='/')) ||
           (!strncmp(p,"/hostapp",8) && (!p[8] || p[8]=='/'));
}
/* Saves (/savedataN): see File. The copy starts as the old file unless the open truncates. */
#ifdef O_BINARY
#define HOST_BINARY O_BINARY
#else
#define HOST_BINARY 0
#endif
static unsigned temp_serial;
static int copy_contents(int from,int to) {
    char buffer[65536];
    for (;;) {
        ssize_t n=read(from,buffer,sizeof(buffer));
        if (n<0 && errno==EINTR) continue;
        if (n<=0) return n<0 ? -1 : 0;
        for (ssize_t done=0; done<n;) {
            ssize_t w=write(to,buffer+done,(size_t)(n-done));
            if (w<0 && errno==EINTR) continue;
            if (w<0) return -1;
            done+=w;
        }
    }
}
/* A save file open for writing is its copy to the operations by path too (stat, access, open for
 * reading, truncate): the game sees what it has written so far. */
static void current_copy(char *path,size_t size,int write) {
    pthread_mutex_lock(&lock);
    for (int i=3;i<MAX_FILES;++i)
        if (files[i].used && files[i].commit && !strcmp(files[i].commit,path)) { snprintf(path,size,"%s",files[i].temp); files[i].dirty|=write; break; }
    pthread_mutex_unlock(&lock);
}
/* Returns the descriptor of the copy, or -(errno). */
static int open_for_commit(const char *path,const char *source,int flags,int mode,char **commit,char **temp) {
    int hf=host_flags(flags);
    struct stat s;
    int exists=!stat(source,&s);
    if (exists && !S_ISREG(s.st_mode)) { int h=open(path,hf,mode ? mode : 0644); return h<0 ? -errno : h; }
    if (exists && (hf&O_CREAT) && (hf&O_EXCL)) return -EEXIST;
    if (!exists && !(hf&O_CREAT)) return -ENOENT;
    size_t n=strlen(path)+32;
    char *t=malloc(n), *c=strdup(path);
    if (!t || !c) { free(t); free(c); return -ENOMEM; }
    snprintf(t,n,"%s.%u.bbtmp",path,__atomic_add_fetch(&temp_serial,1,__ATOMIC_RELAXED));
    int out=open(t,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|HOST_BINARY,exists ? (int)(s.st_mode&07777) : mode ? mode : 0644);
    int e=out<0 ? errno : 0;
    if (!e && exists && !(hf&O_TRUNC)) {
        int in=open(source,O_RDONLY|O_CLOEXEC|HOST_BINARY);
        if (in<0 || copy_contents(in,out)) e=errno ? errno : EIO;
        if (in>=0) close(in);
    }
    if (out>=0) close(out);
    int host=e ? -1 : open(t,hf&~(O_CREAT|O_EXCL|O_TRUNC));
    if (host<0) {
        if (!e) e=errno;
        unlink(t); free(t); free(c);
        return -e;
    }
    *commit=c; *temp=t;
    return host;
}
/* Moves the written copy over the save file; on failure the old file is left as it was. */
static int replace_file(const char *temp,const char *target) {
#ifdef _WIN32
    /* rename() does not replace an existing file on Windows. Host paths are in the ANSI code page,
     * as open() takes them. */
    wchar_t wide_temp[1024], wide_target[1024];
    if (!MultiByteToWideChar(CP_ACP,0,temp,-1,wide_temp,1024) ||
        !MultiByteToWideChar(CP_ACP,0,target,-1,wide_target,1024)) return ENAMETOOLONG;
    if (MoveFileExW(wide_temp,wide_target,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return 0;
    const DWORD w=GetLastError();
    return w==ERROR_ACCESS_DENIED ? EACCES : w==ERROR_SHARING_VIOLATION ? EBUSY
         : w==ERROR_FILE_NOT_FOUND || w==ERROR_PATH_NOT_FOUND ? ENOENT : EIO;
#else
    if (rename(temp,target)) return errno;
    char dir[1024];
    snprintf(dir,sizeof(dir),"%s",target);
    char *slash=strrchr(dir,'/');
    if (slash) {
        *slash=0;
        int d=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if (d>=0) { fsync(d); close(d); }
    }
    return 0;
#endif
}
static int commit_file(const File *f) {
    if (!f->dirty) { close(f->host); unlink(f->temp); return 0; } /* opened for writing, not written */
    int e=fsync(f->host) ? errno : 0;
    if (close(f->host) && !e) e=errno;
    if (!e) e=replace_file(f->temp,f->commit);
    if (e) {
        fprintf(stderr,"Runtime: save file %s not replaced (%s); the old one is kept\n",f->path,strerror(e));
        unlink(f->temp);
        return -e;
    }
    return 0;
}
/* All operations return >=0 or -(host errno); wrappers adapt the convention. */
static int64_t do_open(const char *guest,int flags,int mode) {
    if (game_path(guest) && (flags & (3|0x8|0x200|0x400|0x800))) return traced("open",guest,NULL,-EROFS);
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return traced("open",guest,NULL,-e);
    char *commit=NULL, *temp=NULL, current[1024];
    snprintf(current,sizeof(current),"%s",path);
    if (save_path(guest)) current_copy(current,sizeof(current),0);
    int host;
    if (save_path(guest) && (flags&3)) {
        host=open_for_commit(path,current,flags,mode,&commit,&temp);
        if (host<0) { errno=-host; host=-1; }
    } else host=open(current,host_flags(flags),mode ? mode : 0644);
    if (host<0) {
        e=errno;
        if (e==ENOENT) { ++missing; printf("Runtime: open(%s) -> not found\n",guest); }
        return traced("open",guest,path,-e);
    }
    traced("open",guest,temp ? temp : current,host);
    struct stat s;
    Listing *dir=NULL;
    if (!fstat(host,&s) && S_ISDIR(s.st_mode)) dir=list_directory(path);
    pthread_mutex_lock(&lock);
    int fd=-1;
    for (int i=3;i<MAX_FILES;++i) if (!files[i].used) { fd=i; break; }
    if (fd<0) {
        pthread_mutex_unlock(&lock); close(host); free_listing(dir);
        if (temp) unlink(temp);
        free(commit); free(temp); return -EMFILE;
    }
    files[fd]=(File){.used=1,.host=host,.dir=dir,.commit=commit,.temp=temp};
    snprintf(files[fd].path,sizeof(files[fd].path),"%s",guest);
    ++opens;
    pthread_mutex_unlock(&lock);
    if (audio_trace() && strstr(guest,"sound/")) printf("Audio trace: open(%s) -> fd %d, %lld bytes\n",guest,fd,(long long)s.st_size);
    const char *mod_trace=getenv("BB_MOD_TRACE"), *mod_root=getenv("BB_MODS_DIR");
    if (mod_trace && mod_trace[0]=='1' && mod_root) {
        char actual[PATH_MAX],root[PATH_MAX];
        static unsigned traced;
        if (realpath(path,actual) && realpath(mod_root,root)) {
            size_t n=strlen(root);
            if (!strncmp(actual,root,n) && actual[n]=='/' &&
                __atomic_fetch_add(&traced,1,__ATOMIC_RELAXED)<32)
                printf("Mods: open %s -> %s\n",guest,actual);
        }
    }
    return fd;
}
static int64_t traced_fd(const char *op,int fd,uint64_t size,int64_t offset,int64_t result);
int runtime_file_open_saves(char *first, size_t size) {
    int count=0;
    if (first && size) first[0]=0;
    pthread_mutex_lock(&lock);
    for (int i=3;i<MAX_FILES;++i)
        if (files[i].used && !strncmp(files[i].path,"/savedata",9)) {
            if (!count++ && first && size) snprintf(first,size,"%s",files[i].path);
        }
    pthread_mutex_unlock(&lock);
    return count;
}
static int64_t do_close(int fd) {
    if (fd>=0 && fd<3) return 0;
    traced_fd("close",fd,0,0,0);
    pthread_mutex_lock(&lock);
    File *f=get(fd);
    if (!f) { pthread_mutex_unlock(&lock); return -EBADF; }
    File closed=*f;
    *f=(File){0};
    pthread_mutex_unlock(&lock);
    free_listing(closed.dir);
    int result=0;
    if (closed.temp) result=commit_file(&closed);
    else close(closed.host);
    if (closed.temp && runtime_save_trace())
        printf("FileTrace: close %s: %s = %d\n",closed.path,closed.dirty ? "replaced by its copy" : "not written, copy removed",result);
    free(closed.commit); free(closed.temp);
    return result;
}
static int host_fd(int fd) {
    if (fd>=0 && fd<3) return fd;
    File *f=get(fd);
    return f ? f->host : -1;
}
/* host_fd() for a write: marks a save file's copy as written (see File). */
static int host_fd_written(int fd) {
    if (fd>=0 && fd<3) return fd;
    File *f=get(fd);
    if (!f) return -1;
    f->dirty=1;
    return f->host;
}
/* BB_FILE_TRACE: an operation on an open descriptor, named by the guest path it was opened as. */
static int64_t traced_fd(const char *op,int fd,uint64_t size,int64_t offset,int64_t result) {
    if (!file_trace() && !runtime_save_trace()) return result;
    File *f=fd>=3 ? get(fd) : NULL;
    if (!file_trace() && !(f && save_path(f->path))) return result;
    printf("FileTrace: %s fd %d (%s) size %llu off %lld = %lld\n",op,fd,f ? f->path : "?",
           (unsigned long long)size,(long long)offset,(long long)result);
    return result;
}
/* The GPU side is told of the write first (runtime_memory_note_write: its tracking unprotects the
 * range). Pages protected again meanwhile would make the kernel's copy fail with EFAULT instead
 * of faulting to our handler: a user-mode write to each page first goes through the handler. */
static void touch_for_write(void *buffer,uint64_t size) {
    if (!size) return;
    uintptr_t p=(uintptr_t)buffer & ~(uintptr_t)4095, end=(uintptr_t)buffer+size;
    for (; p<end; p+=4096) {
        volatile unsigned char *b=(volatile unsigned char *)(p<(uintptr_t)buffer ? (uintptr_t)buffer : p);
        *b=*b;
    }
}
static int64_t do_read(int fd,void *buffer,uint64_t size) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    runtime_memory_note_write((uintptr_t)buffer,size);
    touch_for_write(buffer,size);
    ssize_t n=read(h,buffer,size);
    traced_fd("read",fd,size,-1,n<0 ? -errno : n);
    if (n<0) { if (audio_trace()) printf("Audio trace: read(fd %d, %llu) failed, errno %d\n",fd,(unsigned long long)size,errno); return -errno; }
    if (n>0) runtime_memory_note_write((uintptr_t)buffer,(uint64_t)n); /* and once the data is there */
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_pread(int fd,void *buffer,uint64_t size,int64_t offset) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    runtime_memory_note_write((uintptr_t)buffer,size);
    touch_for_write(buffer,size);
    ssize_t n=pread(h,buffer,size,offset);
    traced_fd("pread",fd,size,offset,n<0 ? -errno : n);
    if (n<0) { if (audio_trace()) printf("Audio trace: pread(fd %d, %llu @%lld) failed, errno %d\n",fd,(unsigned long long)size,(long long)offset,errno); return -errno; }
    if (n>0) runtime_memory_note_write((uintptr_t)buffer,(uint64_t)n); /* and once the data is there */
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_write(int fd,const void *buffer,uint64_t size) {
    int h=host_fd_written(fd);
    if (h<0) return -EBADF;
    ssize_t n=write(h,buffer,size);
    traced_fd("write",fd,size,-1,n<0 ? -errno : n);
    if (n<0) return -errno;
    __atomic_add_fetch(&writes,1,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_pwrite(int fd,const void *buffer,uint64_t size,int64_t offset) {
    int h=host_fd_written(fd);
    if (h<0) return -EBADF;
    ssize_t n=pwrite(h,buffer,size,offset);
    return traced_fd("pwrite",fd,size,offset,n<0 ? -errno : n);
}
static int64_t do_lseek(int fd,int64_t offset,int whence) {
    File *f=get(fd);
    if (!f) return -EBADF;
    if (whence<0 || whence>2) return -EINVAL;
    if (f->dir) {
        /* Directory offsets are entry indices for getdirentries. */
        if (whence==0 && offset>=0) { f->position=(size_t)offset; return offset; }
        return -EINVAL;
    }
    off_t r=lseek(f->host,offset,whence);
    return traced_fd(whence==0 ? "lseek_set" : whence==1 ? "lseek_cur" : "lseek_end",fd,0,offset,r<0 ? -errno : r);
}
static int64_t do_fstat(int fd,GuestStat *out) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    if (!out) return -EFAULT;
    struct stat s;
    if (fstat(h,&s)) return -errno;
    convert_stat(&s,out); return 0;
}
static int64_t do_stat(const char *guest,GuestStat *out) {
    char path[1024]; struct stat s;
    int e=translate(guest,path,sizeof(path));
    if (e) return traced("stat",guest,NULL,-e);
    if (save_path(guest)) current_copy(path,sizeof(path),0);
    if (!out) return -EFAULT;
    if (stat(path,&s)) return traced("stat",guest,path,-errno);
    convert_stat(&s,out); return traced("stat",guest,path,0);
}
static int64_t do_getdents(int fd,char *buffer,uint64_t size,int64_t *basep) {
    pthread_mutex_lock(&lock);
    File *f=get(fd);
    int64_t result=0;
    if (!f) result=-EBADF;
    else if (!f->dir) result=-EINVAL;
    else if (!buffer) result=-EFAULT;
    else if (size<512) result=-EINVAL;
    else {
        if (basep) *basep=(int64_t)f->position;
        uint64_t written=0;
        while (f->position<f->dir->count) {
            const char *name=f->dir->names+f->dir->offsets[f->position];
            size_t n=strlen(name); if (n>255) n=255;
            uint16_t reclen=(uint16_t)((8+n+1+7)&~(size_t)7);
            if (written+reclen>size) break;
            char *p=buffer+written;
            memset(p,0,reclen);
            uint32_t ino=(uint32_t)f->position+1;
            memcpy(p,&ino,4); memcpy(p+4,&reclen,2);
            p[6]=(char)f->dir->types[f->position]; p[7]=(char)n;
            memcpy(p+8,name,n);
            written+=reclen; ++f->position;
        }
        result=(int64_t)written;
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static int64_t path_op(const char *guest,int op,int mode) {
    if (game_path(guest)) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    int r= op==0 ? mkdir(path,mode ? mode : 0755) : op==1 ? rmdir(path) : unlink(path);
    return traced(op==0 ? "mkdir" : op==1 ? "rmdir" : "unlink",guest,path,r ? -errno : 0);
}
static int64_t do_rename(const char *from,const char *to) {
    if (game_path(from) || game_path(to)) return -EROFS;
    char a[1024],b[1024];
    int e=translate(from,a,sizeof(a));
    if (!e) e=translate(to,b,sizeof(b));
    if (e) return -e;
#ifdef _WIN32
    /* rename() fails on Windows when the destination exists (POSIX replaces it), which breaks the
     * usual write-then-rename save pattern. As in the Mrsuss60 port: MoveFileExA replacing it. */
    int64_t r=0;
    if (!MoveFileExA(a,b,MOVEFILE_REPLACE_EXISTING|MOVEFILE_COPY_ALLOWED)) {
        const DWORD w=GetLastError();
        r = w==ERROR_FILE_NOT_FOUND || w==ERROR_PATH_NOT_FOUND ? -ENOENT : w==ERROR_ACCESS_DENIED ? -EACCES
          : w==ERROR_ALREADY_EXISTS || w==ERROR_FILE_EXISTS ? -EEXIST : w==ERROR_SHARING_VIOLATION ? -EBUSY : -EIO;
    }
#else
    const int64_t r=rename(a,b) ? -errno : 0;
#endif
    if (file_trace() || (runtime_save_trace() && (save_path(from) || save_path(to))))
        printf("FileTrace: rename %s -> %s = %lld\n",from,to,(long long)r);
    return r;
}
static int64_t do_ftruncate(int fd,int64_t length) {
    int h=host_fd_written(fd);
    if (h<0) return -EBADF;
    return ftruncate(h,length) ? -errno : 0;
}
static int64_t do_truncate(const char *guest,int64_t length) {
    if (game_path(guest)) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
    if (save_path(guest)) current_copy(path,sizeof(path),1);
    return traced("truncate",guest,path,truncate(path,length) ? -errno : 0);
}
static int64_t do_fsync(int fd) { int h=host_fd(fd); if (h<0) return -EBADF; return fsync(h) ? -errno : 0; }
static int64_t do_access(const char *guest,int mode) {
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return traced("access",guest,NULL,-e);
    if (save_path(guest)) current_copy(path,sizeof(path),0);
    return traced("access",guest,path,access(path,mode&7) ? -errno : 0);
}

/* Convention adapters: sceKernel* -> Orbis error codes, POSIX -> -1 + errno. */
static int64_t sce(int64_t r) { return r<0 ? ERR(runtime_guest_errno((int)-r)) : r; }
static int64_t posix(int64_t r) { if (r<0) { *runtime_errno()=runtime_guest_errno((int)-r); return -1; } return r; }
#define PAIR(name,params,args) \
    static ABI int64_t sce_##name params { return sce(do_##name args); } \
    static ABI int64_t posix_##name params { return posix(do_##name args); }
PAIR(open,(const char *p,int f,int m),(p,f,m))
PAIR(close,(int fd),(fd))
PAIR(read,(int fd,void *b,uint64_t n),(fd,b,n))
PAIR(pread,(int fd,void *b,uint64_t n,int64_t o),(fd,b,n,o))
PAIR(write,(int fd,const void *b,uint64_t n),(fd,b,n))
PAIR(pwrite,(int fd,const void *b,uint64_t n,int64_t o),(fd,b,n,o))
PAIR(lseek,(int fd,int64_t o,int w),(fd,o,w))
PAIR(fstat,(int fd,GuestStat *s),(fd,s))
PAIR(stat,(const char *p,GuestStat *s),(p,s))
PAIR(getdents,(int fd,char *b,uint64_t n,int64_t *base),(fd,b,n,base))
PAIR(rename,(const char *a,const char *b),(a,b))
PAIR(ftruncate,(int fd,int64_t l),(fd,l))
PAIR(truncate,(const char *p,int64_t l),(p,l))
PAIR(fsync,(int fd),(fd))
static ABI int64_t posix_access(const char *p,int m) { return posix(do_access(p,m)); }
static ABI int64_t sce_mkdir(const char *p,int m) { return sce(path_op(p,0,m)); }
static ABI int64_t posix_mkdir(const char *p,int m) { return posix(path_op(p,0,m)); }
static ABI int64_t sce_rmdir(const char *p) { return sce(path_op(p,1,0)); }
static ABI int64_t posix_rmdir(const char *p) { return posix(path_op(p,1,0)); }
static ABI int64_t sce_unlink(const char *p) { return sce(path_op(p,2,0)); }
static ABI int64_t posix_unlink(const char *p) { return posix(path_op(p,2,0)); }
static ABI int32_t sce_check_reachability(const char *p) {
    if (!p) return ERR(EFAULT);
    size_t n=strnlen(p,256);
    if (n==256) return (int32_t)UINT32_C(0x8002003f); /* KERNEL_ERROR_ENAMETOOLONG */
    if (!strcmp(p,"/")) return 0;
    static const char *const devices[]={
        "/dev/stdin","/dev/stdout","/dev/stderr","/dev/fd/0","/dev/fd/1","/dev/fd/2",
        "/dev/deci_stdin","/dev/deci_stdout","/dev/deci_stderr","/dev/null","/dev/urandom",
        "/dev/random","/dev/srandom","/dev/console","/dev/deci_tty6","/dev/deci_tty7",
        "/dev/rng","/dev/zero"
    };
    for (size_t i=0;i<sizeof(devices)/sizeof(devices[0]);++i)
        if (!strncmp(p,devices[i],strlen(devices[i]))) return 0;
    GuestStat s; return (int32_t)sce(do_stat(p,&s));
}

static const RuntimeExport exports[]={
    {"sceKernelOpen",sce_open}, {"open",posix_open}, {"_open",posix_open},
    {"sceKernelClose",sce_close}, {"close",posix_close}, {"_close",posix_close},
    {"sceKernelRead",sce_read}, {"read",posix_read}, {"_read",posix_read},
    {"sceKernelPread",sce_pread}, {"pread",posix_pread},
    {"sceKernelWrite",sce_write}, {"write",posix_write}, {"_write",posix_write},
    {"sceKernelPwrite",sce_pwrite}, {"pwrite",posix_pwrite},
    {"sceKernelLseek",sce_lseek}, {"lseek",posix_lseek},
    {"sceKernelFstat",sce_fstat}, {"fstat",posix_fstat},
    {"sceKernelStat",sce_stat}, {"stat",posix_stat},
    {"sceKernelGetdirentries",sce_getdents}, {"getdirentries",posix_getdents},
    {"sceKernelRename",sce_rename}, {"rename",posix_rename},
    {"sceKernelFtruncate",sce_ftruncate}, {"ftruncate",posix_ftruncate},
    {"sceKernelTruncate",sce_truncate}, {"truncate",posix_truncate},
    {"sceKernelFsync",sce_fsync}, {"fsync",posix_fsync},
    {"access",posix_access}, {"sceKernelCheckReachability",sce_check_reachability},
    {"sceKernelMkdir",sce_mkdir}, {"mkdir",posix_mkdir},
    {"sceKernelRmdir",sce_rmdir}, {"rmdir",posix_rmdir},
    {"sceKernelUnlink",sce_unlink}, {"unlink",posix_unlink},
};
uintptr_t runtime_file_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
/* Host-side helpers for other modules (e.g. SaveData, Fios). */
int64_t runtime_file_open(const char *p,int f,int m) { return do_open(p,f,m); }
int64_t runtime_file_close(int fd) { return do_close(fd); }
int64_t runtime_file_read(int fd,void *b,uint64_t n) { return do_read(fd,b,n); }
int64_t runtime_file_pread(int fd,void *b,uint64_t n,int64_t o) { return do_pread(fd,b,n,o); }
int64_t runtime_file_write(int fd,const void *b,uint64_t n) { return do_write(fd,b,n); }
int64_t runtime_file_lseek(int fd,int64_t o,int w) { return do_lseek(fd,o,w); }
int64_t runtime_file_stat(const char *p,void *s) { return do_stat(p,s); }
int64_t runtime_file_fstat(int fd,void *s) { return do_fstat(fd,s); }
int64_t runtime_file_getdents(int fd,char *b,uint64_t n,int64_t *base) { return do_getdents(fd,b,n,base); }
int runtime_file_translate(const char *guest,char *out,size_t size) { return translate(guest,out,size); }
void runtime_file_report(void) {
    printf("Runtime: files opened=%zu, reads=%zu (%llu bytes), writes=%zu, not found=%zu\n",
           opens,reads,(unsigned long long)bytes_read,writes,missing);
}
