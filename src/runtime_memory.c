/* PS4 virtual memory: direct (physical) pool, flexible memory, reservations,
 * protection changes and queries. One sparse memfd backs the whole direct
 * pool, so any mapping of any physical range aliases the same storage.
 * Guest addresses are placed below 1 TiB (PS4 user range): GPU descriptors
 * encode 40-bit addresses, so host-default 0x7f... addresses would not fit. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <sys/mman.h>  /* gpu/shim: PROT_*/MAP_* plus mprotect on VirtualProtect */
#include <unistd.h>    /* gpu/shim: gettid(), sysconf() */
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
/* sceKernelGetDirectMemorySize on retail PS4: 5056 MiB. BB_DMEM_MB raises it (the resolution
 * patches above 1080p need about 4 GiB more; run.sh sets it). */
static uint64_t pool_size_bytes(void) {
    static uint64_t size;
    if (!size) {
        const char *env = getenv("BB_DMEM_MB");
        uint64_t mb = env ? strtoull(env, NULL, 10) : 0;
        if (mb < 5056 || mb > 16384) mb = 5056;
        size = mb * 1024 * 1024;
    }
    return size;
}
#define POOL_SIZE pool_size_bytes()
#define FLEXIBLE_SIZE (UINT64_C(448) * 1024 * 1024)
#define PAGE UINT64_C(16384)
#define USER_MIN UINT64_C(0x1000000000)
#define USER_MAX UINT64_C(0xfc00000000)
#define LIMIT 4096
#define INVALID ((int32_t)UINT32_C(0x80020016))
#define NO_MEMORY ((int32_t)UINT32_C(0x8002000c))
#define ACCESS ((int32_t)UINT32_C(0x8002000d))
#define NO_SPACE ((int32_t)UINT32_C(0x80020023))
#define MAP_FIXED_FLAG 0x10
#define MAP_NO_OVERWRITE 0x80
enum { KIND_RESERVED=1, KIND_DIRECT, KIND_FLEXIBLE, KIND_POOL_RESERVED, KIND_POOL_COMMITTED };
static int kind_is_reserved(int kind) { return kind==KIND_RESERVED || kind==KIND_POOL_RESERVED; }
typedef struct { uint64_t start, size; int type, used; } Block;
typedef struct { uint64_t start, size, committed; int used; } PoolBacking;
typedef struct { uintptr_t start, end; int kind, prot, type; uint64_t phys; } Vma;
typedef struct {
    uintptr_t start, end; uint64_t offset; int32_t protection, memory_type;
    uint32_t flags; char name[32];
} VirtualQueryInfo; /* flags: flexible, direct, stack, pooled, committed */
typedef struct { void *start; uint64_t offset, length; int8_t prot, type; int16_t reserved; int32_t operation; } BatchEntry;
_Static_assert(sizeof(VirtualQueryInfo)==72,"PS4 virtual query info layout");
_Static_assert(sizeof(BatchEntry)==32,"PS4 batch map entry layout");

/* Region table lock. Lookups (GPU per-draw queries, VirtualQuery, GPU page
 * protection) share it; map/unmap/protect take it exclusively. Both nest: code
 * under the lock may write guest memory, and a GPU-tracked page then faults into
 * runtime_memory_gpu_protect on the same thread. Readers nest because glibc's
 * default rwlock prefers readers; a thread holding it exclusively skips relocking. */
static pthread_rwlock_t lock=PTHREAD_RWLOCK_INITIALIZER;
static __thread unsigned exclusive_depth;
/* Odd while a writer holds the lock; readers' region caches are valid for one even value. */
static uint64_t table_generation;
static void write_lock(void) {
    if (!exclusive_depth++) { pthread_rwlock_wrlock(&lock); __atomic_add_fetch(&table_generation,1,__ATOMIC_ACQ_REL); }
}
static void write_unlock(void) {
    if (!--exclusive_depth) { __atomic_add_fetch(&table_generation,1,__ATOMIC_RELEASE); pthread_rwlock_unlock(&lock); }
}
static void read_lock(void) { if (!exclusive_depth) pthread_rwlock_rdlock(&lock); }
static void read_unlock(void) { if (!exclusive_depth) pthread_rwlock_unlock(&lock); }
static Block blocks[LIMIT];
static PoolBacking pool_backing[LIMIT];
static Vma *vmas; static size_t vma_count, vma_capacity;
#ifndef _WIN32
static int pool_fd=-1;
#endif
/* bbport: direct memory in GPU-visible system memory the GPU library allocates (dma-buf chunks,
 * BB_GUEST_GPU_MEMORY=1): the GPU reads the game's data in place instead of the CPU copying it
 * into staging buffers first. Chunks are allocated when the game allocates direct memory in them;
 * a chunk the GPU library cannot provide stays in the memfd. */
#define CHUNK (UINT64_C(256) << 20)
typedef int (*GuestChunkAlloc)(uint64_t phys, uint64_t size);
static GuestChunkAlloc chunk_alloc;
#ifndef _WIN32
static int *chunk_fds; /* per CHUNK of direct memory: -1 not decided, -2 memfd, else a dma-buf fd */
#endif
/* Drivers whose dma-buf maps at offset 0 only (NVIDIA): a chunk per direct allocation instead of
 * the CHUNK grid. The game maps each of its allocations once, whole, so its mappings and the
 * backing view start at offset 0 of their chunk. */
static unsigned char *backing_base; /* second view of the pool: host writes bypass guest/GPU page protection */
static int whole_chunks;
#ifndef _WIN32
typedef struct { uint64_t phys, size; int fd; } Region;
#define MAX_REGIONS 256
static Region regions[MAX_REGIONS];
static size_t region_count;
/* The region holding phys (sorted by phys), or NULL; *next: start of the next region above. */
static const Region *region_at(uint64_t phys, uint64_t *next) {
    *next=POOL_SIZE;
    for (size_t i=0;i<region_count;++i) {
        if (phys<regions[i].phys) { *next=regions[i].phys; return NULL; }
        if (phys<regions[i].phys+regions[i].size) return &regions[i];
    }
    return NULL;
}
static void add_region(uint64_t phys, uint64_t size) {
    if (region_count==MAX_REGIONS) return;
    int fd=chunk_alloc(phys,size);
    if (fd>=0 && mmap(backing_base+phys,size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0)==MAP_FAILED) {
        close(fd);
        fd=-1;
    }
    size_t i=region_count;
    while (i>0 && regions[i-1].phys>phys) { regions[i]=regions[i-1]; --i; }
    regions[i]=(Region){phys,size,fd>=0 ? fd : -2};
    ++region_count;
}
#endif
#define FLEX_SPAN (UINT64_C(1024) * 1024 * 1024)
static uint64_t flex_bitmap[FLEX_SPAN/PAGE/64];
/* GPU hooks (bbgpu): notified outside the lock, in order, after each operation. */
typedef void (*GpuRange)(uintptr_t address, uint64_t size);
static GpuRange hook_map, hook_unmap, hook_invalidate;
static GpuRange hook_note_write; /* bbport: data written into GPU memory (runtime_memory_note_write) */
enum { HOOK_MAP, HOOK_UNMAP, HOOK_INVALIDATE };
typedef struct { int kind; uintptr_t address; uint64_t size; } PendingHook;
static PendingHook pending[64]; static size_t pending_count;
static size_t allocations, maps, flexible_maps, protects, queries;
static uint64_t live_bytes, flexible_bytes;

