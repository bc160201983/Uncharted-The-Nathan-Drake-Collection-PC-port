/* PS4 virtual memory: direct (physical) pool, flexible memory, reservations,
 * protection changes and queries. One sparse memfd backs the whole direct
 * pool, so any mapping of any physical range aliases the same storage.
 * Windows: a pagefile-backed section plays the memfd's part, and the PS4 user range is
 * reserved as one placeholder that views of the section replace (VirtualAlloc2/MapViewOfFile3,
 * as shadPS4 does); see the "host mappings" section below.
 * Guest addresses are placed below 1 TiB (PS4 user range): GPU descriptors
 * encode 40-bit addresses, so host-default 0x7f... addresses would not fit. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
/* sceKernelGetDirectMemorySize on retail PS4: 5056 MiB. GOW3_DMEM_MB raises it (the resolution
 * patches above 1080p need about 4 GiB more; run.sh sets it). */
static uint64_t pool_size_bytes(void) {
    static uint64_t size;
    if (!size) {
        const char *env = getenv("GOW3_DMEM_MB");
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
enum { KIND_RESERVED=1, KIND_DIRECT, KIND_FLEXIBLE };
typedef struct { uint64_t start, size; int type, used; } Block;
typedef struct { uintptr_t start, end; int kind, prot, type; uint64_t phys; char name[32]; } Vma;
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
 * runtime_memory_gpu_protect on the same thread. A thread holding it exclusively skips
 * relocking. Readers must NOT write guest memory while they hold it: winpthreads' rwlock (the
 * Windows build) lets a waiting writer go first, so a nested reader deadlocks (gow3 5 Nov 2026). */
static pthread_rwlock_t lock=PTHREAD_RWLOCK_INITIALIZER;
static __thread unsigned exclusive_depth;
/* Odd while a writer holds the lock; readers' region caches are valid for one even value. */
static uint64_t table_generation;
/* gow3 19 Oct 2026: time spent in guest memory maps, unmaps and releases, and waiting for the
 * region lock exclusively (U1: a second without frames before a cutscene while the game mapped
 * ~100 1 MiB blocks). map_time_report prints when they took 20 ms or more since the last line. */
static uint64_t mono_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart*1e9/(double)frequency.QuadPart);
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*UINT64_C(1000000000)+(uint64_t)now.tv_nsec;
#endif
}
static uint64_t mt_lock_wait_ns, mt_lock_waits, mt_maps, mt_map_ns, mt_map_host_ns, mt_map_hooks_ns,
    mt_unmaps, mt_unmap_ns, mt_releases, mt_release_ns, mt_last_report_ns,
    mt_carves, mt_carve_ns, mt_carve_span, mt_carve_runs;
