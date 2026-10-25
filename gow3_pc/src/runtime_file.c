/* Guest file system: PS4 mount points mapped onto host directories.
 *   /app0, /hostapp  -> game package root (read-only by convention)
 *   /temp0, /download0, /data, and mounts added by SaveData -> user directory
 * Guest descriptors are small integers in our own table; stdio 0-2 pass through.
 * Paths containing ".." components are rejected rather than normalized. */
#define _GNU_SOURCE
#include <time.h>
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/statvfs.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <io.h>
/* 64-bit sizes and times; directories are never opened as CRT descriptors. */
typedef struct _stat64 HostStat;
/* gow3 24 Oct 2026 (GitHub #43, #73, #139, #151, #153, #165 ...): the UCRT of Windows 11 24H2
 * and later stats a name without following it, so a file symbolic link reports its own size, 0.
 * The update view links the update's files with symbolic links where Windows allows them
 * (Developer Mode, a launcher run as administrator): the games saw bin.psarc as empty, mounted
 * nothing and stopped at files.json ("ASSERTION: root"). A file that is a reparse point is
 * stat'ed through an open descriptor, which follows the link. */
static int host_stat(const char *path,HostStat *s) {
    if (_stat64(path,s)) return -1;
    const DWORD attributes=GetFileAttributesA(path);
    if (attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT) &&
        !(attributes&FILE_ATTRIBUTE_DIRECTORY)) {
        const int fd=_open(path,_O_RDONLY|_O_BINARY);
        if (fd>=0) {
            HostStat target;
            if (!_fstat64(fd,&target)) *s=target;
            _close(fd);
        }
    }
    return 0;
}
#define host_fstat(fd,s) _fstat64(fd,s)
#define host_lseek _lseeki64
#define O_DIRECTORY 0x40000000 /* gow3 marker, removed before _open */
#else
typedef struct stat HostStat;
#define host_stat(path,s) stat(path,s)
#define host_fstat(fd,s) fstat(fd,s)
#define host_lseek lseek
#endif
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
typedef struct { int used, host, traced; Listing *dir; size_t position; char path[512]; } File;
typedef struct { char guest[64]; char host[512]; } Mount;
static File files[MAX_FILES];
static Mount mounts[MAX_MOUNTS];
static size_t mount_count, opens, reads, writes, missing;
static uint64_t bytes_read;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;

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
/* Update layers over /app0 and /hostapp (--app0-overlay, the first added wins). A game update
 * (1.02) holds only the files it changes: those are read from the update and everything else
 * from the game, as the PS4 mounts a patch over the disc. The probe uses this where the two
 * can't be combined as a folder of links (exFAT and FAT32 drives). Each layer is indexed once
 * at start, names without case, so a lookup costs no system call; the index never changes. */
#define MAX_OVERLAYS 4
typedef struct { char *key, *rel; int dir; } OverlayEntry;
typedef struct { char root[512]; OverlayEntry *entries; size_t count, capacity, files, dirs; } Overlay;
static Overlay overlays[MAX_OVERLAYS];
static size_t overlay_count;
static int overlay_mount(const char *guest) { return !strcmp(guest,"/app0") || !strcmp(guest,"/hostapp"); }
/* A mount-relative guest path as an index key: no leading, trailing or doubled slashes, no "."
 * components, lower case. Returns its length (0 for the mount's root) or -1 if too long. */