static int valid_alignment(uint64_t a) { return a >= PAGE && a <= POOL_SIZE && !(a & (a-1)); }
static uint64_t align_up(uint64_t n, uint64_t a) { return (n+a-1) & ~(a-1); }
static int host_prot(int prot) {
    /* GPU read/write bits (0x10/0x20) need host access for the future GPU backend. */
    return ((prot & 0x11) ? PROT_READ : 0) | ((prot & 0x22) ? PROT_WRITE|PROT_READ : 0) | ((prot & 4) ? PROT_EXEC|PROT_READ : 0);
}
#ifdef _WIN32
/* ---- host mappings (Windows) ----
 * [USER_MIN,USER_MAX) is covered by regions, each one placeholder or one view of the section.
 * The PS4 maps at 16 KiB granularity, while MapViewOfFileEx and VirtualAlloc place views and
 * reservations only at the 64 KiB allocation granularity (ERROR_MAPPED_ALIGNMENT, 1132). Placeholders
 * (VirtualAlloc2 MEM_RESERVE_PLACEHOLDER, Windows 10 1803+) can be split, coalesced and replaced by
 * views at page granularity; shadPS4 (src/core/address_space.cpp) and the Phroster Windows port
 * (windows/compat/memory.c) map guest memory this way, and this backend follows them. The API is
 * resolved from kernelbase.dll at run time, as in the Phroster port, so no import library is needed.
 *
 * The whole range is reserved as one placeholder before anything else runs (runtime_memory_reserve),
 * so nothing else in the process (heaps, Vulkan drivers) lands in it and any two adjacent
 * placeholders belong to the same reservation and can be coalesced. Setting [a,b) to a view or
 * back to a placeholder: the regions it overlaps become placeholders (views are unmapped with
 * MEM_PRESERVE_PLACEHOLDER), those are coalesced into one and split at a and b, and the parts of
 * views outside [a,b) are mapped again at the protection they had (GPU page tracking included).
 *
 * The PS4 user range is fixed, so the process image must not be allowed to land in it: the
 * launcher links with --disable-high-entropy-va. */
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
typedef struct { uintptr_t start, end; uint64_t phys; int mapped; } HostRegion;
/* phys == RESERVED_PHYS marks a region that is a bare placeholder, with no view. */
#define RESERVED_PHYS UINT64_MAX
static HostRegion *host_regions; static size_t host_region_count, host_region_capacity;
static HANDLE section;
typedef PVOID (WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef PVOID (WINAPI *MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef BOOL (WINAPI *UnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);
static VirtualAlloc2Fn virtual_alloc2;
static MapViewOfFile3Fn map_view3;
static UnmapViewOfFile2Fn unmap_view2;
static DWORD win_prot(int prot) {
    if (prot & PROT_EXEC) return (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : (prot & PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    return (prot & PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
}
static void win_fail(const char *what, uintptr_t address, uint64_t size) {
    fprintf(stderr,"STOP: %s failed at %p (+0x%" PRIx64 "), error %lu\n",what,(void *)address,size,GetLastError());
    exit(21);
}
static size_t host_region_index(uintptr_t a) { /* first region with end > a */
    size_t lo=0, hi=host_region_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (host_regions[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
static void host_region_insert(size_t at, HostRegion r) {
    if (host_region_count==host_region_capacity) {
        size_t capacity=host_region_capacity ? host_region_capacity*2 : 256;
        HostRegion *next=realloc(host_regions,capacity*sizeof(*next));
        if (!next) { fputs("Out of memory (host regions)\n",stderr); exit(1); }
        host_regions=next; host_region_capacity=capacity;
    }
    memmove(host_regions+at+1,host_regions+at,(host_region_count-at)*sizeof(*host_regions));
    host_regions[at]=r; ++host_region_count;
}
static void host_region_erase(size_t at, size_t n) {
    memmove(host_regions+at,host_regions+at+n,(host_region_count-at-n)*sizeof(*host_regions));
    host_region_count-=n;
}
/* Reserves the whole guest range as one placeholder; the first region covers all of it. */
void runtime_memory_reserve(void) {
    static int done;
    if (done++) return;
    HMODULE kernelbase=GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase) {
        virtual_alloc2=(VirtualAlloc2Fn)(void *)GetProcAddress(kernelbase,"VirtualAlloc2");
        map_view3=(MapViewOfFile3Fn)(void *)GetProcAddress(kernelbase,"MapViewOfFile3");
        unmap_view2=(UnmapViewOfFile2Fn)(void *)GetProcAddress(kernelbase,"UnmapViewOfFile2");
    }
    if (!virtual_alloc2 || !map_view3 || !unmap_view2) {
        fputs("STOP: no placeholder memory API (VirtualAlloc2 needs Windows 10 1803 or later)\n",stderr);
        exit(21);
    }
    if (virtual_alloc2(GetCurrentProcess(),(void *)USER_MIN,USER_MAX-USER_MIN,
                       MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0)!=(void *)USER_MIN) {
        const DWORD error=GetLastError();
        /* Name what already occupies the range, so the failure says which module or heap it is. */
        MEMORY_BASIC_INFORMATION m;
        for (uintptr_t at=USER_MIN; at<USER_MAX && VirtualQuery((void *)at,&m,sizeof m); at=(uintptr_t)m.BaseAddress+m.RegionSize) {
            if (m.State==MEM_FREE) continue;
            char module[MAX_PATH]="";
            if (m.Type==MEM_IMAGE) GetModuleFileNameA((HMODULE)m.AllocationBase,module,sizeof module);
            fprintf(stderr,"Runtime: user range occupied: %p +0x%zx state=0x%lx type=0x%lx base=%p %s\n",
                    m.BaseAddress,(size_t)m.RegionSize,m.State,m.Type,m.AllocationBase,module);
        }
        SetLastError(error);
        win_fail("reserving the PS4 user range as a placeholder",USER_MIN,USER_MAX-USER_MIN);
    }
    host_region_insert(0,(HostRegion){USER_MIN,USER_MAX,RESERVED_PHYS,0});
}
/* Replaces placeholder r (exactly its extent) with a view of the section at r->phys. */
static void view_map(HostRegion *r, DWORD protect) {
    void *p=map_view3(section,GetCurrentProcess(),(void *)r->start,r->phys,r->end-r->start,
                      MEM_REPLACE_PLACEHOLDER,PAGE_READWRITE,NULL,0);
    if (p!=(void *)r->start) win_fail("MapViewOfFile3",r->start,r->end-r->start);
    if (protect!=PAGE_READWRITE) {
        DWORD old;
        if (!VirtualProtect((void *)r->start,r->end-r->start,protect,&old))
            win_fail("VirtualProtect",r->start,r->end-r->start);
    }
    r->mapped=1;
}
/* Turns a view back into a placeholder of the same extent. */
static void view_unmap(HostRegion *r) {
    if (r->mapped && !unmap_view2(GetCurrentProcess(),(void *)r->start,MEM_PRESERVE_PLACEHOLDER))
        win_fail("UnmapViewOfFile2",r->start,r->end-r->start);
    r->mapped=0; r->phys=RESERVED_PHYS;
}
/* Splits [start,start+size) off the front of the placeholder that begins at start. */
static void placeholder_split(uintptr_t start, uint64_t size) {
    if (!VirtualFree((void *)start,size,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))
        win_fail("VirtualFree(MEM_PRESERVE_PLACEHOLDER)",start,size);
}
static int host_protect(uintptr_t address, uint64_t size, int prot) {
    DWORD protect=win_prot(prot), old;
    uintptr_t end=address+size;
    for (size_t i=host_region_index(address); i<host_region_count && host_regions[i].start<end; ++i) {
        if (!host_regions[i].mapped) continue;
        uintptr_t a=host_regions[i].start>address ? host_regions[i].start : address, b=host_regions[i].end<end ? host_regions[i].end : end;
        if (!VirtualProtect((void *)a,b-a,protect,&old)) return -1;
    }
    return 0;
}
/* Direct memory occupies [0,POOL_SIZE) of the section, flexible memory [POOL_SIZE,+FLEX_SPAN). */
static int pool(void) {
    if (backing_base) return 0;
    runtime_memory_reserve();
    const uint64_t bytes=POOL_SIZE+FLEX_SPAN;
    section=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,
                               (DWORD)(bytes>>32),(DWORD)(bytes&0xffffffffu),NULL);
    if (!section) { fprintf(stderr,"Runtime: cannot create %" PRIu64 " MiB of guest memory (error %lu); is the page file large enough?\n",bytes>>20,GetLastError()); return -1; }
    backing_base=MapViewOfFile(section,FILE_MAP_ALL_ACCESS,0,0,bytes);
    return backing_base ? 0 : -1;
}
/* Makes [a,b) one region: a view of the section at phys with protection protect, or a bare
 * placeholder when phys is RESERVED_PHYS. Everything outside [a,b) keeps its mapping and the
 * protection it had, so a split never loses or widens guest access. Returns the region's index. */
static size_t host_set_range(uintptr_t a, uintptr_t b, uint64_t phys, DWORD protect) {
    if (a<USER_MIN || b>USER_MAX || a>=b) win_fail("mapping outside the PS4 user range",a,b-a);
    size_t first=host_region_index(a), last=first;
    while (last<host_region_count && host_regions[last].start<b) ++last;
    const HostRegion left=host_regions[first], right=host_regions[last-1];
    const int keep_left=left.start<a, keep_right=right.end>b;
    DWORD left_prot=PAGE_READWRITE, right_prot=PAGE_READWRITE;
    MEMORY_BASIC_INFORMATION m;
    if (keep_left && left.mapped && VirtualQuery((void *)(a-1),&m,sizeof m)) left_prot=m.Protect;
    if (keep_right && right.mapped && VirtualQuery((void *)b,&m,sizeof m)) right_prot=m.Protect;
    for (size_t i=first;i<last;++i) view_unmap(&host_regions[i]);
    /* [left.start,right.end) is now last-first adjacent placeholders of one reservation. */
    if (last-first>1 && !VirtualFree((void *)left.start,right.end-left.start,MEM_RELEASE|MEM_COALESCE_PLACEHOLDERS))
        win_fail("VirtualFree(MEM_COALESCE_PLACEHOLDERS)",left.start,right.end-left.start);
    host_region_erase(first,last-first);
    size_t at=first;
    if (keep_left) {
        placeholder_split(left.start,a-left.start);
        host_region_insert(at,(HostRegion){left.start,a,left.mapped ? left.phys : RESERVED_PHYS,0});
        if (left.mapped) view_map(&host_regions[at],left_prot);
        ++at;
    }
    if (keep_right) placeholder_split(a,b-a);
    const size_t middle=at;
    host_region_insert(at,(HostRegion){a,b,phys,0});
    if (phys!=RESERVED_PHYS) view_map(&host_regions[at],protect);
    ++at;
    if (keep_right) {
        host_region_insert(at,(HostRegion){b,right.end,right.mapped ? right.phys+(b-right.start) : RESERVED_PHYS,0});
        if (right.mapped) view_map(&host_regions[at],right_prot);
    }
    return middle;
}
/* Reserves [a,b) for the guest: a placeholder, so any access faults until it is mapped. */
static size_t host_reserve_range(uintptr_t a, uintptr_t b) { return host_set_range(a,b,RESERVED_PHYS,PAGE_NOACCESS); }
/* Turns [a,b) into a view of the section at phys. */
static size_t host_map_range(uintptr_t a, uintptr_t b, int prot, uint64_t phys) { return host_set_range(a,b,phys,win_prot(prot)); }
/* Releases [a,b) and leaves it reserved for a later mapping. */
static void host_unmap_range(uintptr_t a, uintptr_t b) { host_set_range(a,b,RESERVED_PHYS,PAGE_NOACCESS); }
#else
/* Direct memory occupies [0,POOL_SIZE) of the memfd, flexible memory [POOL_SIZE,+FLEX_SPAN). */
static int pool(void) {
    if (pool_fd>=0) return 0;
    pool_fd=memfd_create("bb-guest-memory", MFD_CLOEXEC);
    if (pool_fd<0 || ftruncate(pool_fd,(off_t)(POOL_SIZE+FLEX_SPAN))) return -1;
    void *view=mmap(NULL,POOL_SIZE+FLEX_SPAN,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_NORESERVE,pool_fd,0);
    if (view==MAP_FAILED) return -1;
    backing_base=view;
    chunk_fds=malloc((POOL_SIZE/CHUNK+1)*sizeof(int));
    if (!chunk_fds) return -1;
    for (uint64_t c=0;c<=POOL_SIZE/CHUNK;++c) chunk_fds[c]=-1;
    return 0;
}
#endif
#ifndef _WIN32
/* Size of direct memory chunk c (the last one may be shorter). */
static uint64_t chunk_size(uint64_t c) {
    const uint64_t start=c*CHUNK;
    return POOL_SIZE-start<CHUNK ? POOL_SIZE-start : CHUNK;
}
/* Decides where the chunks of [phys, phys+size) of direct memory live (lock held). */
static void ensure_chunks(uint64_t phys, uint64_t size) {
    if (!chunk_fds || phys>=POOL_SIZE) return;
    if (whole_chunks) {
        if (!chunk_alloc) return;
        const uint64_t end=phys+size<POOL_SIZE ? phys+size : POOL_SIZE;
        for (uint64_t at=phys;at<end;) {
            uint64_t next;
            const Region *r=region_at(at,&next);
            if (r) { at=r->phys+r->size; continue; }
            const uint64_t gap_end=next<end ? next : end;
            add_region(at,gap_end-at);
            at=gap_end;
        }
        return;
    }
    const uint64_t last=(phys+size-1<POOL_SIZE ? phys+size-1 : POOL_SIZE-1)/CHUNK;
    for (uint64_t c=phys/CHUNK;c<=last;++c) {
        if (chunk_fds[c]!=-1) continue;
        int fd=chunk_alloc ? chunk_alloc(c*CHUNK,chunk_size(c)) : -1;
        if (fd>=0 && mmap(backing_base+c*CHUNK,chunk_size(c),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0)==MAP_FAILED) {
            close(fd);
            fd=-1;
        }
        chunk_fds[c]=fd>=0 ? fd : -2;
    }
}
#else
/* No dma-buf chunks on Windows: every direct range is a view of the one section. */
static void ensure_chunks(uint64_t phys, uint64_t size) { (void)phys; (void)size; }
#endif /* !_WIN32: the dma-buf chunk grid is Linux-only */
/* mmap of direct or flexible memory at phys, split where it crosses chunks. */
static void *map_phys(uintptr_t address, uint64_t size, int prot, uint64_t phys) {
#ifdef _WIN32
    /* One view of the section per range at a fixed guest address; no dma-buf, so no chunk split. */
    host_map_range(address,address+size,host_prot(prot),phys);
    return (void *)address;
#else
    if (!chunk_fds || phys>=POOL_SIZE)
        return mmap((void *)address,size,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)phys);
    ensure_chunks(phys,size);
    for (uint64_t done=0;whole_chunks && done<size;) {
        const uint64_t at=phys+done;
        uint64_t next;
        const Region *r=region_at(at,&next);
        const uint64_t room=r ? r->phys+r->size-at : next-at, n=size-done<room ? size-done : room;
        void *p = r && r->fd>=0
            ? mmap((void *)(address+done),n,prot,MAP_SHARED|MAP_FIXED,r->fd,(off_t)(at-r->phys))
            : mmap((void *)(address+done),n,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)at);
        if (p==MAP_FAILED) {
            static int reported;
            if (reported++ < 4)
                fprintf(stderr,"Runtime: mapping direct memory %#" PRIx64 "+%#" PRIx64 " from %s failed: %s\n",
                        at,n,r && r->fd>=0 ? "a GPU-visible dma-buf (whole-allocation chunk)" : "the memfd",strerror(errno));
            return MAP_FAILED;
        }
        done+=n;
        if (done==size) return (void *)address;
    }
    for (uint64_t done=0;done<size;) {
        const uint64_t at=phys+done, c=at/CHUNK;
        const uint64_t room=c*CHUNK+chunk_size(c)-at, n=size-done<room ? size-done : room;
        void *p = chunk_fds[c]>=0
            ? mmap((void *)(address+done),n,prot,MAP_SHARED|MAP_FIXED,chunk_fds[c],(off_t)(at-c*CHUNK))
            : mmap((void *)(address+done),n,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)at);
        if (p==MAP_FAILED) {
            static int reported;
            if (reported++ < 4)
                fprintf(stderr,"Runtime: mapping direct memory %#" PRIx64 "+%#" PRIx64 " from %s failed: %s\n",
                        at,n,chunk_fds[c]>=0 ? "a GPU-visible dma-buf chunk" : "the memfd",strerror(errno));
            return MAP_FAILED;
        }
        done+=n;
    }
    return (void *)address;
#endif /* _WIN32 */
}
/* Returns released physical memory to zero: hole punching in the memfd, a clear in a chunk. */
static void zero_phys(uint64_t phys, uint64_t size) {
#ifdef _WIN32
    /* The section is already committed, so released pages read as zero once they are cleared. */
    memset(backing_base+phys,0,size);
#else
    for (uint64_t done=0;done<size;) {
        const uint64_t at=phys+done;
        if (!chunk_fds || at>=POOL_SIZE) {
            fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)at,(off_t)(size-done));
            return;
        }
        if (whole_chunks) {
            uint64_t next;
            const Region *r=region_at(at,&next);
            const uint64_t room=r ? r->phys+r->size-at : next-at, n=size-done<room ? size-done : room;
            if (r && r->fd>=0) memset(backing_base+at,0,n);
            else fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)at,(off_t)n);
            done+=n;
            continue;
        }
        const uint64_t c=at/CHUNK, room=c*CHUNK+chunk_size(c)-at, n=size-done<room ? size-done : room;
        if (chunk_fds[c]>=0) memset(backing_base+at,0,n);
        else fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)at,(off_t)n);
        done+=n;
    }