static void map_time_report(void) {
    const uint64_t now=mono_ns(), last=__atomic_load_n(&mt_last_report_ns,__ATOMIC_RELAXED);
    if (now-last<UINT64_C(250000000)) return;
    const uint64_t total=__atomic_load_n(&mt_map_ns,__ATOMIC_RELAXED)+__atomic_load_n(&mt_unmap_ns,__ATOMIC_RELAXED)+
                         __atomic_load_n(&mt_release_ns,__ATOMIC_RELAXED);
    if (total<UINT64_C(20000000)) return;
    if (!__atomic_compare_exchange_n(&mt_last_report_ns,(uint64_t *)&last,now,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) return;
    printf("Runtime: memory map time: %" PRIu64 " maps %.1f ms (Windows mapping %.1f ms, GPU hooks %.1f ms), "
           "%" PRIu64 " unmaps %.1f ms, %" PRIu64 " releases %.1f ms; region lock waits %" PRIu64 " (%.1f ms); "
           "Windows carves %" PRIu64 " (%.1f ms, largest span %.1f MiB, %" PRIu64 " protection runs restored)\n",
           __atomic_exchange_n(&mt_maps,0,__ATOMIC_RELAXED),__atomic_exchange_n(&mt_map_ns,0,__ATOMIC_RELAXED)/1e6,
           __atomic_exchange_n(&mt_map_host_ns,0,__ATOMIC_RELAXED)/1e6,__atomic_exchange_n(&mt_map_hooks_ns,0,__ATOMIC_RELAXED)/1e6,
           __atomic_exchange_n(&mt_unmaps,0,__ATOMIC_RELAXED),__atomic_exchange_n(&mt_unmap_ns,0,__ATOMIC_RELAXED)/1e6,
           __atomic_exchange_n(&mt_releases,0,__ATOMIC_RELAXED),__atomic_exchange_n(&mt_release_ns,0,__ATOMIC_RELAXED)/1e6,
           __atomic_exchange_n(&mt_lock_waits,0,__ATOMIC_RELAXED),__atomic_exchange_n(&mt_lock_wait_ns,0,__ATOMIC_RELAXED)/1e6,
           __atomic_exchange_n(&mt_carves,0,__ATOMIC_RELAXED),__atomic_exchange_n(&mt_carve_ns,0,__ATOMIC_RELAXED)/1e6,
           __atomic_exchange_n(&mt_carve_span,0,__ATOMIC_RELAXED)/1048576.0,__atomic_exchange_n(&mt_carve_runs,0,__ATOMIC_RELAXED));
}
static void write_lock(void) {
    if (!exclusive_depth++) {
        if (pthread_rwlock_trywrlock(&lock)) {
            const uint64_t start=mono_ns();
            pthread_rwlock_wrlock(&lock);
            __atomic_add_fetch(&mt_lock_wait_ns,mono_ns()-start,__ATOMIC_RELAXED);
            __atomic_add_fetch(&mt_lock_waits,1,__ATOMIC_RELAXED);
        }
        __atomic_add_fetch(&table_generation,1,__ATOMIC_ACQ_REL);
    }
}
static void write_unlock(void) {
    if (!--exclusive_depth) { __atomic_add_fetch(&table_generation,1,__ATOMIC_RELEASE); pthread_rwlock_unlock(&lock); }
}
static void read_lock(void) { if (!exclusive_depth) pthread_rwlock_rdlock(&lock); }
static void read_unlock(void) { if (!exclusive_depth) pthread_rwlock_unlock(&lock); }
static Block blocks[LIMIT];
static Vma *vmas; static size_t vma_count, vma_capacity;
#ifndef _WIN32
static int pool_fd=-1;
#endif
static unsigned char *backing_base; /* second view of the pool: host writes bypass guest/GPU page protection */
#define FLEX_SPAN (UINT64_C(1024) * 1024 * 1024)
static uint64_t flex_bitmap[FLEX_SPAN/PAGE/64];
/* GPU hooks (gow3gpu): notified outside the lock, in order, after each operation. */
typedef void (*GpuRange)(uintptr_t address, uint64_t size);
static GpuRange hook_map, hook_unmap, hook_invalidate;
enum { HOOK_MAP, HOOK_UNMAP, HOOK_INVALIDATE };
typedef struct { int kind; uintptr_t address; uint64_t size; } PendingHook;
static PendingHook *pending; static size_t pending_count, pending_capacity;
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
 * [USER_MIN,USER_MAX) is covered by regions, each one placeholder or one view of the
 * section. Mapping a range carves it out as a single placeholder first: views overlapping
 * it are unmapped, placeholders coalesced and split, and the parts of views outside the
 * range mapped again with the page protections they had (GPU page tracking included). */
typedef struct { uintptr_t start, end; uint64_t phys; int mapped; } HostRegion;
typedef struct { uintptr_t start, end; DWORD protect; } ProtRun;
static HostRegion *regions; static size_t region_count, region_capacity;
static HANDLE section;
static DWORD win_prot(int prot) {
    if (prot & PROT_EXEC) return (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : (prot & PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    return (prot & PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
}
static void win_fail(const char *what, uintptr_t address, uint64_t size) {
    fprintf(stderr,"STOP: %s failed at %p (+0x%" PRIx64 "), error %lu\n",what,(void *)address,size,GetLastError());
    exit(21);
}
static size_t region_index(uintptr_t a) { /* first region with end > a */
    size_t lo=0, hi=region_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (regions[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
static void region_insert(size_t at, HostRegion r) {
    if (region_count==region_capacity) {
        size_t capacity=region_capacity ? region_capacity*2 : 256;
        HostRegion *next=realloc(regions,capacity*sizeof(*regions));
        if (!next) { fputs("Out of memory (host regions)\n",stderr); exit(1); }
        regions=next; region_capacity=capacity;
    }
    memmove(regions+at+1,regions+at,(region_count-at)*sizeof(*regions));
    regions[at]=r; ++region_count;
}
static void region_erase(size_t at, size_t n) {
    memmove(regions+at,regions+at+n,(region_count-at-n)*sizeof(*regions)); region_count-=n;
}
/* The guest range is reserved before anything else can be placed there (runtime_start). */
void runtime_memory_reserve(void) {
    if (region_count) return;
    if (!VirtualAlloc2(NULL,(void *)USER_MIN,USER_MAX-USER_MIN,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0))
        win_fail("reserving the PS4 address range (link with --disable-high-entropy-va)",USER_MIN,USER_MAX-USER_MIN);
    region_insert(0,(HostRegion){USER_MIN,USER_MAX,0,0});
}
static void view_map(const HostRegion *r, DWORD protect) {
    DWORD initial=protect==PAGE_NOACCESS ? PAGE_READWRITE : protect, old;
    if (!MapViewOfFile3(section,GetCurrentProcess(),(void *)r->start,r->phys,r->end-r->start,MEM_REPLACE_PLACEHOLDER,initial,NULL,0))
        win_fail("MapViewOfFile3",r->start,r->end-r->start);
    if (initial!=protect && !VirtualProtect((void *)r->start,r->end-r->start,protect,&old))
        win_fail("VirtualProtect",r->start,r->end-r->start);
}
static size_t save_protections(uintptr_t start, uintptr_t end, ProtRun **runs, size_t count, size_t *capacity) {
    for (uintptr_t at=start; at<end;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)at,&m,sizeof(m))) win_fail("VirtualQuery",at,0);
        uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;
        if (next>end) next=end;
        if (count==*capacity) { *capacity=*capacity ? *capacity*2 : 16; *runs=realloc(*runs,*capacity*sizeof(**runs)); }
        (*runs)[count++]=(ProtRun){at,next,m.Protect};
        at=next;
    }
    return count;
}
static uint64_t mono_ns(void);

/* gow3 5 Nov 2026 ("closes after playing a while"): a partial map or unmap of a view (carve_host)
 * unmaps the whole view and maps the untouched pages back. For that moment other threads (guest
 * threads, GPU copy threads, draw preparation) found pages missing that were never unmapped: a
 * fatal fault. A fault inside the window now waits for the carve to end and retries; a fault
 * raised during a carve whose handler only ran after it ended retries once when the page is
 * there again. Carves run under the region write lock: one at a time. */
static volatile uintptr_t carve_lo, carve_hi;
static volatile DWORD carve_thread;
static volatile LONG carve_active, carve_seq;
static volatile uint64_t carve_end_tsc; /* __rdtsc() when the last carve ended */
int runtime_memory_wait_carve(uintptr_t address) {
    static int on=-1;
    if (on<0) { const char *env=getenv("GOW3_CARVE_WAIT"); on=!(env && env[0]=='0'); }
    if (!on) return 0;
    if (InterlockedCompareExchange(&carve_active,0,0)) {
        if (address<carve_lo || address>=carve_hi || GetCurrentThreadId()==carve_thread) return 0;
        while (InterlockedCompareExchange(&carve_active,0,0)) YieldProcessor();
        return 1;
    }
    if (address<carve_lo || address>=carve_hi || GetCurrentThreadId()==carve_thread) return 0;
    /* Only right after a carve (about 1 ms): GPU write-tracking faults in that range otherwise
     * paid a VirtualQuery and a second fault each. */
    if (__rdtsc()-carve_end_tsc > UINT64_C(4000000)) return 0;
    static __thread LONG retried_seq=-1;
    static __thread uintptr_t retried_address;
    const LONG seq=InterlockedCompareExchange(&carve_seq,0,0);
    if (retried_seq==seq && retried_address==address) return 0; /* retried once already */
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery((void *)address,&m,sizeof(m)) || m.State!=MEM_COMMIT ||
        (m.Protect & (PAGE_NOACCESS|PAGE_GUARD))) return 0;
    retried_seq=seq; retried_address=address;
    return 1;
}
/* The saved protections of one side, GPU write tracking included, right after it is mapped
 * back (it comes back writable: a view can't be raised above the protection it was mapped with). */
static void restore_protections(const ProtRun *runs, size_t from, size_t to) {
    for (size_t i=from;i<to;++i) {
        DWORD old;
        if (runs[i].protect!=PAGE_READWRITE && !VirtualProtect((void *)runs[i].start,runs[i].end-runs[i].start,runs[i].protect,&old))
            win_fail("restoring page protection",runs[i].start,runs[i].end-runs[i].start);
    }
}

static size_t carve_host_timed(uintptr_t a, uintptr_t b);
/* Leaves [a,b) as exactly one unmapped placeholder; returns its region index. */
static size_t carve_host(uintptr_t a, uintptr_t b) {
    const uint64_t start=mono_ns();
    const size_t at=carve_host_timed(a,b);
    __atomic_add_fetch(&mt_carves,1,__ATOMIC_RELAXED);
    __atomic_add_fetch(&mt_carve_ns,mono_ns()-start,__ATOMIC_RELAXED);
    return at;
}
static size_t carve_host_timed(uintptr_t a, uintptr_t b) {
    size_t first=region_index(a), last=first;
    if (first==region_count || regions[first].start>a) win_fail("guest range outside the reservation",a,b-a);
    while (last<region_count && regions[last].start<b) ++last;
    if (regions[last-1].end<b) win_fail("guest range outside the reservation",a,b-a);
    uintptr_t lo=regions[first].start, hi=regions[last-1].end;
    if (hi-lo>__atomic_load_n(&mt_carve_span,__ATOMIC_RELAXED)) __atomic_store_n(&mt_carve_span,hi-lo,__ATOMIC_RELAXED);
    HostRegion left=regions[first], right=regions[last-1];
    int keep_left=left.mapped && lo<a, keep_right=right.mapped && b<hi;
    if (first==last-1 && !left.mapped && lo==a && hi==b) return first;
    ProtRun *runs=NULL; size_t nruns=0, capacity=0, left_runs=0;
    if (keep_left) left_runs=nruns=save_protections(lo,a,&runs,nruns,&capacity);
    if (keep_right) nruns=save_protections(b,hi,&runs,nruns,&capacity);
    const int window=keep_left || keep_right;
    if (window) {
        carve_lo=lo; carve_hi=hi; carve_thread=GetCurrentThreadId();
        InterlockedExchange(&carve_active,1);
    }
    for (size_t i=first;i<last;++i) if (regions[i].mapped) {
        if (!UnmapViewOfFile2(GetCurrentProcess(),(void *)regions[i].start,MEM_PRESERVE_PLACEHOLDER))
            win_fail("UnmapViewOfFile2",regions[i].start,regions[i].end-regions[i].start);
    }
    if (last-first>1 && !VirtualFree((void *)lo,hi-lo,MEM_RELEASE|MEM_COALESCE_PLACEHOLDERS))
        win_fail("coalescing placeholders",lo,hi-lo);
    region_erase(first,last-first);
    region_insert(first,(HostRegion){lo,hi,0,0});
    size_t at=first;
    if (lo<a) {
        if (!VirtualFree((void *)lo,a-lo,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder",lo,a-lo);
        regions[at].end=a;
        region_insert(at+1,(HostRegion){a,hi,0,0});
        if (keep_left) {
            regions[at].phys=left.phys; regions[at].mapped=1;
            view_map(&regions[at],PAGE_READWRITE);
            restore_protections(runs,0,left_runs);
        }
        ++at;
    }
    if (b<hi) {
        if (!VirtualFree((void *)a,b-a,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder",a,b-a);
        regions[at].end=b;
        region_insert(at+1,(HostRegion){b,hi,0,0});
        if (keep_right) {
            regions[at+1].phys=right.phys+(b-right.start); regions[at+1].mapped=1;
            view_map(&regions[at+1],PAGE_READWRITE);
            restore_protections(runs,left_runs,nruns);
        }
    }
    if (window) {
        InterlockedIncrement(&carve_seq);
        carve_end_tsc=__rdtsc();
        InterlockedExchange(&carve_active,0);
    }
    __atomic_add_fetch(&mt_carve_runs,nruns,__ATOMIC_RELAXED);
    free(runs);
    return at;
}
/* gow3 19 Oct 2026: a guest mapping is made of views of at most VIEW_CHUNK bytes, cut at
 * VIEW_CHUNK-aligned guest addresses. Windows only unmaps whole views: unmapping 1 MiB of a large
 * mapping (carve_host) unmapped all of it, mapped the rest again and restored every page's
 * protection, about 23 ms each time. Drake's Fortune unmaps ~100 1 MiB pieces before some
 * cutscenes: a 1-1.8 s freeze. Now it touches at most the two chunks at the ends of the range.
 * Mappings whose address or offset is not 64 KiB aligned stay one view (view offsets must keep
 * the alignment Windows asks for). GOW3_VIEW_CHUNK_MB=0 maps every range as one view, as before. */
#define VIEW_CHUNK (UINT64_C(2)<<20)
static int view_chunks(void) {
    static int on=-1;
    if (on<0) { const char *env=getenv("GOW3_VIEW_CHUNK_MB"); on=!(env && env[0]=='0'); }
    return on;
}
static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys) {
    size_t i=carve_host(address,address+size);
    if (reserved) return 0;
    const DWORD protect=win_prot(prot);
    const uintptr_t end=address+size;
    const int chunked=view_chunks() && !(address&0xffff) && !(phys&0xffff);
    for (uintptr_t at=address; at<end; ++i) {
        uintptr_t next=chunked ? (at/VIEW_CHUNK+1)*VIEW_CHUNK : end;
        if (next>end) next=end;
        if (next<end) { /* split the rest off: [at,next) stays at regions[i] */
            if (!VirtualFree((void *)at,next-at,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER))
                win_fail("splitting a placeholder",at,next-at);
            regions[i].end=next;
            region_insert(i+1,(HostRegion){next,end,0,0});
        }
        regions[i].phys=phys+(at-address); regions[i].mapped=1;
        view_map(&regions[i],protect);
        at=next;
    }
    return 0;
}
static int host_unmap(uintptr_t address, uint64_t size) { carve_host(address,address+size); return 0; }
static int host_protect(uintptr_t address, uint64_t size, int prot) {
    DWORD protect=win_prot(prot), old;
    uintptr_t end=address+size;
    for (size_t i=region_index(address); i<region_count && regions[i].start<end; ++i) {
        if (!regions[i].mapped) continue;
        uintptr_t a=regions[i].start>address ? regions[i].start : address, b=regions[i].end<end ? regions[i].end : end;
        if (!VirtualProtect((void *)a,b-a,protect,&old)) return -1;
    }
    return 0;
}
/* Released pages read as zero when reused, like the memfd's punched holes. */
static void host_discard(uint64_t phys, uint64_t size) { memset(backing_base+phys,0,size); }
/* gow3 18 Oct 2026: the section is only reserved; pages are committed when the game allocates
 * them (direct allocations, flexible maps), so Windows charges the commit limit for the ~4.8 GB a
 * game uses, not the whole 6+ GB pool at start. A full commit made 16 GB PCs with a small page
 * file fail at start (error 1450) whenever other programs were open. When the commit limit is
 * reached, Windows grows a system-managed page file a moment later: retry for up to 10 s.
 * Committed pages stay committed (a section's pages cannot be decommitted). */
static int host_commit(uint64_t phys, uint64_t size) {
    for (int attempt=0;;++attempt) {
        if (VirtualAlloc(backing_base+phys,size,MEM_COMMIT,PAGE_READWRITE)) {
            if (attempt) printf("Runtime: committed %" PRIu64 " MiB of guest memory after %d retries (page file grew)\n",size>>20,attempt);
            return 0;
        }
        DWORD error=GetLastError();
        if (attempt==40 || (error!=ERROR_COMMITMENT_LIMIT && error!=ERROR_NOT_ENOUGH_MEMORY && error!=ERROR_NO_SYSTEM_RESOURCES && error!=ERROR_COMMITMENT_MINIMUM)) {
            fprintf(stderr,"Runtime: cannot commit %" PRIu64 " MiB of guest memory (error %lu); is the page file large enough?\n",size>>20,error);
            return -1;
        }
        if (!attempt) printf("Runtime: commit limit reached (%" PRIu64 " MiB of guest memory, error %lu): waiting for the page file to grow\n",size>>20,error);
        /* Not while holding the region table: every GPU upload and tracked-page fault waits on
         * it. The caller's range is claimed already (block or flexible bits). */
        int held=exclusive_depth==1;
        if (held) write_unlock();
        Sleep(250);
        if (held) write_lock();
    }
}
/* Direct memory occupies [0,POOL_SIZE) of the section, flexible memory [POOL_SIZE,+FLEX_SPAN). */
static int pool(void) {
    if (backing_base) return 0;
    runtime_memory_reserve();
    uint64_t bytes=POOL_SIZE+FLEX_SPAN;
    section=CreateFileMapping2(INVALID_HANDLE_VALUE,NULL,FILE_MAP_ALL_ACCESS,PAGE_EXECUTE_READWRITE,SEC_RESERVE,bytes,NULL,NULL,0);
    if (!section) { fprintf(stderr,"Runtime: cannot create %" PRIu64 " MiB of guest memory (error %lu); is the page file large enough?\n",bytes>>20,GetLastError()); return -1; }
    backing_base=MapViewOfFile3(section,GetCurrentProcess(),NULL,0,bytes,0,PAGE_READWRITE,NULL,0);
    return backing_base ? 0 : -1;
}
#else
/* Direct memory occupies [0,POOL_SIZE) of the memfd, flexible memory [POOL_SIZE,+FLEX_SPAN). */
static int pool(void) {
    if (pool_fd>=0) return 0;
    pool_fd=memfd_create("gow3-guest-memory", MFD_CLOEXEC);
    if (pool_fd<0 || ftruncate(pool_fd,(off_t)(POOL_SIZE+FLEX_SPAN))) return -1;
    void *view=mmap(NULL,POOL_SIZE+FLEX_SPAN,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_NORESERVE,pool_fd,0);
    if (view==MAP_FAILED) return -1;
    backing_base=view;
    return 0;
}
void runtime_memory_reserve(void) {}
int runtime_memory_wait_carve(uintptr_t address) { (void)address; return 0; } /* mmap replaces in place */
static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys) {
    void *mapped = !reserved
        ? mmap((void *)address,size,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)phys)
        : mmap((void *)address,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED,-1,0);
    return mapped==MAP_FAILED ? -1 : 0;
}
static int host_unmap(uintptr_t address, uint64_t size) { return munmap((void *)address,size); }
static int host_protect(uintptr_t address, uint64_t size, int prot) { return mprotect((void *)address,size,prot); }
static void host_discard(uint64_t phys, uint64_t size) {
    fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)phys,(off_t)size);
}
static int host_commit(uint64_t phys, uint64_t size) { (void)phys; (void)size; return 0; }
#endif
static void queue_hook(int kind, uintptr_t address, uint64_t size) {
    GpuRange hook = kind==HOOK_MAP ? hook_map : kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
    if (!hook) return;
    if (pending_count==pending_capacity) {
        size_t capacity=pending_capacity ? pending_capacity*2 : 64;
        if (capacity<pending_capacity || capacity>SIZE_MAX/sizeof(*pending)) {
            fputs("STOP: GPU hook queue capacity overflow\n",stderr); exit(21);
        }
        PendingHook *next=realloc(pending,capacity*sizeof(*pending));
        if (!next) { fputs("STOP: GPU hook queue allocation failed\n",stderr); exit(21); }
        pending=next; pending_capacity=capacity;
    }
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
    host_discard(phys,size);
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
    /* Direct and flexible pieces both keep their physical offset (18 Oct 2026: flexible ones did
     * not, so after a partial mprotect/munmap of flexible memory the right piece's backing copies
     * read and wrote the left piece's pages and flex_free freed and zeroed the wrong ones). */
    if (right.kind!=KIND_RESERVED) right.phys+=a-vmas[i].start;
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
        if (i==vma_count || vmas[i].start>at || (!allow_reserved && vmas[i].kind==KIND_RESERVED)) return 0;
        at=vmas[i].end;
    }
    return 1;
}
static int overlaps(uintptr_t start, uintptr_t end, int ignore_reserved) {
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (!ignore_reserved || vmas[i].kind!=KIND_RESERVED) return 1;
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
    if (host_map(address,size,host_prot(prot),kind==KIND_RESERVED,phys)) return NO_MEMORY;
    void *mapped=(void *)address;
    drop_range(address,address+size);
    Vma region={address,address+size,kind,prot,type,phys,{0}};
    strcpy(region.name,kind==KIND_RESERVED ? "reserved" : kind==KIND_DIRECT ? "direct" : "flexible");
    if (vma_insert(vma_index(address),region)) return NO_MEMORY;
    if (kind==KIND_FLEXIBLE) flexible_bytes+=size;
    if (kind!=KIND_RESERVED) queue_hook(HOOK_MAP,address,size);
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
    /* Uncharted searches up to the retail pool size (5056 MiB); a raised pool (GOW3_DMEM_MB, the
     * render resolution patch) extends that search. */
    if ((uint64_t)high >= UINT64_C(5056) * 1024 * 1024 && (uint64_t)high < POOL_SIZE) high = (int64_t)POOL_SIZE;
    uint64_t end = (uint64_t)high < POOL_SIZE ? (uint64_t)high : POOL_SIZE;
    if ((uint64_t)low >= end || size > end - (uint64_t)low) {
        printf("Runtime: direct allocation of %" PRIu64 " bytes in [0x%" PRIx64 ", 0x%" PRIx64 ") failed: no space\n",
               size, (uint64_t)low, (uint64_t)high);
        return NO_SPACE;
    }
    write_lock();
    if (pool()) { write_unlock(); return NO_MEMORY; }
    uint64_t start = align_up((uint64_t)low, alignment);
    int slot = -1;
    for (int i=0; i<LIMIT; ++i) if (!blocks[i].used) { slot=i; break; }
    for (;;) {
        if (slot<0 || start > end || size > end-start) {
            write_unlock();
            printf("Runtime: direct allocation of %" PRIu64 " bytes in [0x%" PRIx64 ", 0x%" PRIx64 ") failed: no free range\n",
                   size, (uint64_t)low, (uint64_t)high);
            return NO_SPACE;
        }
        int overlap = 0;
        for (int i=0; i<LIMIT; ++i) {
            Block b = blocks[i];
            if (b.used && start < b.start+b.size && b.start < start+size) {
                start = align_up(b.start+b.size, alignment); overlap = 1; break;
            }
        }
        if (!overlap) break;
    }
    /* Claimed before the commit: host_commit may drop the lock while the page file grows. */
    blocks[slot] = (Block){start, size, type, 1};
    if (host_commit(start, size)) { blocks[slot].used = 0; write_unlock(); return NO_MEMORY; }
    *out = (int64_t)start;
    ++allocations; live_bytes += size;
    write_unlock();
    printf("Runtime: direct allocation offset=0x%" PRIx64 ", size=%" PRIu64 " bytes, type=%d\n", start, size, type);
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
    const uint64_t start=mono_ns();
    write_lock();
    const uint64_t locked=mono_ns();
    int32_t result=map_direct_locked(out,size,prot,flags,physical,alignment);
    const uint64_t mapped=mono_ns();
    write_unlock();
    flush_hooks();
    const uint64_t done=mono_ns();
    __atomic_add_fetch(&mt_maps,1,__ATOMIC_RELAXED);
    __atomic_add_fetch(&mt_map_ns,done-start,__ATOMIC_RELAXED);
    __atomic_add_fetch(&mt_map_host_ns,mapped-locked,__ATOMIC_RELAXED);
    __atomic_add_fetch(&mt_map_hooks_ns,done-mapped,__ATOMIC_RELAXED);
    if (!result) printf("Runtime: mapped direct memory %p, size=%" PRIu64 ", prot=0x%x\n", *out, size, prot);
    map_time_report();
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
        if (vmas[i].kind!=KIND_RESERVED) {
            uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
            queue_hook(HOOK_UNMAP,a,b-a);
        }
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i) {
        uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
        if (host_unmap(a,b-a)) return INVALID;
    }
    drop_range(start,end);
    return 0;
}
static ABI int32_t direct_unmap(void *address, uint64_t size) {
    const uint64_t start=mono_ns();
    /* The GPU forgets the range before it disappears (as shadPS4's early GPU unmap). */
    if (hook_unmap && !((uintptr_t)address%PAGE) && size) hook_unmap((uintptr_t)address,align_up(size,PAGE));
    write_lock();
    size_t before=pending_count;
    int32_t result=unmap_locked((uintptr_t)address,size);
    pending_count=before; /* already notified above */
    write_unlock();
    __atomic_add_fetch(&mt_unmaps,1,__ATOMIC_RELAXED);
    __atomic_add_fetch(&mt_unmap_ns,mono_ns()-start,__ATOMIC_RELAXED);
    map_time_report();
    return result;
}
static ABI int32_t direct_release(uint64_t start, uint64_t size) {
    if (start % PAGE || size % PAGE) return INVALID;
    if (!size) return 0;
    const uint64_t release_start=mono_ns();
    write_lock();
    if (!direct_range_allocated(start,size)) { write_unlock(); return INVALID; }
    /* Mappings of released physical pages disappear with them. */
    for (size_t i=0; i<vma_count;) {
        Vma v=vmas[i];
        uint64_t length=v.end-v.start;
        if (v.kind==KIND_DIRECT && v.phys<start+size && start<v.phys+length) {
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
    host_discard(start,size); /* zero on reuse */
    write_unlock();
    flush_hooks();
    __atomic_add_fetch(&mt_releases,1,__ATOMIC_RELAXED);
    __atomic_add_fetch(&mt_release_ns,mono_ns()-release_start,__ATOMIC_RELAXED);
    map_time_report();
    return 0;
}
static int32_t map_flexible_locked(void **inout, uint64_t size, int prot, int flags) {
    if (!inout || !size || size%PAGE || (prot & ~0x37)) return INVALID;
    if (flexible_bytes+size>FLEXIBLE_SIZE || pool()) return NO_MEMORY;
    uint64_t phys=flex_alloc(size);
    if (phys==UINT64_MAX) return NO_MEMORY;
    if (host_commit(phys,size)) { flex_set((phys-POOL_SIZE)/PAGE,size/PAGE,0); return NO_MEMORY; }
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
/* POSIX mmap as libc uses it: anonymous memory from the flexible pool. File mappings fail with
 * ENODEV (no guest file descriptor can be mapped). */
static ABI void *posix_mmap(void *address, uint64_t size, int prot, int flags, int fd, int64_t offset) {
    if (!(flags & 0x1000) || fd!=-1 || offset) { *runtime_errno()=19; return (void *)-1; }
    void *out=address;
    int32_t r=map_flexible(&out,(size+PAGE-1)&~(PAGE-1),prot,flags & 0x10);
    if (r) { *runtime_errno()=12; return (void *)-1; }
    return out;
}
static ABI int32_t posix_msync(void *address, uint64_t size, int flags) { (void)address; (void)size; (void)flags; return 0; }
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
/* GOW3_FAULT_STATS=1: protection changes that reach the GPU (each one invalidates the range,
 * which downloads GPU-written data with a full GPU wait), every 5 s by guest caller. */
static void note_protect(uintptr_t start, uint64_t size, int old_prot, int prot, uintptr_t caller) {
    static int enabled=-1;
    if (enabled<0) { const char *e=getenv("GOW3_FAULT_STATS"); enabled=e && e[0]=='1'; }
    if (!enabled) return;
    enum { SLOTS=16 };
    static struct { uintptr_t caller, start; uint64_t size; int old_prot, prot; unsigned count; } slots[SLOTS];
    static unsigned total, skipped_slots;
    static time_t last;
    ++total;
    int found=-1;
    for (int i=0;i<SLOTS;++i) {
        if (slots[i].count && slots[i].caller==caller && slots[i].old_prot==old_prot && slots[i].prot==prot) { found=i; break; }
        if (!slots[i].count && found<0) { found=i; slots[i].caller=caller; slots[i].start=start; slots[i].size=size;
            slots[i].old_prot=old_prot; slots[i].prot=prot; break; }
    }
    if (found>=0) ++slots[found].count; else ++skipped_slots;
    const time_t now=time(NULL);
    if (now-last<5) return;
    last=now;
    printf("Protect stats (5 s): %u protection changes reached the GPU (%u not itemized)\n",total,skipped_slots);
    for (int i=0;i<SLOTS;++i) if (slots[i].count)
        printf("  caller %#llx: %u x, e.g. %#llx size %#llx prot %#x -> %#x\n",(unsigned long long)slots[i].caller,
               slots[i].count,(unsigned long long)slots[i].start,(unsigned long long)slots[i].size,
               slots[i].old_prot,slots[i].prot);
    fflush(stdout);
    memset(slots,0,sizeof(slots)); total=0; skipped_slots=0;
}
static uintptr_t protect_caller;
static int32_t protect_locked(uintptr_t start, uint64_t size, int prot, int type) {
    uintptr_t end=start+align_up(size,PAGE);
    start&=~(uintptr_t)(PAGE-1);
    if ((prot & ~0x37) || !covered(start,end,0)) return INVALID;
    /* An unchanged protection is a no-op: the host pages keep their GPU-tracking protection and
     * the GPU caches stay valid. Uncharted re-protects ranges several times per frame; each
     * invalidation downloaded GPU-written data with a full GPU wait (temple: ~47% of frame time). */
    int unchanged=1, old_prot=-1;
    for (size_t j=vma_index(start); j<vma_count && vmas[j].start<end; ++j) {
        if (old_prot<0) old_prot=vmas[j].prot;
        if (vmas[j].prot!=prot || (type>=0 && vmas[j].type!=type)) { unchanged=0; break; }
    }
    if (unchanged) return 0;
    note_protect(start,end-start,old_prot,prot,protect_caller);
    if (host_protect(start,end-start,host_prot(prot))) return INVALID;
    int error; size_t i=carve(start,end,&error);
    if (error) return NO_MEMORY;
    for (; i<vma_count && vmas[i].start<end; ++i) { vmas[i].prot=prot; if (type>=0) vmas[i].type=type; }
    queue_hook(HOOK_INVALIDATE,start,end-start);
    ++protects;
    return 0;
}
static ABI int32_t kernel_mprotect(const void *address, uint64_t size, int prot) {
    write_lock();
    protect_caller=(uintptr_t)__builtin_return_address(0);
    int32_t result=protect_locked((uintptr_t)address,size,prot,-1);
    write_unlock();
    flush_hooks();
    return result;
}
static ABI int32_t query_protection(void *address, void **start, void **end, uint32_t *prot) {
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || vmas[i].start>(uintptr_t)address || vmas[i].kind==KIND_RESERVED) {
        read_unlock(); return ACCESS;
    }
    /* Report the whole contiguous run with identical protection. */
    size_t first=i, last=i;
    while (first && vmas[first-1].end==vmas[first].start && vmas[first-1].kind!=KIND_RESERVED && vmas[first-1].prot==vmas[i].prot) --first;
    while (last+1<vma_count && vmas[last+1].start==vmas[last].end && vmas[last+1].kind!=KIND_RESERVED && vmas[last+1].prot==vmas[i].prot) ++last;
    /* gow3 5 Nov 2026: the results go to guest memory only after the lock is released (see
     * virtual_query). */
    void *run_start=(void *)vmas[first].start, *run_end=(void *)vmas[last].end;
    const uint32_t run_prot=(uint32_t)vmas[i].prot;
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    if (start) *start=run_start;
    if (end) *end=run_end;
    if (prot) *prot=run_prot;
    return 0;
}
/* Diagnostic names belong to the VMAs, including after protection/range splits.
 * ABI/NIDs: shadPS4 src/core/libraries/kernel/memory.cpp. */
static ABI int32_t set_range_name(const void *address, uint64_t size, const char *name) {
    if (!name) return (int32_t)UINT32_C(0x8002000e); /* EFAULT */
    size_t length=0;
    while (length<32 && name[length]) ++length;
    if (length==32) return (int32_t)UINT32_C(0x8002003f); /* ENAMETOOLONG */
    uintptr_t start=(uintptr_t)address;
    if (!size || start%PAGE || size%PAGE || size>UINTPTR_MAX-start) return INVALID;
    write_lock();
    if (!covered(start,start+size,1)) { write_unlock(); return ACCESS; }
    int error;
    size_t i=carve(start,start+size,&error);
    if (error) { write_unlock(); return NO_MEMORY; }
    for (; i<vma_count && vmas[i].start<start+size; ++i) {
        memset(vmas[i].name,0,sizeof(vmas[i].name));
        memcpy(vmas[i].name,name,length);
    }
    write_unlock();
    return 0;
}
static ABI int32_t clear_range_name(const void *address, uint64_t size) {
    return set_range_name(address,size,"");
}
static ABI int32_t virtual_query(const void *address, int flags, VirtualQueryInfo *info, uint64_t info_size) {
    if (!info || info_size!=sizeof(*info)) return INVALID;
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || (vmas[i].start>(uintptr_t)address && !(flags & 1))) { read_unlock(); return ACCESS; }
    Vma v=vmas[i];
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    /* gow3 5 Nov 2026: filled after the lock is released. A store into a GPU-tracked guest page
     * faults, and the handler takes this lock again on the same thread (runtime_memory_gpu_protect):
     * winpthreads' rwlock lets a waiting writer go first, so the nested reader waited for the
     * writer and the writer for this thread: a silent freeze. */
    VirtualQueryInfo result;
    memset(&result,0,sizeof(result));
    result.start=v.start; result.end=v.end;
    result.offset=v.kind==KIND_DIRECT ? v.phys : 0;
    result.protection=v.prot; result.memory_type=v.type;
    result.flags=(v.kind==KIND_FLEXIBLE ? 1u : 0) | (v.kind==KIND_DIRECT ? 2u : 0) | (v.kind!=KIND_RESERVED ? 16u : 0);
    memcpy(result.name,v.name,sizeof(result.name));
    memcpy(info,&result,sizeof(result));
    return 0;
}
static ABI int32_t direct_memory_type(uint64_t phys, int *type, void **start, void **end) {
    read_lock();
    for (int i=0;i<LIMIT;++i) if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) {
        /* gow3 5 Nov 2026: written to guest memory after the lock is released (see virtual_query). */
        const int block_kind=blocks[i].type;
        void *block_start=(void *)(uintptr_t)blocks[i].start;
        void *block_end=(void *)(uintptr_t)(blocks[i].start+blocks[i].size);
        read_unlock();
        if (type) *type=block_kind;
        if (start) *start=block_start;
        if (end) *end=block_end;
        return 0;
    }
    read_unlock();
    return ACCESS;
}
static ABI int32_t batch_map2(BatchEntry *entries, int count, int *processed, int flags) {
    if (!entries || count<0) return INVALID;
    int32_t result=0; int done=0;
    write_lock();
    protect_caller=(uintptr_t)__builtin_return_address(0);
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
    {"mmap",posix_mmap}, {"msync",posix_msync},
    {"sceKernelGetDirectMemorySize",direct_size}, {"sceKernelAllocateDirectMemory",direct_allocate},
    {"sceKernelMapDirectMemory",direct_map}, {"sceKernelMapNamedDirectMemory",direct_map_named},
    {"sceKernelReleaseDirectMemory",direct_release}, {"sceKernelMunmap",direct_unmap}, {"munmap",direct_unmap},
    {"sceKernelMapFlexibleMemory",map_flexible}, {"sceKernelMapNamedFlexibleMemory",map_flexible_named},
    {"sceKernelReleaseFlexibleMemory",release_flexible}, {"sceKernelReserveVirtualRange",reserve_range},
    {"sceKernelMprotect",kernel_mprotect}, {"sceKernelQueryMemoryProtection",query_protection},
    {"sceKernelVirtualQuery",virtual_query}, {"sceKernelGetDirectMemoryType",direct_memory_type},
    {"sceKernelSetVirtualRangeName",set_range_name}, {"sceKernelClearVirtualRangeName",clear_range_name},
    {"sceKernelBatchMap",batch_map}, {"sceKernelBatchMap2",batch_map2},
};
uintptr_t runtime_memory_resolve(const char *name) {
    /* Direct-memory NIDs are exercised by test_runtime.c before names are loaded. */
    if (!strcmp(name, "pO96TwzOm5E#libkernel")) return (uintptr_t)direct_size;
    if (!strcmp(name, "rTXw65xmLIA#libkernel")) return (uintptr_t)direct_allocate;
    if (!strcmp(name, "L-Q3LEjIbgA#libkernel")) return (uintptr_t)direct_map;
    if (!strcmp(name, "MBuItvba6z8#libkernel")) return (uintptr_t)direct_release;
    if (!strcmp(name, "cQke9UuBQOk#libkernel")) return (uintptr_t)direct_unmap;
    if (!strcmp(name, "DGMG3JshrZU#libkernel")) return (uintptr_t)set_range_name;
    if (!strcmp(name, "mkgXxsoxWHg#libkernel")) return (uintptr_t)clear_range_name;
    return RUNTIME_LOOKUP(exports,name);
}
/* Guest memory for the GPU library's own data (movie frames scaled to the display size,
 * gpu/shim/gow3_movie.cpp): flexible memory, CPU and GPU read/write. Placed high: games claim
 * virtual ranges from 0x1000000000 up without reserving them (Uncharted's memory areas, whose
 * freed pages leave holes that look free here). */
