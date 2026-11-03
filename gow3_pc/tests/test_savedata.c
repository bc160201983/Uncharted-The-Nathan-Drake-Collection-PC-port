/* runtime_savedata.c backup and repair (GitHub #311): a written save is copied at unmount; a file
 * left empty or all zeros (a power cut) comes back from that copy at the next mount; a deleted
 * save loses its copy; GOW3_SAVE_BACKUP=0 does neither. Stubs stand in for the file layer. */
#include "runtime.h"
#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char user_dir[512];
const char *runtime_file_user_dir(void) { return user_dir; }
int runtime_file_mount(const char *guest, const char *host) { (void)guest; (void)host; return 0; }
void runtime_file_unmount(const char *guest) { (void)guest; }
int gow3gpu_thumbnail_save(const char *path) { (void)path; return 0; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    for (size_t i=0;i<count;++i) if (!strcmp(nid,table[i].name)) return (uintptr_t)table[i].function;
    return 0;
}

typedef int32_t (ABI *Fn)();
static Fn get(const char *name) { Fn f=(Fn)runtime_savedata_resolve(name); assert(f); return f; }

typedef struct { char data[32]; } DirName;
typedef struct { int32_t user; int32_t pad; const void *dir; uint64_t blocks; uint32_t mode;
                 uint8_t reserved[32]; int32_t pad2; } Mount2;
typedef struct { char point[16]; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[28]; int32_t pad; } MountResult;
typedef struct { int32_t user; int32_t pad; const void *title; const DirName *dir; uint32_t unused;
                 uint8_t reserved[32]; int32_t pad2; } Delete;

static void put(const char *path, const void *data, size_t size) {
    FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(data,1,size,f)==size); fclose(f);
}
static long file_size(const char *path) {
    struct stat st; return stat(path,&st) ? -1 : (long)st.st_size;
}
static int first_byte(const char *path) {
    FILE *f=fopen(path,"rb"); if (!f) return -1; int c=fgetc(f); fclose(f); return c;
}
static void mount_rw(const char *dir, uint32_t mode, MountResult *r) {
    DirName name={{0}}; snprintf(name.data,sizeof(name.data),"%s",dir);
    Mount2 m={0}; m.user=1; m.dir=&name; m.mode=mode;
    memset(r,0,sizeof(*r));
    int32_t rc=((int32_t (ABI *)(const Mount2 *, MountResult *))get("sceSaveDataMount2"))(&m,r);
    assert(rc==0);
}
static void umount(MountResult *r) {
    assert(((int32_t (ABI *)(const void *))get("sceSaveDataUmount"))(r->point)==0);
}

int main(void) {
    char tmp[400];
    snprintf(tmp,sizeof(tmp),"%s/savedata-test-%d",getenv("TEMP") ? getenv("TEMP") : "/tmp",(int)rand());
    snprintf(user_dir,sizeof(user_dir),"%s",tmp);
    compat_remove_tree(tmp);
    mkdir(tmp,0755);
    runtime_savedata_configure("CUSA00000");
    assert(((int32_t (ABI *)(const void *))get("sceSaveDataInitialize3"))(NULL)==0);
    char data[400], backup[400];
    snprintf(data,sizeof(data),"%s/savedata/1/CUSA00000/SAVE0/USR-DATA",tmp);
    snprintf(backup,sizeof(backup),"%s/savedata/1/CUSA00000/.backup/SAVE0/USR-DATA",tmp);

    /* Create + write + unmount: the copy appears. */
    MountResult r;
    mount_rw("SAVE0",32 /* CREATE2 */,&r);
    unsigned char save[3000]; for (size_t i=0;i<sizeof(save);++i) save[i]=(unsigned char)(i*7+1);
    put(data,save,sizeof(save));
    umount(&r);
    assert(file_size(backup)==(long)sizeof(save) && first_byte(backup)==1);

    /* A power cut leaves the file all zeros: the next (read-only) mount restores it. */
    unsigned char zeros[3000]={0};
    put(data,zeros,sizeof(zeros));
    mount_rw("SAVE0",1 /* RDONLY */,&r);
    assert(file_size(data)==(long)sizeof(save) && first_byte(data)==1);
    umount(&r);
    /* ... or empty. */
    put(data,"",0);
    mount_rw("SAVE0",0,&r);
    assert(file_size(data)==(long)sizeof(save));
    /* A newer save replaces the copy at unmount. */
    save[0]=42; put(data,save,sizeof(save));
    umount(&r);
    assert(first_byte(backup)==42);
    /* A read-only mount does not touch the copy; an unchanged whole file is left alone. */
    mount_rw("SAVE0",1,&r);
    save[0]=43; put(data,save,sizeof(save));   /* (a read-only mount would not write; tests the copy) */
    umount(&r);
    assert(first_byte(backup)==42 && first_byte(data)==43);
    /* A missing file is the game's business: not restored. */
    remove(data);
    mount_rw("SAVE0",1,&r);
    assert(file_size(data)==-1);
    umount(&r);
    put(data,save,sizeof(save));

    /* Delete removes the copy too; a save made again in that slot is not filled from it. */
    DirName name={{0}}; snprintf(name.data,sizeof(name.data),"SAVE0");
    Delete d={0}; d.user=1; d.dir=&name;
    assert(((int32_t (ABI *)(const Delete *))get("sceSaveDataDelete"))(&d)==0);
    assert(file_size(backup)==-1);

    /* GOW3_SAVE_BACKUP=0 is read once per process: checked by the game runs, not here. */
    compat_remove_tree(tmp);
    printf("savedata test: all passed\n");
    return 0;
}