#endif /* _WIN32 */
}
static void queue_hook(int kind, uintptr_t address, uint64_t size) {
    GpuRange hook = kind==HOOK_MAP ? hook_map : kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
    if (!hook) return;
    if (pending_count==sizeof(pending)/sizeof(*pending)) { fputs("STOP: GPU hook queue overflow\n",stderr); exit(21); }
    pending[pending_count++]=(PendingHook){kind,address,size};
}
/* Runs queued GPU notifications; callers invoke it after dropping the lock. */
static void flush_hooks(void) {
    for (;;) {
        write_lock();
        if (!pending_count) { write_unlock(); return; }
        PendingHook h=pending[0];
        memmove(pending,pending+1,--pending_count*sizeof(*pending));
        GpuRange hook = h.kind==HOOK_MAP ? hook_map : h.kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
        write_unlock();
        if (hook) hook(h.address,h.size);
    }
}
static int flex_test(uint64_t page) { return (int)((flex_bitmap[page/64]>>(page%64))&1); }
static void flex_set(uint64_t first, uint64_t count, int used) {
    for (uint64_t p=first;p<first+count;++p) {
        if (used) flex_bitmap[p/64]|=UINT64_C(1)<<(p%64); else flex_bitmap[p/64]&=~(UINT64_C(1)<<(p%64));
    }
}
static uint64_t flex_alloc(uint64_t size) {
    uint64_t pages=size/PAGE, total=FLEX_SPAN/PAGE, run=0;
    for (uint64_t p=0;p<total;++p) {
        run = flex_test(p) ? 0 : run+1;
        if (run==pages) { flex_set(p+1-pages,pages,1); return POOL_SIZE+(p+1-pages)*PAGE; }
    }
    return UINT64_MAX;
}
static void flex_free(uint64_t phys, uint64_t size) {
    flex_set((phys-POOL_SIZE)/PAGE,size/PAGE,0);
#ifdef _WIN32
    memset(backing_base+phys,0,size);
#else
    fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)phys,(off_t)size);