void *runtime_map_guest_memory(uint64_t size) {
    void *address=(void *)UINT64_C(0xf000000000);
    return map_flexible(&address,align_up(size,PAGE),0x33,0)==0 ? address : NULL;
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
/* ---- GPU library interface (gpu/shim/gow3gpu.cpp) ---- */
/* Optimizations switched off at run time (diagnostics): the number in the file named by
 * GOW3_TOGGLE_FILE, re-read every 250 ms. Bits: 1 region cache, 2 fetch shader cache,
 * 4 page tracking early exit, 8 pending-op poll limit, 16 threaded Vulkan recording,
 * 32 texture descriptor cache, 64 lock-free upload check, 128 image lookup cache,
 * 256 buffer uploads on the recording thread, 512 barrier tracker insert memo,
 * 1024 texture binding memo, 2048 page-granular read tracking for barriers,
 * 4096 resource sharps read by the draw-preparation workers,
 * 8192 prepared draws from the draw-preparation workers,
 * 16384 small read-only buffer copies on the recording thread,
 * 32768 hot pages (opt-in with GOW3_HOT_PAGES=1),
 * 65536 unprotect the 256 KiB window around a guest write fault (GOW3_FAULT_WINDOW KiB),
 * 131072 large guest memory copies split across copy threads (GOW3_COPY_THREADS),
 * 262144 with GOW3_ASYNC_FENCES=1: wait for guest copies at fences again,
 * 524288 small guest copies batched for the copy threads instead of the recording thread,
 * 1073741824 the lock-free UpdateImage path for clean, tracked images. */
/* Bits 32 and up: the draw pipeline and related GPU thread work (gpu/shim/gow3_toggles.h). */
uint64_t runtime_disabled_optimizations;
/* Speculative readers of guest memory (GPU draw-preparation workers) register a recovery
 * point: a fault on that thread jumps back to it instead of terminating (probe.c). */
__thread sigjmp_buf *runtime_fault_recover;
static void *toggle_watcher(void *path) {
    for (unsigned long long last=ULLONG_MAX;;) {
        FILE *f=fopen(path,"r");
        unsigned long long value=0;
        if (f) { if (fscanf(f,"%llu",&value)!=1) value=0; fclose(f); }
        if (value!=last) {
            __atomic_store_n(&runtime_disabled_optimizations,(uint64_t)value,__ATOMIC_RELEASE);
            printf("Runtime: disabled optimizations mask=%llu\n",value);
            last=value;
        }
#ifdef _WIN32
        compat_sleep_ns(250000000);
#else
        struct timespec t={0,250000000}; nanosleep(&t,NULL);
#endif
    }
    return NULL;
}
void runtime_memory_set_gpu_hooks(GpuRange map, GpuRange unmap, GpuRange invalidate) {
    const char *toggles=getenv("GOW3_TOGGLE_FILE");
    static pthread_t watcher;
    if (toggles && !watcher) pthread_create(&watcher,NULL,toggle_watcher,(void *)toggles);
    write_lock();
    hook_map=map; hook_unmap=unmap; hook_invalidate=invalidate;
    for (size_t i=0;i<vma_count;++i) if (vmas[i].kind!=KIND_RESERVED) queue_hook(HOOK_MAP,vmas[i].start,vmas[i].end-vmas[i].start);
    write_unlock();
    flush_hooks();
}
/* The region cache (below): backing reads and writes inside one cached mapping take no lock. The
 * GPU thread copies through the backing view for every upload and every landed readback; the
 * read-write lock there cost Uncharted 2 ~15% of the GPU thread (winpthreads, 16 Oct). */
static const void *cached_backing(uintptr_t address, uint64_t size);
static void cache_region(size_t i);
/* Writes through the backing view; 0 when part of the range has no backing. */
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size) {
    { const void *fast=cached_backing(address,size); if (fast) { memcpy((void *)fast,data,size); return 1; } }
    read_lock();
    { size_t first=vma_index(address); if (first<vma_count && vmas[first].start<=address) cache_region(first); }
    int ok=1;
    for (uintptr_t at=address, end=address+size; at<end && ok;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) { ok=0; break; }
        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;
        memcpy(backing_base+vmas[i].phys+(at-vmas[i].start),(const unsigned char *)data+(at-address),n);
        at+=n;
    }
    read_unlock();
    return ok;
}
/* Reads through the backing view (no guest/GPU page protection applies); 0 when part of the
 * range has no backing. */
int runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size) {
    { const void *fast=cached_backing(address,size); if (fast) { memcpy(data,fast,size); return 1; } }
    read_lock();
    int ok=1;
    for (uintptr_t at=address, end=address+size; at<end && ok;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) { ok=0; break; }
        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;
        memcpy((unsigned char *)data+(at-address),backing_base+vmas[i].phys+(at-vmas[i].start),n);
        at+=n;
    }
    read_unlock();
    return ok;
}
/* GPU uploads (CopySparseMemory): copies guest memory through the backing view and zero-fills
 * unmapped parts. The GPU thread copies while it holds the buffer tracker's locks; through the
 * guest view, a page read-protected for an async readback (GOW3_ASYNC_READBACK=1) faulted into a
 * handler that needs the same locks (Uncharted 3 hung while loading, 16 Oct). An upload copies
 * CPU-written bytes only, so it never needs to wait for a readback's GPU bytes. */
void runtime_memory_copy_backing(uintptr_t address, void *dest, uint64_t size) {
    unsigned char *out=dest;
    { const void *fast=cached_backing(address,size); if (fast) { memcpy(dest,fast,size); return; } }
    read_lock();
    { size_t first=vma_index(address); if (first<vma_count && vmas[first].start<=address) cache_region(first); }
    for (uintptr_t at=address, end=address+size; at<end;) {
        size_t i=vma_index(at);
        uintptr_t next=end;
        if (i<vma_count && vmas[i].start<=at) {
            next=vmas[i].end<end ? vmas[i].end : end;
            if (vmas[i].kind==KIND_RESERVED) memset(out+(at-address),0,next-at);
            else if (backing_base) memcpy(out+(at-address),backing_base+vmas[i].phys+(at-vmas[i].start),next-at);
            else memcpy(out+(at-address),(const void *)at,next-at);
        } else {
            if (i<vma_count && vmas[i].start<end) next=vmas[i].start;
            memset(out+(at-address),0,next-at);
        }
        at=next;
    }
    read_unlock();
}
/* Per-thread cache of recently found regions for the GPU's per-draw queries:
 * entries hold for as long as the table generation they were read at. */