static int overlay_key(const char *rest,char *key,size_t size) {
    size_t n=0;
    while (*rest) {
        while (*rest=='/' || *rest=='\\') ++rest;
        if (!*rest) break;
        const char *end=rest;
        while (*end && *end!='/' && *end!='\\') ++end;
        if (end-rest==1 && rest[0]=='.') { rest=end; continue; }
        if (n+(size_t)(end-rest)+2>size) return -1;
        if (n) key[n++]='/';
        for (;rest<end;++rest) key[n++]=(char)(*rest>='A' && *rest<='Z' ? *rest+32 : *rest);
    }
    key[n]=0;
    return (int)n;
}
static int overlay_compare(const void *a,const void *b) { return strcmp(((const OverlayEntry *)a)->key,((const OverlayEntry *)b)->key); }
static const OverlayEntry *overlay_find(const Overlay *o,const char *key) {
    OverlayEntry probe={(char *)key,NULL,0};
    return bsearch(&probe,o->entries,o->count,sizeof(*o->entries),overlay_compare);
}
static void overlay_index(Overlay *o,const char *rel) {
    char path[1100];
    snprintf(path,sizeof(path),"%s%s%s",o->root,*rel ? "/" : "",rel);
    DIR *d=opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e=readdir(d))) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char child[1024], host[1100], key[1024];
        HostStat s;
        if ((size_t)snprintf(child,sizeof(child),"%s%s%s",rel,*rel ? "/" : "",e->d_name)>=sizeof(child)) continue;
        snprintf(host,sizeof(host),"%s/%s",o->root,child);
        if (host_stat(host,&s) || overlay_key(child,key,sizeof(key))<=0) continue;
        if (o->count==o->capacity) {
            o->capacity=o->capacity ? o->capacity*2 : 256;
            o->entries=realloc(o->entries,o->capacity*sizeof(*o->entries));
            if (!o->entries) { fputs("Out of memory indexing an /app0 overlay\n",stderr); exit(1); }
        }
        int dir=S_ISDIR(s.st_mode);
        o->entries[o->count++]=(OverlayEntry){strdup(key),strdup(child),dir};
        if (dir) { ++o->dirs; overlay_index(o,child); } else ++o->files;
    }
    closedir(d);
}
int runtime_file_add_overlay(const char *root) {
    if (overlay_count==MAX_OVERLAYS || strlen(root)>=sizeof(overlays[0].root)) return -1;
    Overlay *o=&overlays[overlay_count];
    snprintf(o->root,sizeof(o->root),"%s",root);
    /* Trailing separators go, except a drive root's ("E:" alone is that drive's current folder). */
    for (size_t n=strlen(o->root);n>1 && !(n==3 && o->root[1]==':') && (o->root[n-1]=='/' || o->root[n-1]=='\\');) o->root[--n]=0;
    overlay_index(o,"");
    if (!o->count) { printf("Runtime: /app0 overlay %s is empty or unreadable; not used\n",root); return -1; }
    qsort(o->entries,o->count,sizeof(*o->entries),overlay_compare);
    printf("Runtime: /app0 overlay %s: %zu files, %zu folders over the game\n",o->root,o->files,o->dirs);
    ++overlay_count;
    return 0;
}
/* Resolve a guest path to a host path (through the /app0 overlays unless base_only);
 * returns 0 or a host errno. */
static int resolve(const char *guest,char *out,size_t size,int base_only) {
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
    char key[1024];
    const Overlay *layer=NULL; const OverlayEntry *hit=NULL;
    if (m && overlay_count && !base_only && overlay_mount(m->guest) && overlay_key(buffer+best,key,sizeof(key))>0)
        for (size_t i=0;i<overlay_count && !hit;++i) if ((hit=overlay_find(&overlays[i],key))) layer=&overlays[i];
    if (!m) result=ENOENT;
    else if (hit) { if ((size_t)snprintf(out,size,"%s/%s",layer->root,hit->rel)>=size) result=ENAMETOOLONG; }
    else if ((size_t)snprintf(out,size,"%s%s",m->host,buffer+best)>=size) result=ENAMETOOLONG;
    pthread_mutex_unlock(&lock);
    if (result==ENOENT) fprintf(stderr,"Runtime: no mount for guest path %s\n",guest);
    return result;
}
static int translate(const char *guest,char *out,size_t size) { return resolve(guest,out,size,0); }
int32_t runtime_file_available_space_kb(const char *guest,uint64_t *kb) {
    if (!guest || !kb) return ERR(22);
    char host[1024];
    int error=translate(guest,host,sizeof(host));
    if (error) return ERR(error);
#ifdef _WIN32
    ULARGE_INTEGER available;
    if (!GetDiskFreeSpaceExA(host,&available,NULL,NULL)) return ERR(compat_errno_from_win32(GetLastError()));
    *kb=available.QuadPart/1024;
#else
    struct statvfs info;
    if (statvfs(host,&info)) return ERR(errno);
    *kb=(uint64_t)info.f_bavail*info.f_frsize/1024;
#endif
    return 0;
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
#ifdef _WIN32
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
#ifdef _WIN32
        /* MinGW's dirent has no d_type. */
        char child[1100]; HostStat entry;
        snprintf(child,sizeof(child),"%s/%s",path,e->d_name);
        l->types[l->count]=host_stat(child,&entry) ? 0 : S_ISDIR(entry.st_mode) ? 4 : S_ISREG(entry.st_mode) ? 8 : 0;
#else
        unsigned char type=e->d_type;
        if (type==DT_LNK || type==DT_UNKNOWN) {
            struct stat entry;
            if (!fstatat(dirfd(d),e->d_name,&entry,0))
                type=S_ISDIR(entry.st_mode) ? DT_DIR : S_ISREG(entry.st_mode) ? DT_REG : type;
        }
        l->types[l->count]=type==DT_DIR ? 4 : type==DT_REG ? 8 : type==DT_LNK ? 10 : 0;
#endif
        ++l->count; bytes+=n;
    }
    closedir(d);
    return l;
}
/* A folder of /app0 that is in the game and in an overlay lists the entries of both (names
 * without case, the chosen layer's first). */