#endif
}
static size_t vma_index(uintptr_t a) { /* first VMA with end > a */
    size_t lo=0, hi=vma_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (vmas[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
static int vma_insert(size_t at, Vma v) {
    if (vma_count==vma_capacity) {
        size_t capacity=vma_capacity ? vma_capacity*2 : 256;
        Vma *next=realloc(vmas,capacity*sizeof(*vmas));
        if (!next) return -1;
        vmas=next; vma_capacity=capacity;
    }
    memmove(vmas+at+1,vmas+at,(vma_count-at)*sizeof(*vmas));
    vmas[at]=v; ++vma_count; return 0;
}
static void vma_erase(size_t at, size_t n) {
    memmove(vmas+at,vmas+at+n,(vma_count-at-n)*sizeof(*vmas)); vma_count-=n;
}
static int split_at(uintptr_t a) {
    size_t i=vma_index(a);
    if (i==vma_count || vmas[i].start>=a) return 0;
    Vma right=vmas[i];
    right.start=a;
    if (right.kind==KIND_DIRECT || right.kind==KIND_POOL_COMMITTED) right.phys+=a-vmas[i].start;
    vmas[i].end=a;
    return vma_insert(i+1,right);
}
/* Range [start,end) split on its edges; returns the first index inside it. */
static size_t carve(uintptr_t start, uintptr_t end, int *error) {
    *error = split_at(start) || split_at(end);
    return vma_index(start);
}
static int covered(uintptr_t start, uintptr_t end, int allow_reserved) {
    size_t i=vma_index(start); uintptr_t at=start;
    for (; at<end; ++i) {
        if (i==vma_count || vmas[i].start>at || (!allow_reserved && kind_is_reserved(vmas[i].kind))) return 0;
        at=vmas[i].end;
    }
    return 1;
}
static int overlaps(uintptr_t start, uintptr_t end, int ignore_reserved) {
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (!ignore_reserved || !kind_is_reserved(vmas[i].kind)) return 1;
    return 0;
}
/* First-fit search in the PS4 user range, starting from the hint. */
static uintptr_t find_free(uintptr_t hint, uint64_t size, uint64_t alignment) {
    uintptr_t at=align_up(hint<USER_MIN ? USER_MIN : hint, alignment);
    for (size_t i=vma_index(at); at+size<=USER_MAX; ++i) {
        if (i==vma_count || vmas[i].start>=at+size) return at;
        at=align_up(vmas[i].end,alignment);
    }
    return 0;
}
static void drop_range(uintptr_t start, uintptr_t end) {
    int error; size_t i=carve(start,end,&error), n=0;
    while (i+n<vma_count && vmas[i+n].end<=end && vmas[i+n].start>=start) {
        if (vmas[i+n].kind==KIND_FLEXIBLE) {
            flexible_bytes-=vmas[i+n].end-vmas[i+n].start;
            flex_free(vmas[i+n].phys,vmas[i+n].end-vmas[i+n].start);
        }
        ++n;
    }
    vma_erase(i,n);
}
/* Places a host mapping; with MAP_FIXED it replaces what the guest had there. */
static int32_t place(void **inout, uint64_t size, int prot, int flags, uint64_t alignment,
                     int kind, int type, uint64_t phys) {
    uintptr_t address=(uintptr_t)*inout;
    if (flags & MAP_FIXED_FLAG) {
        if (!address || address%PAGE) return INVALID;
        if ((flags & MAP_NO_OVERWRITE) && overlaps(address,address+size,1)) return NO_MEMORY;
    } else {
        address=find_free(address,size,alignment);
        if (!address) return NO_MEMORY;
    }
    void *mapped;
#ifdef _WIN32
    /* Reserved kinds keep a placeholder (PAGE_NOACCESS) until the guest commits them; the rest
     * get a view of the direct/flexible memory at phys. */
    if (kind_is_reserved(kind)) {
        host_reserve_range(address,address+size);
        mapped=(void *)address;
    } else
        mapped=map_phys(address,size,host_prot(prot),phys);
#else
    mapped = !kind_is_reserved(kind)
        ? map_phys(address,size,host_prot(prot),phys)
        : mmap((void *)address,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED,-1,0);
#endif
    if (mapped==MAP_FAILED) return NO_MEMORY;
    drop_range(address,address+size);
    if (vma_insert(vma_index(address),(Vma){address,address+size,kind,prot,type,phys})) return NO_MEMORY;
    if (kind==KIND_FLEXIBLE) flexible_bytes+=size;
    if (!kind_is_reserved(kind)) queue_hook(HOOK_MAP,address,size);
    *inout=mapped;
    return 0;
}
static int block_type(uint64_t phys) {
    for (int i=0;i<LIMIT;++i)
        if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) return blocks[i].type;
    return -1;
}
static int direct_range_allocated(uint64_t phys, uint64_t size) {
    for (uint64_t at=phys; at<phys+size;) {
        int found=0;
        for (int i=0;i<LIMIT;++i) if (blocks[i].used && at>=blocks[i].start && at-blocks[i].start<blocks[i].size) {
            at=blocks[i].start+blocks[i].size; found=1; break;
        }
        if (!found) return 0;
    }
    return 1;
}

static ABI uint64_t direct_size(void) { return POOL_SIZE; }
static ABI int32_t direct_allocate(int64_t low, int64_t high, uint64_t size,
                                  uint64_t alignment, int type, int64_t *out) {
    if (!alignment) alignment = PAGE;
    if (!out || low < 0 || high < 0 || !size || size % PAGE || !valid_alignment(alignment) || type < 0 || type > 10)
        return INVALID;
    uint64_t end = (uint64_t)high < POOL_SIZE ? (uint64_t)high : POOL_SIZE;
    if ((uint64_t)low >= end || size > end - (uint64_t)low) return NO_SPACE;
    write_lock();
    if (pool()) { write_unlock(); return NO_MEMORY; }
    uint64_t start = align_up((uint64_t)low, alignment);
    int slot = -1;
    for (int i=0; i<LIMIT; ++i) if (!blocks[i].used) { slot=i; break; }
    for (;;) {
        if (slot<0 || start > end || size > end-start) { write_unlock(); return NO_SPACE; }
        int overlap = 0;
        for (int i=0; i<LIMIT; ++i) {
            Block b = blocks[i];
            if (b.used && start < b.start+b.size && b.start < start+size) {
                start = align_up(b.start+b.size, alignment); overlap = 1; break;
            }
        }
        if (!overlap) break;
    }
    blocks[slot] = (Block){start, size, type, 1}; *out = (int64_t)start;
    ensure_chunks(start,size);
    ++allocations; live_bytes += size;
    write_unlock();
    printf("Runtime: direct allocation offset=0x%" PRIx64 ", size=%" PRIu64 " bytes, type=%d\n", start, size, type);
    return 0;
}
/* sceKernelMemoryPoolExpand reserves direct-memory backing for later pool commits.
 * Keep the PS4 ABI and validation aligned with shadPS4's libkernel implementation;
 * type 3 is the memory type assigned to expanded pool backing there as well. */
static ABI int32_t memory_pool_expand(uint64_t search_start, uint64_t search_end,
                                     uint64_t size, uint64_t alignment, uint64_t *physical_out) {
    const uint64_t pool_alignment = UINT64_C(64) * 1024;
    if (search_end <= search_start || !size || size % pool_alignment ||
        (alignment && alignment % pool_alignment) || !physical_out)
        return INVALID;
    if (search_end - search_start < size) return NO_MEMORY;
    if (!alignment) alignment = pool_alignment;
    if (search_start > INT64_MAX || search_end > INT64_MAX) return INVALID;
    int64_t physical = 0;
    write_lock();
    int slot=-1;
    for (int i=0;i<LIMIT;++i) if (!pool_backing[i].used) { slot=i; break; }
    if (slot<0) { write_unlock(); return NO_MEMORY; }
    int32_t result = direct_allocate((int64_t)search_start, (int64_t)search_end,
                                     size, alignment, 3, &physical);
    if (result == NO_SPACE) { write_unlock(); return NO_MEMORY; }
    if (result) { write_unlock(); return result; }
    pool_backing[slot]=(PoolBacking){(uint64_t)physical,size,0,1};
    *physical_out = (uint64_t)physical;
    write_unlock();
    return 0;
}
static int32_t map_direct_locked(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!out || physical < 0 || (uint64_t)physical % PAGE || !size || size % PAGE || !valid_alignment(alignment) ||
        (prot & ~0x37)) return INVALID;
    if (!direct_range_allocated((uint64_t)physical,size)) return INVALID;
    int32_t result=place(out,size,prot,flags,alignment,KIND_DIRECT,block_type((uint64_t)physical),(uint64_t)physical);
    if (!result) ++maps;
    return result;
}
static ABI int32_t direct_map(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    write_lock();
    int32_t result=map_direct_locked(out,size,prot,flags,physical,alignment);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped direct memory %p, size=%" PRIu64 ", prot=0x%x\n", *out, size, prot);
    else printf("Runtime: mapping direct memory %#" PRIx64 ", size=%" PRIu64 " failed (error %#x)\n", (uint64_t)physical, size, (unsigned)result);
    return result;
}
static ABI int32_t direct_map_named(void **out, uint64_t size, int prot, int flags, int64_t physical,
                                    uint64_t alignment, const char *name) {
    (void)name; return direct_map(out,size,prot,flags,physical,alignment);
}
static int32_t unmap_locked(uintptr_t start, uint64_t size) {
    if (start%PAGE || !size) return INVALID;
    uint64_t end=start+align_up(size,PAGE);
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (!kind_is_reserved(vmas[i].kind)) {
            uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
            queue_hook(HOOK_UNMAP,a,b-a);
        }
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i) {
        uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
#ifdef _WIN32
        host_unmap_range(a,b);
#else
        if (munmap((void *)a,b-a)) return INVALID;
#endif
    }
    drop_range(start,end);
    return 0;
}
static ABI int32_t direct_unmap(void *address, uint64_t size) {
    /* The GPU forgets the range before it disappears (as shadPS4's early GPU unmap). */
    if (hook_unmap && !((uintptr_t)address%PAGE) && size) hook_unmap((uintptr_t)address,align_up(size,PAGE));
    write_lock();
    size_t before=pending_count;
    int32_t result=unmap_locked((uintptr_t)address,size);
    pending_count=before; /* already notified above */
    write_unlock();
    return result;
}
static ABI int32_t direct_release(uint64_t start, uint64_t size) {
    if (start % PAGE || size % PAGE) return INVALID;
    if (!size) return 0;
    write_lock();
    if (!direct_range_allocated(start,size)) { write_unlock(); return INVALID; }
    /* Mappings of released physical pages disappear with them. */
    for (size_t i=0; i<vma_count;) {
        Vma v=vmas[i];
        uint64_t length=v.end-v.start;
        if ((v.kind==KIND_DIRECT || v.kind==KIND_POOL_COMMITTED) && v.phys<start+size && start<v.phys+length) {
            uint64_t a=v.phys>start ? v.phys : start, b=v.phys+length<start+size ? v.phys+length : start+size;
            unmap_locked(v.start+(a-v.phys),b-a);
            i=vma_index(v.start);
            continue;
        }
        ++i;
    }
    for (int i=0; i<LIMIT; ++i) {
        Block *b=&blocks[i];
        if (!b->used || b->start>=start+size || start>=b->start+b->size) continue;
        uint64_t a=b->start>start ? b->start : start, e=b->start+b->size<start+size ? b->start+b->size : start+size;
        Block left=*b, right=*b;
        left.size=a-b->start; right.start=e; right.size=b->start+b->size-e;
        b->used=0;
        if (left.size) *b=left;
        if (right.size) for (int j=0;j<LIMIT;++j) if (!blocks[j].used) { blocks[j]=right; break; }
        live_bytes-=e-a;
    }
    zero_phys(start,size); /* zero on reuse */
    write_unlock();
    flush_hooks();
    return 0;
}
static int32_t map_flexible_locked(void **inout, uint64_t size, int prot, int flags) {
    if (!inout || !size || size%PAGE || (prot & ~0x37)) return INVALID;
    if (flexible_bytes+size>FLEXIBLE_SIZE || pool()) return NO_MEMORY;
    uint64_t phys=flex_alloc(size);
    if (phys==UINT64_MAX) return NO_MEMORY;
    int32_t result=place(inout,size,prot,flags,PAGE,KIND_FLEXIBLE,0,phys);
    if (result) flex_free(phys,size); else ++flexible_maps;
    return result;
}
static ABI int32_t map_flexible(void **inout, uint64_t size, int prot, int flags) {
    write_lock();
    int32_t result=map_flexible_locked(inout,size,prot,flags);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped flexible memory %p, size=%" PRIu64 ", prot=0x%x\n", *inout, size, prot);
    else fprintf(stderr,"Runtime: flexible map of %" PRIu64 " bytes failed (0x%x), in use=%" PRIu64 "\n",
                 size, (unsigned)result, flexible_bytes);
    return result;
}
static ABI int32_t map_flexible_named(void **inout, uint64_t size, int prot, int flags, const char *name) {
    (void)name; return map_flexible(inout,size,prot,flags);
}
static ABI int32_t release_flexible(void *address, uint64_t size) { return direct_unmap(address,size); }
/* POSIX mmap: the game's libc grows its malloc heap with anonymous private mappings once the
 * heap it started with is full (seen after minutes of area streaming). They come from flexible
 * memory as sceKernelMapFlexibleMemory does; files are not mapped. MAP_FAILED (-1) and errno on
 * failure, as POSIX. */