typedef struct { uint64_t generation; uintptr_t start, end; int kind; uint64_t phys; } CachedRegion;
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
/* The backing address of [address, address + size) when it lies in one cached, backed mapping. */
static const void *cached_backing(uintptr_t address, uint64_t size) {
    if (!backing_base || (runtime_disabled_optimizations & 1)) return NULL;
    const CachedRegion *c=cached_region(address);
    if (!c || c->kind==KIND_RESERVED || size>c->end-address) return NULL;
    return backing_base+c->phys+(address-c->start);
}
/* Called under the read lock after a lookup of address. */
static void cache_region(size_t i) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (i==vma_count || (generation & 1)) return;
    region_cache[region_cache_next++ % 8]=(CachedRegion){generation,vmas[i].start,vmas[i].end,vmas[i].kind,vmas[i].phys};
}
/* Mapped length from address, up to size, across adjacent mappings. */
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->kind!=KIND_RESERVED && size<=c->end-address) return size;
    read_lock();
    { size_t i=vma_index(address); if (i<vma_count && vmas[i].start<=address) cache_region(i); }
    uint64_t length=0;
    for (size_t i=vma_index(address); i<vma_count && length<size; ++i) {
        if (vmas[i].start>address+length || vmas[i].kind==KIND_RESERVED) break;
        length=vmas[i].end-address;
    }
    read_unlock();
    return length<size ? length : size;
}
/* Region around address: [start,end) mapped or a gap; 0 past the last mapping. */
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c) { *start=c->start; *end=c->end; *mapped=c->kind!=KIND_RESERVED; return 1; }
    read_lock();
    size_t i=vma_index(address);
    int found=i<vma_count;
    if (found && vmas[i].start<=address) {
        cache_region(i);
        *start=vmas[i].start; *end=vmas[i].end; *mapped=vmas[i].kind!=KIND_RESERVED;
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
        if (vmas[i].kind==KIND_RESERVED) continue;
        uintptr_t a=vmas[i].start>address ? vmas[i].start : address;
        uintptr_t b=vmas[i].end<address+size ? vmas[i].end : address+size;
        int guest=host_prot(vmas[i].prot);
        int want=(read ? PROT_READ : 0)|(write ? PROT_WRITE|PROT_READ : 0)|(guest & PROT_EXEC);
        host_protect(a,b-a,guest & want);
    }
    read_unlock();
}
/* Host view of a guest range, for the GPU backend: the range must be mapped. */
int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->kind!=KIND_RESERVED && size<=c->end-address) return 1;
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