static void merge_listing(Listing *l,const char *path) {
    Listing *other=list_directory(path);
    if (!other) return;
    size_t bytes=l->count ? l->offsets[l->count-1]+strlen(l->names+l->offsets[l->count-1])+1 : 0;
    for (size_t i=0;i<other->count;++i) {
        const char *name=other->names+other->offsets[i];
        size_t j=0;
        while (j<l->count && strcasecmp(l->names+l->offsets[j],name)) ++j;
        if (j<l->count) continue;
        size_t n=strlen(name)+1;
        l->offsets=realloc(l->offsets,(l->count+1)*sizeof(size_t));
        l->types=realloc(l->types,l->count+1);
        l->names=realloc(l->names,bytes+n);
        if (!l->offsets || !l->types || !l->names) { fputs("Out of memory listing directory\n",stderr); exit(1); }
        memcpy(l->names+bytes,name,n);
        l->offsets[l->count]=bytes; l->types[l->count]=other->types[i];
        ++l->count; bytes+=n;
    }
    free_listing(other);
}
static void merge_layers(const char *guest,const char *chosen,Listing *l) {
    char base[1024], key[1024], layer[1100];
    if (!overlay_count || resolve(guest,base,sizeof(base),1)) return;
    if (strcmp(base,chosen)) merge_listing(l,base);
    const char *rest=guest; /* relative guest paths are under /app0 */
    if (guest[0]=='/') {
        if (!strncmp(guest,"/app0",5) && (!guest[5] || guest[5]=='/')) rest=guest+5;
        else if (!strncmp(guest,"/hostapp",8) && (!guest[8] || guest[8]=='/')) rest=guest+8;
        else return;
    }
    int n=overlay_key(rest,key,sizeof(key));
    if (n<0) return;
    for (size_t i=0;i<overlay_count;++i) {
        const OverlayEntry *e=n ? overlay_find(&overlays[i],key) : NULL;
        if (n && (!e || !e->dir)) continue;
        if (n) snprintf(layer,sizeof(layer),"%s/%s",overlays[i].root,e->rel);
        else snprintf(layer,sizeof(layer),"%s",overlays[i].root);
        if (strcmp(layer,chosen)) merge_listing(l,layer);
    }
}
static void convert_stat(const HostStat *s,GuestStat *g) {
    memset(g,0,sizeof(*g));
    g->dev=(uint32_t)s->st_dev; g->ino=(uint32_t)s->st_ino;
    g->nlink=(uint16_t)s->st_nlink;
    g->size=s->st_size;
#ifdef _WIN32
    /* FreeBSD mode bits: directories rwxr-xr-x, files rw-r--r--. */
    g->mode=(uint16_t)(S_ISDIR(s->st_mode) ? 0040755 : 0100644);
    g->blocks=(s->st_size+511)/512; g->blksize=4096;
    g->atime=(GuestTimespec){s->st_atime,0};
    g->mtime=(GuestTimespec){s->st_mtime,0};
    g->ctime=(GuestTimespec){s->st_ctime,0};
#else
    g->mode=(uint16_t)s->st_mode;
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
/* GOW3_AUDIO_TRACE=1: sound file opens and failed reads (missing game sounds). */
static int audio_trace(void) { static int v=-1; if (v<0) { const char *e=getenv("GOW3_AUDIO_TRACE"); v=e && e[0]=='1'; } return v; }
/* GOW3_FILE_TRACE=<text>: opens, reads and seeks of files whose guest path contains <text>
 * (at most 200000 lines, with monotonic time), to follow streaming I/O. */
static const char *file_trace_match(void) {
    static const char *v; static int checked;
    if (!checked) { v=getenv("GOW3_FILE_TRACE"); checked=1; }
    return v && *v ? v : NULL;
}
static int file_trace_line(void) {
    static unsigned lines;
    if (__atomic_fetch_add(&lines,1,__ATOMIC_RELAXED)>=200000) return 0;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    printf("[%ld.%03ld] ",(long)t.tv_sec,t.tv_nsec/1000000);
    return 1;
}
static int file_traced(int fd) { File *f=(fd>=3 && fd<MAX_FILES) ? &files[fd] : NULL; return f && f->used && f->traced; }
/* Game mounts (including linked mod overlays) are read-only. Saves use other mounts. */
static int game_path(const char *p) {
    if (!p || !*p) return 0;
    if (*p!='/') return 1;
    return (!strncmp(p,"/app0",5) && (!p[5] || p[5]=='/')) ||
           (!strncmp(p,"/hostapp",8) && (!p[8] || p[8]=='/'));
}
/* All operations return >=0 or -(host errno); wrappers adapt the convention. */
static int64_t do_open(const char *guest,int flags,int mode) {
    if (game_path(guest) && (flags & (3|0x8|0x200|0x400|0x800))) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) { printf("Runtime: open(%s) failed: path translation errno %d\n",guest,e); return -e; }
#ifdef _WIN32
    /* Windows cannot open a directory as a CRT descriptor: directories are listings only. */
    (void)mode;
    HostStat s;
    Listing *dir=NULL;
    int host=-1;
    if (!host_stat(path,&s) && S_ISDIR(s.st_mode)) {
        if (flags&3) return -EISDIR;
        if (!(dir=list_directory(path))) return -EACCES;
        merge_layers(guest,path,dir);
    } else if (flags&0x20000) {
        e=errno==ENOENT ? ENOENT : ENOTDIR;
        if (e==ENOENT) { ++missing; printf("Runtime: open(%s) -> not found\n",guest); }
        return -e;
    } else {
        host=open(path,host_flags(flags)&~O_DIRECTORY,_S_IREAD|_S_IWRITE);
        if (host<0) {
            e=errno;
            if (e==ENOENT) { ++missing; printf("Runtime: open(%s) -> not found\n",guest); }
            else printf("Runtime: open(%s, flags %#x) failed: errno %d\n",guest,flags,e);
            return -e;
        }
        if (host_fstat(host,&s)) memset(&s,0,sizeof(s));
    }
#else
    int host=open(path,host_flags(flags),mode ? mode : 0644);
    if (host<0) {
        e=errno;
        if (e==ENOENT) { ++missing; printf("Runtime: open(%s) -> not found\n",guest); }
        return -e;
    }
    struct stat s;
    Listing *dir=NULL;
    if (!fstat(host,&s) && S_ISDIR(s.st_mode) && (dir=list_directory(path))) merge_layers(guest,path,dir);
#endif
    pthread_mutex_lock(&lock);
    int fd=-1;
    for (int i=3;i<MAX_FILES;++i) if (!files[i].used) { fd=i; break; }
    if (fd<0) {
        pthread_mutex_unlock(&lock); if (host>=0) close(host); free_listing(dir);
        printf("Runtime: open(%s) failed: guest file table full\n",guest);
        return -EMFILE;
    }
    files[fd]=(File){.used=1,.host=host,.dir=dir,
                     .traced=file_trace_match() && strstr(guest,file_trace_match())};
    snprintf(files[fd].path,sizeof(files[fd].path),"%s",guest);
    ++opens;
    pthread_mutex_unlock(&lock);
    if (files[fd].traced && file_trace_line())
        printf("File trace: open(%s) -> fd %d, %lld bytes\n",guest,fd,(long long)s.st_size);
    if (audio_trace() && (strstr(guest,"sound/") || strstr(guest,"sound1/"))) printf("Audio trace: open(%s) -> fd %d, %lld bytes\n",guest,fd,(long long)s.st_size);
    const char *mod_trace=getenv("GOW3_MOD_TRACE"), *mod_root=getenv("GOW3_MODS_DIR");
    if (mod_trace && mod_trace[0]=='1' && mod_root) {
        char actual[PATH_MAX],root[PATH_MAX];
        static unsigned traced;
        if (realpath(path,actual) && realpath(mod_root,root)) {
            size_t n=strlen(root);
            if (!strncmp(actual,root,n) && (actual[n]=='/' || actual[n]=='\\') &&
                __atomic_fetch_add(&traced,1,__ATOMIC_RELAXED)<32)
                printf("Mods: open %s -> %s\n",guest,actual);
        }
    }
    return fd;
}
static int64_t do_close(int fd) {
    if (fd>=0 && fd<3) return 0;
    pthread_mutex_lock(&lock);
    File *f=get(fd);
    if (!f) { pthread_mutex_unlock(&lock); return -EBADF; }
    if (f->host>=0) close(f->host);
    free_listing(f->dir);
    *f=(File){0};
    pthread_mutex_unlock(&lock);
    return 0;
}
static int host_fd(int fd) {
    if (fd>=0 && fd<3) return fd;
    File *f=get(fd);
    return f ? f->host : -1;
}
#ifdef _WIN32
/* The CRT reads and writes at most INT_MAX bytes per call. */
static ssize_t host_read(int h,void *buffer,uint64_t size) { return read(h,buffer,(unsigned)(size>0x7ffff000 ? 0x7ffff000 : size)); }
static ssize_t host_write(int h,const void *buffer,uint64_t size) { return write(h,buffer,(unsigned)(size>0x7ffff000 ? 0x7ffff000 : size)); }
#else
#define host_read read
#define host_write write
#endif
/* Pages of the destination may be write-protected for GPU tracking: the kernel's copy then
 * fails with EFAULT instead of faulting to our handler. A user-mode write to each page first
 * goes through the handler, which unprotects it (and records the upcoming write). */
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
    touch_for_write(buffer,size);
    ssize_t n=host_read(h,buffer,size);
    if (file_traced(fd) && file_trace_line())
        printf("File trace: read(fd %d, %llu) -> %lld (errno %d)\n",fd,(unsigned long long)size,(long long)n,n<0 ? errno : 0);
    if (n<0) { if (audio_trace()) printf("Audio trace: read(fd %d, %llu) failed, errno %d\n",fd,(unsigned long long)size,errno); return -errno; }
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_pread(int fd,void *buffer,uint64_t size,int64_t offset) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    touch_for_write(buffer,size);
    ssize_t n=pread(h,buffer,size,offset);
    if (file_traced(fd) && file_trace_line())
        printf("File trace: pread(fd %d, %llu @%lld) -> %lld (errno %d)\n",fd,(unsigned long long)size,
               (long long)offset,(long long)n,n<0 ? errno : 0);
    if (n<0) { if (audio_trace()) printf("Audio trace: pread(fd %d, %llu @%lld) failed, errno %d\n",fd,(unsigned long long)size,(long long)offset,errno); return -errno; }
    __atomic_add_fetch(&reads,1,__ATOMIC_RELAXED); __atomic_add_fetch(&bytes_read,(uint64_t)n,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_write(int fd,const void *buffer,uint64_t size) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    ssize_t n=host_write(h,buffer,size);
    if (n<0) return -errno;
    __atomic_add_fetch(&writes,1,__ATOMIC_RELAXED);
    return n;
}
static int64_t do_pwrite(int fd,const void *buffer,uint64_t size,int64_t offset) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
    ssize_t n=pwrite(h,buffer,size,offset);
    return n<0 ? -errno : n;
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
    int64_t r=host_lseek(f->host,offset,whence);
    if (f->traced && file_trace_line())
        printf("File trace: lseek(fd %d, %lld, %d) -> %lld\n",fd,(long long)offset,whence,(long long)r);
    return r<0 ? -errno : r;
}
static int64_t do_fstat(int fd,GuestStat *out) {
    int h=host_fd(fd);
    HostStat s;
#ifdef _WIN32
    if (h<0) { /* directory: a listing only, stat the path it was opened with */
        File *f=get(fd); char path[1024];
        if (!f || !f->dir) return -EBADF;
        if (!out) return -EFAULT;
        if (translate(f->path,path,sizeof(path)) || host_stat(path,&s)) return -EBADF;
        convert_stat(&s,out); return 0;
    }
#else
    if (h<0) return -EBADF;
#endif
    if (!out) return -EFAULT;
    if (host_fstat(h,&s)) return -errno;
    convert_stat(&s,out); return 0;
}
static int64_t do_stat(const char *guest,GuestStat *out) {
    char path[1024]; HostStat s;
    int e=translate(guest,path,sizeof(path));
    /* GOW3_AUDIO_TRACE=1: stat of sound files too (the sound config loader may stat first). */
    int traced=audio_trace() && guest && (strstr(guest,"sound/") || strstr(guest,"sound1/"));
    if (e) { if (traced) printf("Audio trace: stat(%s) failed: path translation errno %d\n",guest,e); return -e; }
    if (!out) return -EFAULT;
    if (host_stat(path,&s)) { e=errno; if (traced) printf("Audio trace: stat(%s) failed, errno %d\n",guest,e); return -e; }
    if (traced) printf("Audio trace: stat(%s) -> %lld bytes\n",guest,(long long)s.st_size);
    convert_stat(&s,out); return 0;
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
    return r ? -errno : 0;
}
static int64_t do_rename(const char *from,const char *to) {
    if (game_path(from) || game_path(to)) return -EROFS;
    char a[1024],b[1024];
    int e=translate(from,a,sizeof(a));
    if (!e) e=translate(to,b,sizeof(b));
    if (e) return -e;
#ifdef _WIN32
    /* POSIX rename replaces an existing target; the CRT's fails. */
    return MoveFileExA(a,b,MOVEFILE_REPLACE_EXISTING) ? 0 : -compat_errno_from_win32(GetLastError());
#else
    return rename(a,b) ? -errno : 0;
#endif
}
static int64_t do_ftruncate(int fd,int64_t length) {
    int h=host_fd(fd);
    if (h<0) return -EBADF;
#ifdef _WIN32
    return _chsize_s(h,length) ? -errno : 0;
#else
    return ftruncate(h,length) ? -errno : 0;
#endif
}
static int64_t do_truncate(const char *guest,int64_t length) {
    if (game_path(guest)) return -EROFS;
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    if (e) return -e;
#ifdef _WIN32
    int h=open(path,O_RDWR|O_BINARY);
    if (h<0) return -errno;
    int r=_chsize_s(h,length) ? -errno : 0;
    close(h);
    return r;
#else
    return truncate(path,length) ? -errno : 0;
#endif
}
static int64_t do_fsync(int fd) { int h=host_fd(fd); if (h<0) return -EBADF; return fsync(h) ? -errno : 0; }
static int64_t do_access(const char *guest,int mode) {
    char path[1024];
    int e=translate(guest,path,sizeof(path));
    int traced=audio_trace() && guest && (strstr(guest,"sound/") || strstr(guest,"sound1/"));
    if (e) { if (traced) printf("Audio trace: access(%s) failed: path translation errno %d\n",guest,e); return -e; }
#ifdef _WIN32
    int r=access(path,mode&6) ? -errno : 0; /* X_OK is invalid for the CRT */
#else
    int r=access(path,mode&7) ? -errno : 0;
#endif
    if (traced) printf("Audio trace: access(%s, %d) -> %d\n",guest,mode,r);
    return r;
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