static _Atomic uint64_t heap_growths;
uint64_t runtime_heap_growths(void) { return atomic_load(&heap_growths); }
static ABI void *posix_mmap(void *addr, uint64_t size, int prot, int flags, int fd, int64_t offset) {
    atomic_fetch_add(&heap_growths,1);
    enum { MAP_ANON_FLAG=0x1000, POSIX_ENOMEM=12, POSIX_ENODEV=19, POSIX_EINVAL=22 };
    if (!(flags & MAP_ANON_FLAG) || fd!=-1 || offset) { *runtime_errno()=POSIX_ENODEV; return (void *)-1; }
    if (!size) { *runtime_errno()=POSIX_EINVAL; return (void *)-1; }
    void *out=(flags & MAP_FIXED_FLAG) ? addr : NULL;
    const int32_t result=map_flexible(&out,align_up(size,PAGE),prot & 0x37,flags & MAP_FIXED_FLAG);
    uint64_t sites[3];
    runtime_guest_call_sites(sites);
    printf("Runtime: mmap %#" PRIx64 " bytes for the game (its heap grows) from +%#" PRIx64 " < +%#" PRIx64 " < +%#" PRIx64 "\n",
           size,sites[0],sites[1],sites[2]);
    if (result) { *runtime_errno()=result==NO_MEMORY ? POSIX_ENOMEM : POSIX_EINVAL; return (void *)-1; }
    return out;
}
static ABI int32_t reserve_range(void **inout, uint64_t size, int flags, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!inout || !size || size%PAGE || !valid_alignment(alignment)) return INVALID;
    write_lock();
    if ((flags & MAP_FIXED_FLAG) && overlaps((uintptr_t)*inout,(uintptr_t)*inout+size,0)) {
        write_unlock(); return NO_MEMORY;
    }
    int32_t result=place(inout,size,0,flags,alignment,KIND_RESERVED,0,0);
    write_unlock();
    if (!result) printf("Runtime: reserved virtual range %p, size=%" PRIu64 "\n", *inout, size);
    return result;
}
/* sceKernelMemoryPoolReserve reserves virtual address space for later pool commits.
 * It is intentionally distinct from MemoryPoolExpand, which reserves physical backing. */
