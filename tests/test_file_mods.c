/* Exercise actual guest file operations on a linked overlay and writable saves. */
#include "../src/runtime_file.c"
#include <assert.h>
static int32_t guest_errno;
static void put(const char *path,const char *text) {
    FILE *f=fopen(path,"w"); assert(f); assert(fputs(text,f)>=0); assert(!fclose(f));
}
static int listed(const char *guest,const char *name) {
    int dir=(int)do_open(guest,0x20000,0); assert(dir>=3);
    char entries[1024]; int64_t count=do_getdents(dir,entries,sizeof(entries),NULL); assert(count>0);
    int found=0;
    for (int64_t p=0;p<count;) {
        uint16_t length; memcpy(&length,entries+p+4,2);
        assert(length);
        if (!strcmp(entries+p+8,name)) found=1;
        p+=length;
    }
    assert(!do_close(dir));
    return found;
}
static void read_back(const char *guest,const char *text) {
    int fd=(int)do_open(guest,0,0); assert(fd>=3);
    char content[16]={0}; assert(do_read(fd,content,sizeof(content)-1)==(int64_t)strlen(text) && !strcmp(content,text));
    assert(!do_close(fd));
}
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
/* Reads into guest memory tell the GPU side (runtime_memory.c); nothing to tell here. */
void runtime_memory_note_write(uintptr_t address, uint64_t size) { (void)address; (void)size; }
/* PGG_SAVE_TRACE lives in runtime_savedata.c; off here. */
int runtime_save_trace(void) { return 0; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t n,const char *name) {
    for (size_t i=0;i<n;++i) if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
int main(void) {
    char root[]="/tmp/bbport-mod-files-XXXXXX";
    assert(mkdtemp(root));
    char game[512],user[512],source[512],link[512];
    snprintf(game,sizeof(game),"%s/game",root);
    snprintf(user,sizeof(user),"%s/user",root);
    snprintf(source,sizeof(source),"%s/mod.dcx",root);
    snprintf(link,sizeof(link),"%s/game/asset.dcx",root);
    assert(!mkdir(game,0755));
    FILE *f=fopen(source,"w"); assert(f); assert(fputs("modded",f)>=0); assert(!fclose(f));
    assert(!symlink(source,link));
    /* Patch overlay: <game>-patch next to the game folder; data only. */
    char patch[600],p[1024];
    snprintf(patch,sizeof(patch),"%s-patch",game);
    assert(!mkdir(patch,0755));
    snprintf(p,sizeof(p),"%s/data",game); assert(!mkdir(p,0755));
    snprintf(p,sizeof(p),"%s/data/base.dat",game); put(p,"base");
    snprintf(p,sizeof(p),"%s/data/both.dat",game); put(p,"old");
    snprintf(p,sizeof(p),"%s/eboot.bin",game); put(p,"eboot100");
    snprintf(p,sizeof(p),"%s/data",patch); assert(!mkdir(p,0755));
    snprintf(p,sizeof(p),"%s/data/both.dat",patch); put(p,"new");
    snprintf(p,sizeof(p),"%s/data/extra",patch); assert(!mkdir(p,0755));
    snprintf(p,sizeof(p),"%s/data/extra/101.dat",patch); put(p,"patch101");
    snprintf(p,sizeof(p),"%s/eboot.bin",patch); put(p,"eboot105");
    runtime_file_configure(game,user);
    assert(patch_count==4); /* data, data/both.dat, data/extra, data/extra/101.dat */
    read_back("/app0/data/base.dat","base");
    read_back("/app0/data/both.dat","new");
    read_back("/app0//data/extra/101.dat","patch101");
    read_back("data/extra/101.dat","patch101");
    read_back("/app0/eboot.bin","eboot100");
    assert(listed("/app0/data","base.dat") && listed("/app0/data","both.dat") && listed("/app0/data","extra"));
    assert(listed("/app0/data/extra","101.dat") && listed("/app0","data") && listed("/app0","eboot.bin"));
    assert(do_open("/app0/data/both.dat",2,0)==-EROFS);
    for (int i=0;i<3;++i) {
        const char *path=i==0 ? "/app0/asset.dcx" : i==1 ? "/hostapp/asset.dcx" : "asset.dcx";
        int fd=(int)do_open(path,0,0); assert(fd>=3);
        char content[8]={0}; assert(do_read(fd,content,6)==6 && !strcmp(content,"modded"));
        GuestStat info; assert(!do_stat(path,&info) && info.size==6);
        assert(!do_close(fd));
        assert(do_open(path,2,0)==-EROFS);
        assert(do_open(path,0x400,0)==-EROFS);
        assert(do_truncate(path,0)==-EROFS);
        assert(path_op(path,2,0)==-EROFS);
        assert(do_rename(path,"/data/moved")==-EROFS);
    }
    int dir=(int)do_open("/app0",0x20000,0); assert(dir>=3);
    char entries[1024]; int64_t count=do_getdents(dir,entries,sizeof(entries),NULL); assert(count>0);
    int found=0;
    for (int64_t p=0;p<count;) {
        uint16_t length; memcpy(&length,entries+p+4,2);
        assert(length);
        if (!strcmp(entries+p+8,"asset.dcx")) { assert(entries[p+6]==8); found=1; }
        p+=length;
    }
    assert(found && !do_close(dir));
    int save=(int)do_open("/data/test-save",0x202,0644); assert(save>=3);
    assert(do_write(save,"save",4)==4 && !do_close(save));
    assert(!path_op("/data/test-save",2,0));
    const char *patch_files[]={"data/extra/101.dat","data/both.dat","eboot.bin"};
    for (int i=0;i<3;++i) { snprintf(p,sizeof(p),"%s/%s",patch,patch_files[i]); assert(!unlink(p)); }
    snprintf(p,sizeof(p),"%s/data/extra",patch); assert(!rmdir(p));
    snprintf(p,sizeof(p),"%s/data",patch); assert(!rmdir(p)); assert(!rmdir(patch));
    const char *game_files[]={"data/base.dat","data/both.dat","eboot.bin"};
    for (int i=0;i<3;++i) { snprintf(p,sizeof(p),"%s/%s",game,game_files[i]); assert(!unlink(p)); }
    snprintf(p,sizeof(p),"%s/data",game); assert(!rmdir(p));
    assert(!unlink(link) && !unlink(source) && !rmdir(game));
    const char *dirs[]={"temp0","download0","data"};
    for (int i=0;i<3;++i) { char p[1024]; snprintf(p,sizeof(p),"%s/%s",user,dirs[i]); assert(!rmdir(p)); }
    assert(!rmdir(user) && !rmdir(root));
    puts("Guest mod files: reads, stat, merged listing, patch overlay, readonly assets and writable saves PASS");
}
