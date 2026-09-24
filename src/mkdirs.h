#ifndef MP_MKDIRS_H
#define MP_MKDIRS_H
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
static int mp_mkdirs(const char* path){
    char tmp[800]; snprintf(tmp,sizeof(tmp),"%s",path);
    size_t len=strlen(tmp);
    if(len==0) return -1;
    for(size_t i=1;i<len;i++){
        if(tmp[i]=='/'){ tmp[i]=0; mkdir(tmp,0777); tmp[i]='/'; }
    }
    mkdir(tmp,0777);
    return 0;
}
static void mp_ensure_files(const char * dir){
    mp_mkdirs(dir);
    char mp[900]; snprintf(mp,sizeof(mp),"%s/music_packs",dir);
    mp_mkdirs(mp);
    char cfg[900]; snprintf(cfg,sizeof(cfg),"%s/config.json",dir);
    FILE* f=fopen(cfg,"rb");
    if(f){ fclose(f); return; }
    f=fopen(cfg,"wb");
    if(!f) return;
    fclose(f);
}
#endif