static ABI int32_t memory_pool_reserve(void *address_in, uint64_t size, uint64_t alignment,
                                      int flags, void **address_out) {
    const uint64_t pool_alignment=UINT64_C(2)*1024*1024;
    if (!address_out || !size || size%pool_alignment) return INVALID;
    if (alignment && ((!valid_alignment(alignment) && alignment%pool_alignment) ||
                      (alignment%PAGE))) return INVALID;
    if (!alignment) alignment=pool_alignment;
    uintptr_t hint=(uintptr_t)address_in;
    if (hint>=USER_MAX || size>USER_MAX-USER_MIN) return INVALID;
    if (flags&MAP_FIXED_FLAG) {
        if (hint<USER_MIN || hint%PAGE || size>USER_MAX-hint) return INVALID;
    }
    void *out=address_in;
    write_lock();
    int32_t result;
    if ((flags&(MAP_FIXED_FLAG|MAP_NO_OVERWRITE))==(MAP_FIXED_FLAG|MAP_NO_OVERWRITE) &&
        overlaps(hint,hint+(uintptr_t)size,0)) result=NO_MEMORY;
    else result=place(&out,size,0,flags,alignment,KIND_POOL_RESERVED,0,0);
    write_unlock();
    if (!result) {
        *address_out=out;
        printf("Runtime: reserved memory-pool virtual range %p, size=%" PRIu64 "\n",out,size);
    }
    return result;
}
typedef struct { int backing; uint64_t physical, size; } PoolCommitPart;
static ABI int32_t memory_pool_commit(void *address, uint64_t size, int type, int prot, int flags) {
    const uint64_t pool_alignment=UINT64_C(64)*1024;
    (void)flags;
    if (!address || !size || size%pool_alignment || (prot&4)) return INVALID;
    uintptr_t start=(uintptr_t)align_up((uintptr_t)address,pool_alignment);
    if (start<USER_MIN || start>=USER_MAX || size>USER_MAX-start) return INVALID;
    const uintptr_t end=start+(uintptr_t)size;
    PoolCommitPart parts[LIMIT];
    size_t part_count=0;
    uint64_t remaining=size;

    write_lock();
    for (uintptr_t at=start;at<end;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind!=KIND_POOL_RESERVED) {
            write_unlock(); return INVALID;
        }
        at=vmas[i].end<end ? vmas[i].end : end;
    }
    for (int i=0;i<LIMIT && remaining;++i) {
        PoolBacking *b=&pool_backing[i];
        if (!b->used || b->committed>=b->size) continue;
        uint64_t available=b->size-b->committed;
        uint64_t amount=available<remaining ? available : remaining;
        parts[part_count++]=(PoolCommitPart){i,b->start+b->committed,amount};
        remaining-=amount;
    }
    if (remaining) { write_unlock(); return NO_MEMORY; }

    uint64_t mapped=0;
    int32_t result=0;
    for (size_t i=0;i<part_count;++i) {
        void *out=(void *)(start+mapped);
        result=place(&out,parts[i].size,prot,MAP_FIXED_FLAG,pool_alignment,
                     KIND_POOL_COMMITTED,type,parts[i].physical);
        if (result) break;
        mapped+=parts[i].size;
    }
    if (result) {
        uint64_t rolled_back=0;
        for (size_t i=0;i<part_count && rolled_back<mapped;++i) {
            const uint64_t amount=parts[i].size<mapped-rolled_back ? parts[i].size : mapped-rolled_back;
            const uintptr_t at=start+rolled_back;
            if (unmap_locked(at,amount)) { fputs("STOP: failed to roll back partial memory-pool commit\n",stderr); exit(21); }
            void *reserved=(void *)at;
            if (place(&reserved,amount,0,MAP_FIXED_FLAG,pool_alignment,KIND_POOL_RESERVED,0,0)) {
                fputs("STOP: failed to restore memory-pool reservation after commit failure\n",stderr); exit(21);
            }
            rolled_back+=amount;
        }
        write_unlock();
        flush_hooks();
        return result;
    }
    for (size_t i=0;i<part_count;++i) {
        pool_backing[parts[i].backing].committed+=parts[i].size;
        ++maps;
    }
    write_unlock();
    flush_hooks();
    printf("Runtime: committed memory-pool range %p, size=%" PRIu64 " bytes, parts=%zu\n",
           (void *)start,size,part_count);
    return 0;
}
static int32_t protect_locked(uintptr_t start, uint64_t size, int prot, int type) {
    uintptr_t end=start+align_up(size,PAGE);
    start&=~(uintptr_t)(PAGE-1);
    if ((prot & ~0x37) || !covered(start,end,0)) return INVALID;
#ifdef _WIN32
    if (host_protect(start,end-start,host_prot(prot))) return INVALID;
#else
    if (mprotect((void *)start,end-start,host_prot(prot))) return INVALID;
#endif
    int error; size_t i=carve(start,end,&error);
    if (error) return NO_MEMORY;
    for (; i<vma_count && vmas[i].start<end; ++i) { vmas[i].prot=prot; if (type>=0) vmas[i].type=type; }
    queue_hook(HOOK_INVALIDATE,start,end-start);
    ++protects;
    return 0;
}
static ABI int32_t kernel_mprotect(const void *address, uint64_t size, int prot) {
    write_lock();
    int32_t result=protect_locked((uintptr_t)address,size,prot,-1);
    write_unlock();
    flush_hooks();
    return result;
}
static ABI int32_t query_protection(void *address, void **start, void **end, uint32_t *prot) {
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || vmas[i].start>(uintptr_t)address || kind_is_reserved(vmas[i].kind)) {
        read_unlock(); return ACCESS;
    }
    /* Report the whole contiguous run with identical protection. */
    size_t first=i, last=i;
    while (first && vmas[first-1].end==vmas[first].start && !kind_is_reserved(vmas[first-1].kind) && vmas[first-1].prot==vmas[i].prot) --first;
    while (last+1<vma_count && vmas[last+1].start==vmas[last].end && !kind_is_reserved(vmas[last+1].kind) && vmas[last+1].prot==vmas[i].prot) ++last;
    if (start) *start=(void *)vmas[first].start;
    if (end) *end=(void *)vmas[last].end;
    if (prot) *prot=(uint32_t)vmas[i].prot;
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t virtual_query(const void *address, int flags, VirtualQueryInfo *info, uint64_t info_size) {
    if (!info || info_size!=sizeof(*info)) return INVALID;
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || (vmas[i].start>(uintptr_t)address && !(flags & 1))) { read_unlock(); return ACCESS; }
    Vma v=vmas[i];
    memset(info,0,sizeof(*info));
    info->start=v.start; info->end=v.end;
    info->offset=(v.kind==KIND_DIRECT || v.kind==KIND_POOL_COMMITTED) ? v.phys : 0;
    info->protection=v.prot; info->memory_type=v.type;
    info->flags=(v.kind==KIND_FLEXIBLE ? 1u : 0) |
                 (v.kind==KIND_DIRECT ? 2u : 0) |
                 (v.kind==KIND_POOL_COMMITTED ? 8u|16u : !kind_is_reserved(v.kind) ? 16u : 0);
    strcpy(info->name, v.kind==KIND_POOL_RESERVED ? "pool_reserved" :
                       v.kind==KIND_POOL_COMMITTED ? "pool_committed" :
                       kind_is_reserved(v.kind) ? "reserved" :
                       v.kind==KIND_DIRECT ? "direct" : "flexible");
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t direct_memory_type(uint64_t phys, int *type, void **start, void **end) {
    read_lock();
    for (int i=0;i<LIMIT;++i) if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) {
        if (type) *type=blocks[i].type;
        if (start) *start=(void *)(uintptr_t)blocks[i].start;
        if (end) *end=(void *)(uintptr_t)(blocks[i].start+blocks[i].size);
        read_unlock(); return 0;
    }
    read_unlock();
    return ACCESS;
}
static ABI int32_t batch_map2(BatchEntry *entries, int count, int *processed, int flags) {
    if (!entries || count<0) return INVALID;
    int32_t result=0; int done=0;
    write_lock();
    for (; done<count; ++done) {
        BatchEntry *e=&entries[done];
        switch (e->operation) {
        case 0: result=map_direct_locked(&e->start,e->length,(uint8_t)e->prot,flags,(int64_t)e->offset,0); break;
        case 1: result=unmap_locked((uintptr_t)e->start,e->length); break;
        case 2: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,-1); break;
        case 3: result=map_flexible_locked(&e->start,e->length,(uint8_t)e->prot,flags); break;
        case 4: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,e->type); break;
        default: result=INVALID;
        }
        if (result) break;
    }
    write_unlock();
    flush_hooks();
    if (processed) *processed=done;
    return result;
}
static ABI int32_t batch_map(BatchEntry *entries, int count, int *processed) {
    return batch_map2(entries,count,processed,MAP_FIXED_FLAG);
}
static const RuntimeExport exports[]={
    {"sceKernelGetDirectMemorySize",direct_size}, {"sceKernelAllocateDirectMemory",direct_allocate},
    {"sceKernelMapDirectMemory",direct_map}, {"sceKernelMapNamedDirectMemory",direct_map_named},
    {"sceKernelReleaseDirectMemory",direct_release}, {"sceKernelMunmap",direct_unmap}, {"munmap",direct_unmap},
    {"sceKernelMapFlexibleMemory",map_flexible}, {"sceKernelMapNamedFlexibleMemory",map_flexible_named},
    {"sceKernelReleaseFlexibleMemory",release_flexible}, {"sceKernelReserveVirtualRange",reserve_range},
    {"sceKernelMprotect",kernel_mprotect}, {"sceKernelQueryMemoryProtection",query_protection},
    {"sceKernelVirtualQuery",virtual_query}, {"sceKernelGetDirectMemoryType",direct_memory_type},
    {"sceKernelBatchMap",batch_map}, {"sceKernelBatchMap2",batch_map2},
    {"mmap",posix_mmap},
};
/* HLE contract mirrored from shadPS4's sceLibcHeapGetTraceInfo implementation.
 * libc.prx uses this to query optional heap trace state during initialization. */
typedef struct {
    uint64_t size;
    uint32_t flag;
    uint32_t get_segment_info;
    uint64_t *mspace_atomic_id_mask;
    uint64_t *mstate_table;
} GuestHeapTraceInfo;
_Static_assert(sizeof(GuestHeapTraceInfo)==32,"PS4 heap trace info layout");
static uint64_t heap_mspace_atomic_id_mask;
static uint64_t heap_mstate_table[64];
static ABI void heap_get_trace_info(GuestHeapTraceInfo *info) {
    info->mspace_atomic_id_mask=&heap_mspace_atomic_id_mask;
    info->mstate_table=heap_mstate_table;
    info->get_segment_info=0;
}
uintptr_t runtime_memory_resolve(const char *name) {
    if (!strcmp(name,"NWtTN10cJzE#libSceLibcInternalExt")) return (uintptr_t)heap_get_trace_info;
    if (!strcmp(name,"mL8NDH86iQI#G#E")) return (uintptr_t)map_flexible_named;
    /* Direct-memory NIDs are exercised by test_runtime.c before names are loaded. */
    if (!strcmp(name, "pO96TwzOm5E#p#J")) return (uintptr_t)direct_size;
    if (!strcmp(name, "rTXw65xmLIA#p#J")) return (uintptr_t)direct_allocate;
    if (!strcmp(name, "qCSfqDILlns#G#E")) return (uintptr_t)memory_pool_expand;
    if (!strcmp(name, "pU-QydtGcGY#G#E")) return (uintptr_t)memory_pool_reserve;
    if (!strcmp(name, "Vzl66WmfLvk#G#E")) return (uintptr_t)memory_pool_commit;
    if (!strcmp(name, "L-Q3LEjIbgA#p#J")) return (uintptr_t)direct_map;
    if (!strcmp(name, "MBuItvba6z8#p#J")) return (uintptr_t)direct_release;
    if (!strcmp(name, "cQke9UuBQOk#p#J")) return (uintptr_t)direct_unmap;
    return RUNTIME_LOOKUP(exports,name);
}
/* Host-owned memory the guest can see (image, stacks, trampolines) lives in
 * [LOW_MIN, USER_MIN): PS4 code packs pointers into 40-bit fields. */
#define LOW_MIN UINT64_C(0x0800000000)
static uintptr_t low_next=LOW_MIN;
void *runtime_low_map(size_t size, int prot) {
    size=align_up(size,PAGE);
    write_lock();
#ifdef _WIN32
    void *p=NULL;
    while (!p && low_next+size<=USER_MIN) {
        p=VirtualAlloc((void *)low_next,size,MEM_RESERVE|MEM_COMMIT,win_prot(prot));
        low_next+=size+UINT64_C(65536); /* unmapped gap catches overruns; allocation granularity */
    }
    write_unlock();
    return p;
#else
    void *p=MAP_FAILED;
    while (low_next+size<=USER_MIN) {
        p=mmap((void *)low_next,size,prot,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        low_next+=size+PAGE; /* unmapped gap catches overruns */
        if (p!=MAP_FAILED) break;
    }
    write_unlock();
    return p==MAP_FAILED ? NULL : p;
#endif
}
/* ---- GPU library interface (gpu/shim/bbgpu.cpp) ---- */
/* Optimizations switched off at run time (diagnostics): the number in the file named by
 * BB_TOGGLE_FILE, re-read every 250 ms. Bits: 1 region cache, 2 fetch shader cache,
 * 4 page tracking early exit, 8 pending-op poll limit, 16 threaded Vulkan recording,
 * 32 texture descriptor cache, 64 lock-free upload check, 128 image lookup cache,
 * 256 buffer uploads on the recording thread, 512 barrier tracker insert memo,
 * 1024 texture binding memo, 2048 page-granular read tracking for barriers,
 * 4096 resource sharps read by the draw-preparation workers,
 * 8192 prepared draws from the draw-preparation workers,
 * 16384 small read-only buffer copies on the recording thread,
 * 32768 hot pages (opt-in with BB_HOT_PAGES=1),
 * 65536 unprotect the 256 KiB window around a guest write fault (BB_FAULT_WINDOW KiB),
 * 131072 large guest memory copies split across copy threads (BB_COPY_THREADS),
 * 262144 with BB_ASYNC_FENCES=1: wait for guest copies at fences again,
 * 524288 small guest copies batched for the copy threads instead of the recording thread,
 * 1073741824 the lock-free UpdateImage path for clean, tracked images. */
/* Bits 32 and up: the draw pipeline and related GPU thread work (gpu/shim/bbport_toggles.h). */
uint64_t runtime_disabled_optimizations;
/* Read the live toggle mask through a function instead of importing writable data across PE DLLs. */
uint64_t runtime_get_disabled_optimizations(void) {
    return __atomic_load_n(&runtime_disabled_optimizations, __ATOMIC_ACQUIRE);
}
/* bbport: a second number in BB_TOGGLE_FILE: temporary experiment bits (BbToggle::Experiment). */
uint64_t runtime_experiment_bits;
/* Speculative readers of guest memory (GPU draw-preparation workers) register a recovery
 * point: a fault on that thread jumps back to it instead of terminating (probe.c). */
#ifndef _WIN32
__thread sigjmp_buf *runtime_fault_recover;
#endif
static void *toggle_watcher(void *path) {
    for (unsigned long long last=ULLONG_MAX;;) {
        FILE *f=fopen(path,"r");
        unsigned long long value=0, experiment=0;
        if (f) { if (fscanf(f,"%llu %llu",&value,&experiment)<1) value=0; fclose(f); }
        __atomic_store_n(&runtime_experiment_bits,(uint64_t)experiment,__ATOMIC_RELEASE);
        if (value!=last) {
            __atomic_store_n(&runtime_disabled_optimizations,(uint64_t)value,__ATOMIC_RELEASE);
            printf("Runtime: disabled optimizations mask=%llu\n",value);
            last=value;
        }
        struct timespec t={0,250000000}; nanosleep(&t,NULL);
    }
    return NULL;
}
/* bbport: the GPU library provides direct memory chunks (see CHUNK). */
void runtime_memory_set_guest_chunk_whole(int whole) { whole_chunks=whole; }
void runtime_memory_set_guest_chunk_allocator(int (*alloc)(uint64_t phys, uint64_t size)) {
    write_lock();
    chunk_alloc=alloc;
    write_unlock();
}
void runtime_memory_set_gpu_hooks(GpuRange map, GpuRange unmap, GpuRange invalidate) {
    const char *toggles=getenv("BB_TOGGLE_FILE");
    static pthread_t watcher;
    if (toggles && !watcher) pthread_create(&watcher,NULL,toggle_watcher,(void *)toggles);
    write_lock();
    hook_map=map; hook_unmap=unmap; hook_invalidate=invalidate;
    for (size_t i=0;i<vma_count;++i) if (!kind_is_reserved(vmas[i].kind)) queue_hook(HOOK_MAP,vmas[i].start,vmas[i].end-vmas[i].start);
    write_unlock();
    flush_hooks();
}
/* bbport: the CPU is about to write [address, address+size) without the guest's code doing it
 * (a file read into it): the GPU side is told first (Rasterizer::InvalidateMemory), for the parts
 * the GPU can access, as a driver is told of a resource update. Its write tracking then has
 * nothing to catch there. */
void runtime_memory_set_note_write_hook(GpuRange note) { hook_note_write=note; }
void runtime_memory_note_write(uintptr_t address, uint64_t size) {
    if (!hook_invalidate || !size) return;
    uintptr_t starts[8], ends[8];
    int n=0;
    read_lock();
    for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size && n<8; ++i) {
        if (kind_is_reserved(vmas[i].kind) || !(vmas[i].prot & 0x30)) continue;
        starts[n]=vmas[i].start>address ? vmas[i].start : address;
        ends[n]=vmas[i].end<address+size ? vmas[i].end : address+size;
        ++n;
    }
    read_unlock();
    GpuRange hook=hook_note_write ? hook_note_write : hook_invalidate;
    for (int i=0;i<n;++i) hook(starts[i],ends[i]-starts[i]);
}
/* bbport BB_GUEST_IN_PLACE without page protection: pages whose CPU writes the GPU side must hear
 * of (a VRAM copy of them, a texture made from them), one bit per 4 KiB page below 2^40. The GPU
 * side sets and clears them where it used to protect; the writers it cannot see coming (libc
 * imports, file reads, hooked game code) check them after writing and tell it, as a driver is
 * told of a resource update. */
#define WATCH_LIMIT (1ull<<40)
static _Atomic(uint64_t) *write_watch;
static GpuRange hook_cpu_write;
void runtime_memory_set_cpu_write_hook(GpuRange hook) {
    if (!write_watch) {
        void *bits=mmap(NULL,WATCH_LIMIT>>15,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,-1,0);
        if (bits==MAP_FAILED) { perror("bbport: write watch bitmap"); return; }
        write_watch=bits;
    }
    hook_cpu_write=hook;
}
void runtime_memory_set_write_watch(uintptr_t address, uint64_t size, int watch) {
    if (!write_watch || !size || address>=WATCH_LIMIT) return;
    uint64_t first=address>>12, last=(address+size-1)>>12;
    if (last>=(WATCH_LIMIT>>12)) last=(WATCH_LIMIT>>12)-1;
    for (uint64_t word=first>>6; word<=last>>6; ++word) {
        const uint64_t lo=word==first>>6 ? first&63 : 0, hi=word==last>>6 ? last&63 : 63;
        const uint64_t mask=(hi==63 ? ~0ull : (2ull<<hi)-1) & ~((1ull<<lo)-1);
        if (watch) atomic_fetch_or_explicit(&write_watch[word],mask,memory_order_release);
        else atomic_fetch_and_explicit(&write_watch[word],~mask,memory_order_release);
    }
}
void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size) {
    if (!write_watch || !size || address>=WATCH_LIMIT || address+size>WATCH_LIMIT) return;
    const uint64_t first=address>>12, last=(address+size-1)>>12;
    for (uint64_t word=first>>6; word<=last>>6; ++word) {
        const uint64_t lo=word==first>>6 ? first&63 : 0, hi=word==last>>6 ? last&63 : 63;
        const uint64_t mask=(hi==63 ? ~0ull : (2ull<<hi)-1) & ~((1ull<<lo)-1);
        if (atomic_load_explicit(&write_watch[word],memory_order_acquire) & mask) {
            hook_cpu_write(address,size);
            return;
        }
    }
}
/* bbport: where direct memory at a guest address lives (BB_GUEST_IN_PLACE: the GPU uses the game's
 * memory where it is): its offset in direct memory and the end of the mapping, which is contiguous
 * there. 0 for anything else (flexible memory, reserved or unmapped addresses). */
int runtime_memory_direct_phys(uintptr_t address, uint64_t *phys, uintptr_t *end) {
    read_lock();
    const size_t i=vma_index(address);
    const int ok = i<vma_count && vmas[i].start<=address && address<vmas[i].end &&
                   (vmas[i].kind==KIND_DIRECT || vmas[i].kind==KIND_POOL_COMMITTED);
    if (ok) {
        *phys=vmas[i].phys+(address-vmas[i].start);
        *end=vmas[i].end;
    }
    read_unlock();
    return ok;
}
/* bbport: protection and direct memory type of the mapping at address (type -1: not direct
 * memory); 0 when nothing is mapped there. */
int runtime_memory_vma_info(uintptr_t address, int *prot, int *type, uintptr_t *end) {
    read_lock();
    const size_t i=vma_index(address);
    const int ok = i<vma_count && vmas[i].start<=address && !kind_is_reserved(vmas[i].kind);
    if (ok) {
        *prot=vmas[i].prot;
        *type=(vmas[i].kind==KIND_DIRECT || vmas[i].kind==KIND_POOL_COMMITTED) ? vmas[i].type : -1;
        *end=vmas[i].end;
    }
    read_unlock();
    return ok;
}
/* Writes through the backing view; 0 when part of the range has no backing. */
/* Guest-visible writes from the GPU side (fence labels, WriteData, downloads): every byte is
 * stored once. glibc's memcpy copies 4-7 bytes as two overlapping 4-byte stores (the same
 * 4 bytes twice for a 4-byte label): a thread preempted between them stored the label again
 * after the guest had seen it and freed the block that held it (a free-list link ending in
 * 0x00000004: guest fault 0x263b8e7). Aligned 2, 4 and 8-byte pieces are single stores. */
static void store_once(unsigned char *dst, const unsigned char *src, uint64_t n) {
    if (n > 64) { memcpy(dst,src,n); return; }
    while (n) {
        const uintptr_t at=(uintptr_t)dst;
        if (n>=8 && !(at&7)) { uint64_t v; memcpy(&v,src,8); __atomic_store_n((uint64_t *)dst,v,__ATOMIC_RELEASE); dst+=8; src+=8; n-=8; }
        else if (n>=4 && !(at&3)) { uint32_t v; memcpy(&v,src,4); __atomic_store_n((uint32_t *)dst,v,__ATOMIC_RELEASE); dst+=4; src+=4; n-=4; }
        else if (n>=2 && !(at&1)) { uint16_t v; memcpy(&v,src,2); __atomic_store_n((uint16_t *)dst,v,__ATOMIC_RELEASE); dst+=2; src+=2; n-=2; }
        else { __atomic_store_n(dst,*src,__ATOMIC_RELEASE); ++dst; ++src; --n; }
    }
}
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size) {
    read_lock();
    int ok=1;
    for (uintptr_t at=address, end=address+size; at<end && ok;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || kind_is_reserved(vmas[i].kind)) { ok=0; break; }
        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;
        store_once(backing_base+vmas[i].phys+(at-vmas[i].start),(const unsigned char *)data+(at-address),n);
        at+=n;
    }
    read_unlock();
    return ok;
}
/* Reads through the backing view: the host's copies of guest memory (uploads), which the GPU's
 * read protection (BB_READBACKS=2: GPU-written pages) must not stop. A fault there would ask the
 * GPU thread for a readback while it waits for these very copies (a hang at start). Gaps read
 * as zeros. */
void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size) {
    unsigned char *out=data;
    read_lock();
    for (uintptr_t at=address, end=address+size; at<end;) {
        const size_t i=vma_index(at);
        uint64_t n;
        if (i<vma_count && vmas[i].start<=at && !kind_is_reserved(vmas[i].kind)) {
            n=(vmas[i].end<end ? vmas[i].end : end)-at;
            memcpy(out+(at-address),backing_base+vmas[i].phys+(at-vmas[i].start),n);
        } else {
            const uintptr_t next=i==vma_count ? end : vmas[i].start>at ? vmas[i].start : vmas[i].end;
            n=(next<end ? next : end)-at;
            memset(out+(at-address),0,n);
        }
        at+=n;
    }
    read_unlock();
}
/* Per-thread cache of recently found regions for the GPU's per-draw queries:
 * entries hold for as long as the table generation they were read at. */
typedef struct { uint64_t generation; uintptr_t start, end; int kind; } CachedRegion;
static __thread CachedRegion region_cache[8];
static __thread unsigned region_cache_next;
static const CachedRegion *cached_region(uintptr_t address) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (generation & 1) return NULL;
    for (unsigned i=0;i<8;++i) {
        const CachedRegion *c=&region_cache[i];
        if (c->generation==generation && c->end && c->start<=address && address<c->end) return c;
    }
    return NULL;
}
/* Called under the read lock after a lookup of address. */
static void cache_region(size_t i) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (i==vma_count || (generation & 1)) return;
    region_cache[region_cache_next++ % 8]=(CachedRegion){generation,vmas[i].start,vmas[i].end,vmas[i].kind};
}
/* The mapping table's generation (odd while it changes): callers cache regions against it. */
const uint64_t *runtime_memory_generation(void) { return &table_generation; }
/* Mapped length from address, up to size, across adjacent mappings. */
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && !kind_is_reserved(c->kind) && size<=c->end-address) return size;
    read_lock();
    { size_t i=vma_index(address); if (i<vma_count && vmas[i].start<=address) cache_region(i); }
    uint64_t length=0;
    for (size_t i=vma_index(address); i<vma_count && length<size; ++i) {
        if (vmas[i].start>address+length || kind_is_reserved(vmas[i].kind)) break;
        length=vmas[i].end-address;
    }
    read_unlock();
    return length<size ? length : size;
}
/* Region around address: [start,end) mapped or a gap; 0 past the last mapping. */
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c) { *start=c->start; *end=c->end; *mapped=!kind_is_reserved(c->kind); return 1; }
    read_lock();
    size_t i=vma_index(address);
    int found=i<vma_count;
    if (found && vmas[i].start<=address) {
        cache_region(i);
        *start=vmas[i].start; *end=vmas[i].end; *mapped=!kind_is_reserved(vmas[i].kind);
    } else if (found) {
        *start=i ? vmas[i-1].end : 0; *end=vmas[i].start; *mapped=0;
    }
    read_unlock();
    return found;
}
/* GPU page tracking: restrict host access, never beyond the guest's own protection. */
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write) {
    read_lock();
    for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size; ++i) {
        if (kind_is_reserved(vmas[i].kind)) continue;
        uintptr_t a=vmas[i].start>address ? vmas[i].start : address;
        uintptr_t b=vmas[i].end<address+size ? vmas[i].end : address+size;
        int guest=host_prot(vmas[i].prot);
        int want=(read ? PROT_READ : 0)|(write ? PROT_WRITE|PROT_READ : 0)|(guest & PROT_EXEC);
#ifdef _WIN32
        host_protect(a,b-a,guest & want);
#else
        mprotect((void *)a,b-a,guest & want);
#endif
    }
    read_unlock();
}
/* Host view of a guest range, for the GPU backend: the range must be mapped. */
int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && !kind_is_reserved(c->kind) && size<=c->end-address) return 1;
    read_lock();
    int result=covered(address,address+size,0);
    read_unlock();
    return result;
}
void runtime_memory_report(void) {
    printf("Runtime: direct memory allocations=%zu, maps=%zu, live=%" PRIu64 ", budget=%" PRIu64 "\n",
           allocations, maps, live_bytes, POOL_SIZE);
    printf("Runtime: flexible maps=%zu, in use=%" PRIu64 ", protects=%zu, queries=%zu, regions=%zu\n",
           flexible_maps, flexible_bytes, protects, queries, vma_count);
}
