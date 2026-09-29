#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdarg.h>
#include "jni_helper.h"
#include "mkdirs.h"
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <strings.h>
#include "tefkernel-cpp-wrapper/tefkernel/module/module_core.h"
#include "tefkernel-cpp-wrapper/tefkernel/patchlib/type.h"
#include "tefkernel-cpp-wrapper/tefkernel/patchlib/method.h"
#include "tefkernel-cpp-wrapper/tefkernel/patchlib/field.h"
#include "tefkernel-cpp-wrapper/tefkernel/patchlib/property.h"
#include "tefkernel-cpp-wrapper/tefkernel/patchlib/thread.h"
#define LOG_TAG "MusicPackZ"
/* 日志开关：0=关闭全部日志，1=开启（logcat + private_dir/mpz.log） */
#define MP_LOG 1

#if MP_LOG
static char g_dir[512] = {0};
static void mpz_filelog(const char* fmt, ...){
    if(!g_dir[0]) return;
    char lp[768]; snprintf(lp,sizeof(lp),"%s/mpz.log",g_dir);

    const long LOG_MAX = 256*1024;
    struct stat st;
    long sz = (stat(lp,&st)==0) ? (long)st.st_size : 0;
    if(sz >= LOG_MAX){
        FILE* rf = fopen(lp,"rb");
        if(rf){
            fseek(rf,0,SEEK_END); long total=ftell(rf);
            long keep = LOG_MAX/2;
            long start = total - keep; if(start<0) start=0;
            fseek(rf,start,SEEK_SET);
            char* buf=(char*)malloc(keep+2);
            if(buf){
                long rd=(long)fread(buf,1,(size_t)keep,rf); buf[rd]=0;
                char* nl=strchr(buf,'\n');
                FILE* wf=fopen(lp,"wb");
                if(wf){ if(nl) fwrite(nl+1,1,strlen(nl+1),wf); fclose(wf); }
                free(buf);
            }
            fclose(rf);
        }
    }
    FILE* f = fopen(lp,"a");
    if(!f) return;
    time_t t=time(0); struct tm tmv; localtime_r(&t,&tmv);
    char ts[32]; strftime(ts,sizeof(ts),"%m-%d %H:%M:%S",&tmv);
    fprintf(f,"[%s] ",ts);
    va_list ap; va_start(ap,fmt);
    vfprintf(f,fmt,ap);
    va_end(ap);
    fputc('\n',f);
    fclose(f);
}
#define LOGI(...) do{ __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__); mpz_filelog(__VA_ARGS__); }while(0)
#define LOGE(...) do{ __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__); mpz_filelog("ERR "__VA_ARGS__); }while(0)
#else
#define LOGI(...) do{}while(0)
#define LOGE(...) do{}while(0)
#endif
static const module_info_t g_info = {
    .pkg_id = "eternal.future.audiopackextension", .name = "MusicPack Extension", .author = "qing",
    .version = "1.1.0", .version_code = 1100, .api_version = 1,
    .plugin_dependencies_sizes = 0, .plugin_dependencies = 0,
};
#define MAX_ITEMS 256
typedef struct { int enable; int music; int priority; int bad; char file[200]; char pack[200]; } item_t;
typedef struct { int enable; int type; char file[200]; char pack[200]; } sfx_item_t;
static sfx_item_t g_sfxitems[MAX_ITEMS];
static int g_sfxcount = 0;

static item_t g_items[MAX_ITEMS];
static int g_count = 0;
static char g_playing_file[300] = {0};
static volatile int g_playing_alive = 0;
static volatile int g_cover = 0;
static volatile long g_cover_ms = 0;
static int g_fading = 0;
static volatile int g_release = 0;
static volatile long g_release_ms = 0;
static volatile long g_leave_ms = 0;
static float g_game_vol = 0.0f;
static volatile int g_play_fail = 0;
static volatile int g_fail_music = -1;
static volatile int g_cur_music = -1;
static const char* skip_ws(const char* p){ while(*p==' '||*p=='\t'||*p=='\n'||*p=='\r') p++; return p; }
static const char* find_key(const char* start,const char* limit,const char* key){
    size_t kl=strlen(key); const char* p=start;
    while(p<limit){
        if(*p=='"'){
            if((size_t)(limit-(p+1))>kl && strncmp(p+1,key,kl)==0 && p[1+kl]=='"'){
                const char* q=skip_ws(p+1+kl+1);
                if(*q==':') return skip_ws(q+1);
            }
        }
        p++;
    }
    return 0;
}
static void jstr(const char* p,char* out,int cap){
    out[0]=0; if(!p||*p!='"') return;
    p++; int i=0; while(*p&&*p!='"'&&i<cap-1) out[i++]=*p++; out[i]=0;
}
static void parse_cfg(const char* s,size_t n){
    const char* p=s; const char* end=s+n; g_count=0; g_sfxcount=0;
    while(p<end && (g_count<MAX_ITEMS && g_sfxcount<MAX_ITEMS)){
        const char* o=strchr(p,'{'); if(!o) break;
        const char* c=strchr(o,'}'); if(!c) break;
        const char* e;
        int en=1, mu=0, ty=0;
        char file[200]={0}, pack[200]={0};
        e=find_key(o,c,"enable");   if(e&&e<c) en=(strncmp(e,"true",4)==0);
        e=find_key(o,c,"music");    if(e&&e<c) mu=atoi(e);
        e=find_key(o,c,"type");     if(e&&e<c) ty=atoi(e);
        e=find_key(o,c,"file");     if(e&&e<c) jstr(e,file,sizeof(file));
        e=find_key(o,c,"pack");     if(e&&e<c) jstr(e,pack,sizeof(pack));
        if(ty>0 && file[0]){
            if(g_sfxcount<MAX_ITEMS){
                sfx_item_t* si=&g_sfxitems[g_sfxcount]; memset(si,0,sizeof(*si));
                si->enable=en; si->type=ty;
                strncpy(si->file,file,sizeof(si->file)-1);
                strncpy(si->pack,pack,sizeof(si->pack)-1);
                g_sfxcount++;
            }
        } else if(mu>0 && file[0]){
            if(g_count<MAX_ITEMS){
                item_t* it=&g_items[g_count]; memset(it,0,sizeof(*it));
                it->enable=en; it->music=mu;
                strncpy(it->file,file,sizeof(it->file)-1);
                strncpy(it->pack,pack,sizeof(it->pack)-1);
                g_count++;
            }
        }
        p=c+1;
    }
    LOGI("cfg items=%d sfx=%d", g_count, g_sfxcount);
    for(int i=0;i<g_count;i++) LOGI("  [music] music=%d file=%s",g_items[i].music,g_items[i].file);
    for(int i=0;i<g_sfxcount;i++) LOGI("  [sfx] type=%d file=%s",g_sfxitems[i].type,g_sfxitems[i].file);
}

static void load_cfg(void){
    char path[600]; snprintf(path,sizeof(path),"%s/config.json",g_dir);
    FILE* f=fopen(path,"rb"); if(!f){ LOGE("no cfg %s",path); return; }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    if(n<=0||n>131072){ fclose(f); return; }
    char* b=(char*)malloc(n+1); if(!b){fclose(f); return;}
    size_t rd=fread(b,1,n,f); b[rd]=0; fclose(f);
    parse_cfg(b,rd); free(b);
}
static void load_cfg_auto(void){
    load_cfg();
}

static JavaVM* g_vm = 0;
static jobject g_player = 0;
static int g_active = -1;
/* --- g_player 交接锁：所有对 g_player 的取/置必须走 pl_take/pl_install/pl_ref --- */
static pthread_mutex_t g_plk = PTHREAD_MUTEX_INITIALIZER;
/* 加锁读取当前 player，返回一个新的局部引用(可为 0)；调用方负责 DeleteLocalRef */
static jobject pl_ref(JNIEnv* e){
    pthread_mutex_lock(&g_plk);
    jobject p = g_player;
    if(p) p = (*e)->NewLocalRef(e, p);
    pthread_mutex_unlock(&g_plk);
    return p;
}
/* 取走当前 player 所有权并置空(返回可能是 0)；调用方负责 stop/release/DeleteGlobalRef */
static jobject pl_take(void){
    pthread_mutex_lock(&g_plk); jobject p = g_player; g_player = 0; pthread_mutex_unlock(&g_plk); return p;
}
/* 安装新 player(globalref)，返回被顶替的旧 player；调用方负责关掉旧值 */
static jobject pl_install(jobject np){
    pthread_mutex_lock(&g_plk); jobject old = g_player; g_player = np; pthread_mutex_unlock(&g_plk); return old;
}
const char* g_pack_sel = 0;
static JNIEnv* jenv(int* det){
    if(!g_vm) return 0; JNIEnv* e=0; *det=0;
    int r=(*g_vm)->GetEnv(g_vm,(void**)&e,JNI_VERSION_1_6);
    if(r==JNI_EDETACHED){ if((*g_vm)->AttachCurrentThread(g_vm,&e,0)!=JNI_OK) return 0; *det=1; }
    else if(r!=JNI_OK) return 0;
    return e;
}
static void jdet(int d){ if(d&&g_vm) (*g_vm)->DetachCurrentThread(g_vm); }
static void light_stop(void){
    jobject pl = pl_take();
    if(!pl) return;
    int d=0; JNIEnv* e=jenv(&d); if(!e){ pl_install(pl); return; }
    jclass c=(*e)->GetObjectClass(e,pl);
    if(c){
        jmethodID st=(*e)->GetMethodID(e,c,"stop","()V"); if(st) (*e)->CallVoidMethod(e,pl,st);
        jmethodID rl=(*e)->GetMethodID(e,c,"release","()V"); if(rl) (*e)->CallVoidMethod(e,pl,rl);
        (*e)->DeleteLocalRef(e,c);
    }
    if((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e);
    (*e)->DeleteGlobalRef(e,pl);
    g_playing_file[0]=0; g_playing_alive=0;
    jdet(d);
    LOGI("light_stop");
}
static void stop_player(void){
    jobject pl = pl_take();
    if(!pl) return;
    int d=0; JNIEnv* e=jenv(&d); if(!e){ pl_install(pl); return; }
    jclass c=(*e)->GetObjectClass(e,pl);
    if(c){
        jmethodID st=(*e)->GetMethodID(e,c,"stop","()V");
        jmethodID rl=(*e)->GetMethodID(e,c,"release","()V");
        if(st) (*e)->CallVoidMethod(e,pl,st);
        if(rl) (*e)->CallVoidMethod(e,pl,rl);
        (*e)->DeleteLocalRef(e,c);
    }
    if((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e);
    (*e)->DeleteGlobalRef(e,pl);
    g_playing_file[0]=0;
    g_playing_alive=0;
    jdet(d);
}
static void play_ogg(const char* file);
static char g_pend_file[300] = {0};
static pthread_t g_play_thread;
static void* play_thread_fn(void* a){
    (void)a;
    char f[300]; strncpy(f, g_pend_file, sizeof(f)-1); f[sizeof(f)-1]=0;
    play_ogg(f);
    return 0;
}
static void update_player_vol(void){
    if(g_fading || !g_playing_alive) return;
    int d=0; JNIEnv* e=jenv(&d); if(!e) return;
    jobject pl=pl_ref(e);
    if(pl){
        jclass c=(*e)->GetObjectClass(e,pl);
        if(c){ jmethodID sv=(*e)->GetMethodID(e,c,"setVolume","(FF)V"); if(sv){ float vv=g_game_vol>0.0f?g_game_vol:0.75f; if(vv>1.0f)vv=1.0f; (*e)->CallVoidMethod(e,pl,sv,vv,vv); if((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e); } (*e)->DeleteLocalRef(e,c); }
        (*e)->DeleteLocalRef(e,pl);
    }
    jdet(d);
}
static void play_ogg_async(const char* file, int idx){
    g_cur_music = idx;
    g_play_fail = 0;
    if(g_playing_alive && g_playing_file[0] && strcmp(g_playing_file, file)==0 && !g_fading){
        LOGI("SKIP replay same file %s", file);
        return;
    }
    g_playing_alive = 1;
    strncpy(g_playing_file, file, sizeof(g_playing_file)-1); g_playing_file[sizeof(g_playing_file)-1]=0;
    strncpy(g_pend_file, file, sizeof(g_pend_file)-1); g_pend_file[sizeof(g_pend_file)-1]=0;
    if(pthread_create(&g_play_thread,0,play_thread_fn,0)==0) pthread_detach(g_play_thread);
}
static int g_bg_paused = 0;
static void pause_player(void){
    if(g_bg_paused) return;
    int d=0; JNIEnv* e=jenv(&d); if(!e) return;
    jobject pl=pl_ref(e);
    if(pl){
        jclass c=(*e)->GetObjectClass(e,pl);
        if(c){ jmethodID pa=(*e)->GetMethodID(e,c,"pause","()V"); if(pa){ (*e)->CallVoidMethod(e,pl,pa); if((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e); g_bg_paused=1; LOGI("BG pause"); } (*e)->DeleteLocalRef(e,c); }
        (*e)->DeleteLocalRef(e,pl);
    }
    jdet(d);
}
static void resume_player(void){
    if(!g_bg_paused) return;
    int d=0; JNIEnv* e=jenv(&d); if(!e) return;
    jobject pl=pl_ref(e);
    if(pl){
        jclass c=(*e)->GetObjectClass(e,pl);
        if(c){ jmethodID st=(*e)->GetMethodID(e,c,"start","()V"); if(st){ (*e)->CallVoidMethod(e,pl,st); if((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e); } (*e)->DeleteLocalRef(e,c); }
        (*e)->DeleteLocalRef(e,pl);
    }
    g_bg_paused=0; LOGI("BG resume");
    jdet(d);
}
static long now_ms(void);
static long long g_fade_start_ms = 0;
static int g_fade_running = 0;
static pthread_t g_fade_thread;
static void* fade_thread_fn(void* a){
  (void)a; int d=0; JNIEnv* e=jenv(&d); if(!e){g_fade_running=0;return 0;}
  jobject pl = pl_ref(e);   /* 安全局部引用；若期间被换掉，下面会检测并放弃 */
  jmethodID sv=0;
  if(pl){ jclass c=(*e)->GetObjectClass(e,pl); if(c){ sv=(*e)->GetMethodID(e,c,"setVolume","(FF)V"); (*e)->DeleteLocalRef(e,c); } }
  int v=1000;
  LOGI("FADE begin player=%p", (void*)pl);
  while(v>0 && g_fade_running){
    v-=15; if(v<0)v=0;
    if(pl && sv){ float fbase=g_game_vol>0.0f?g_game_vol:0.75f; float fv=(float)v/1000.0f*fbase; (*e)->CallVoidMethod(e,pl,sv,fv,fv); if((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e); }
    struct timespec ts; ts.tv_sec=0; ts.tv_nsec=15000000L; nanosleep(&ts,0);
  }
  if(pl)(*e)->DeleteLocalRef(e,pl);
  LOGI("FADE end v=%d running=%d", v, g_fade_running); jdet(d); if(v<=0){ light_stop(); g_active=-1; g_cover=0; g_release=1; g_release_ms=now_ms(); } g_fading=0; g_fade_running=0; return 0;
}
static void start_fade(void){
  LOGI("start_fade fading=%d player=%p", g_fading, (void*)g_player);
  if(!g_player){ g_active=-1; g_cover=0; g_release=1; g_release_ms=now_ms(); return; }
  if(g_fading){ g_fade_start_ms=now_ms(); g_cover_ms=now_ms(); return; } g_fading=1; g_fade_start_ms=now_ms(); g_fade_running=1; int r=pthread_create(&g_fade_thread,0,fade_thread_fn,0); if(r!=0){g_fading=0;g_fade_running=0;} else pthread_detach(g_fade_thread);
}
static volatile long g_last_hook_ms = 0;
static int g_wd_running = 0;
static long now_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec*1000L + ts.tv_nsec/1000000L; }
static void* watchdog_fn(void* a){
  (void)a;
  while(g_wd_running){
    struct timespec ts; ts.tv_sec=0; ts.tv_nsec=300000000L; nanosleep(&ts,0);
    long now=now_ms();
    if(g_last_hook_ms>0 && now-g_last_hook_ms>500 && g_player && !g_bg_paused){
      pause_player();
    }
  }
  return 0;
}
static void start_watchdog(void){
  if(g_wd_running) return; g_wd_running=1;
  pthread_t t; if(pthread_create(&t,0,watchdog_fn,0)==0) pthread_detach(t);
}
static char g_cache_dir[600] = {0};
static void mp_cache_dir_init(void){
    snprintf(g_cache_dir,sizeof(g_cache_dir),"%s/cache",g_dir);
    mp_mkdirs(g_cache_dir);
}

static int resolve_audio(const char* file, const char* pack, char* out, int cap);
/* ---------- file verify ---------- */
static int probe_file(const char* file){
    int d=0; JNIEnv* e=jenv(&d); if(!e) return 1;
    jclass c=(*e)->FindClass(e,"android/media/MediaPlayer");
    if(!c){ jdet(d); return 1; }
    jmethodID ctor=(*e)->GetMethodID(e,c,"<init>","()V");
    jmethodID sd=(*e)->GetMethodID(e,c,"setDataSource","(Ljava/lang/String;)V");
    jmethodID pr=(*e)->GetMethodID(e,c,"prepare","()V");
    jmethodID rl=(*e)->GetMethodID(e,c,"release","()V");
    if(!ctor||!sd||!pr){ (*e)->DeleteLocalRef(e,c); jdet(d); return 1; }
    char full[768];
    resolve_audio(file, g_pack_sel, full, sizeof(full));
    jobject mp=(*e)->NewObject(e,c,ctor);
    if(!mp){ (*e)->DeleteLocalRef(e,c); jdet(d); return 1; }
    int ok=0;
    jstring js=(*e)->NewStringUTF(e,full);
    (*e)->CallVoidMethod(e,mp,sd,js);
    if(!(*e)->ExceptionOccurred(e)){
        (*e)->CallVoidMethod(e,mp,pr);
        if(!(*e)->ExceptionOccurred(e)) ok=1;
        else (*e)->ExceptionClear(e);
    } else (*e)->ExceptionClear(e);
    if(rl) (*e)->CallVoidMethod(e,mp,rl);
    if((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e);
    (*e)->DeleteLocalRef(e,js);
    (*e)->DeleteLocalRef(e,mp);
    (*e)->DeleteLocalRef(e,c);
    jdet(d);
    return ok;
}
static volatile int g_verify_running = 0;
static void* verify_thread_fn(void* a){
    (void)a;
    LOGI("VERIFY start n=%d", g_count);
    int badn=0;
    for(int i=0;i<g_count;i++){
        if(!g_verify_running) break;
        if(!g_items[i].enable || !g_items[i].file[0]) continue;
        g_items[i].bad = probe_file(g_items[i].file) ? 0 : 1;
        if(g_items[i].bad) badn++;
        LOGI("VERIFY i=%d music=%d bad=%d file=%s", i, g_items[i].music, g_items[i].bad, g_items[i].file);
        struct timespec ts; ts.tv_sec=0; ts.tv_nsec=150000000L; nanosleep(&ts,0);
    }
    LOGI("VERIFY done bad=%d", badn);
    g_verify_running = 0;
    return 0;
}
static void start_verify_all(void){
    if(g_verify_running) return;
    g_verify_running = 1;
    pthread_t t; if(pthread_create(&t,0,verify_thread_fn,0)==0) pthread_detach(t);
    else g_verify_running = 0;
}
static int resolve_audio(const char* file, const char* pack, char* out, int cap){
    (void)pack;
    /* 1) 优先 cache 目录(如存在同名文件) */
    if(g_cache_dir[0]){
        char dst[700]; snprintf(dst,sizeof(dst),"%s/%s",g_cache_dir,file);
        struct stat st;
        if(stat(dst,&st)==0 && st.st_size>0){ snprintf(out,cap,"%s",dst); return 1; }
    }
    /* 2) music_packs/<pack>/<file> (pack 非空时) */
    if(pack && pack[0]){
        char p1[700]; snprintf(p1,sizeof(p1),"%s/music_packs/%s/%s",g_dir,pack,file);
        struct stat st1;
        if(stat(p1,&st1)==0 && st1.st_size>0){ snprintf(out,cap,"%s",p1); return 1; }
    }
    /* 3) music_packs/<file> */
    snprintf(out,cap,"%s/music_packs/%s",g_dir,file);
    return 1;
}
static void play_ogg(const char* file){
    g_bg_paused = 0;
    stop_player();
    int d=0; JNIEnv* e=jenv(&d); if(!e) return;
    jclass c=(*e)->FindClass(e,"android/media/MediaPlayer");
    if(!c){ g_play_fail=1; g_fail_music=g_cur_music; LOGE("no MediaPlayer"); jdet(d); return; }
    jmethodID ctor=(*e)->GetMethodID(e,c,"<init>","()V");
    jmethodID sd=(*e)->GetMethodID(e,c,"setDataSource","(Ljava/lang/String;)V");
    jmethodID pr=(*e)->GetMethodID(e,c,"prepare","()V");
    jmethodID st=(*e)->GetMethodID(e,c,"start","()V");
    jmethodID lp=(*e)->GetMethodID(e,c,"setLooping","(Z)V");
    if(!ctor||!sd||!pr||!st){ g_play_fail=1; g_fail_music=g_cur_music; LOGE("mp methods missing"); (*e)->DeleteLocalRef(e,c); jdet(d); return; }
    jobject mp=(*e)->NewObject(e,c,ctor);
    if(!mp){ g_play_fail=1; g_fail_music=g_cur_music; LOGE("NewObject fail"); (*e)->DeleteLocalRef(e,c); jdet(d); return; }
    char full[768];
    extern const char* g_pack_sel;
    if(!resolve_audio(file, g_pack_sel, full, sizeof(full))){ g_play_fail=1; g_fail_music=g_cur_music; LOGE("resolve fail %s", file); (*e)->DeleteLocalRef(e,mp); (*e)->DeleteLocalRef(e,c); jdet(d); return; }
    int nfd = open(full, O_RDONLY);
    LOGI("open=%d errno=%d %s", nfd, errno, full);
    int useFd = 0;
    if(nfd >= 0){
        jmethodID sdfd = (*e)->GetMethodID(e,c,"setDataSource","(Ljava/io/FileDescriptor;)V");
        if(sdfd){
            jclass fdc = (*e)->FindClass(e,"java/io/FileDescriptor");
            if(fdc){
                jmethodID fdctor = (*e)->GetMethodID(e,fdc,"<init>","()V");
                jfieldID ffd = (*e)->GetFieldID(e,fdc,"descriptor","I");
                if(fdctor && ffd){
                    jobject fdo = (*e)->NewObject(e,fdc,fdctor);
                    if(fdo){
                        (*e)->SetIntField(e,fdo,ffd,(jint)nfd);
                        (*e)->CallVoidMethod(e,mp,sdfd,fdo);
                        useFd = 1;
                        close(nfd);
                        (*e)->DeleteLocalRef(e,fdo);
                    }
                }
            }
        }
        if(!useFd) close(nfd);
    }
    if(!useFd){
        jstring js=(*e)->NewStringUTF(e,full);
        (*e)->CallVoidMethod(e,mp,sd,js);
    }
    (*e)->CallVoidMethod(e,mp,pr);
    if((*e)->ExceptionOccurred(e)){
        (*e)->ExceptionClear(e); g_play_fail=1; g_fail_music=g_cur_music; LOGE("prepare fail %s",full);
        jmethodID rl=(*e)->GetMethodID(e,c,"release","()V"); if(rl) (*e)->CallVoidMethod(e,mp,rl);
        (*e)->DeleteLocalRef(e,mp); (*e)->DeleteLocalRef(e,c);
        jdet(d); return;
    }
    g_fading=0; g_fade_running=0;
    if(lp){ jboolean t=1; (*e)->CallVoidMethod(e,mp,lp,t); }
    { jmethodID sv=(*e)->GetMethodID(e,c,"setVolume","(FF)V"); if(sv){ float base = g_game_vol>0.0f ? g_game_vol : 0.75f; float vv=base; if(vv>1.0f)vv=1.0f; (*e)->CallVoidMethod(e,mp,sv,vv,vv); } }
    (*e)->CallVoidMethod(e,mp,st);
    if((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e);
    {
        jobject gnew=(*e)->NewGlobalRef(e,mp);
        jobject old=pl_install(gnew);   /* 装上新的，拿回旧的 */
        if(old){
            /* 关掉被顶替的旧实例 */
            jclass oc=(*e)->GetObjectClass(e,old);
            if(oc){ jmethodID ost=(*e)->GetMethodID(e,oc,"stop","()V"); jmethodID orl=(*e)->GetMethodID(e,oc,"release","()V");
                if(ost)(*e)->CallVoidMethod(e,old,ost); if(orl)(*e)->CallVoidMethod(e,old,orl); (*e)->DeleteLocalRef(e,oc); }
            if((*e)->ExceptionOccurred(e))(*e)->ExceptionClear(e);
            (*e)->DeleteGlobalRef(e,old);
        }
    }
    LOGI("PLAY %s", full);
    jdet(d);
}
/* ===== SFX replace via SoundPool ===== */
static jobject g_sfxpool = 0;
static volatile int g_sfx_ok = 0;
#define SFX_MAX 128
static int g_sfx_type[SFX_MAX];
static int g_sfx_sid[SFX_MAX];
static int g_sfx_n = 0;

static int sfx_lookup(int type){
    for(int i=0;i<g_sfx_n;i++) if(g_sfx_type[i]==type) return g_sfx_sid[i];
    return -1;
}

static int sfx_load_file(JNIEnv* e, const char* path){
    jclass clsSPc=(*e)->FindClass(e,"android/media/SoundPool"); (*e)->ExceptionClear(e);
    if(!clsSPc) return 0;
    jmethodID loadPath=(*e)->GetMethodID(e,clsSPc,"load","(Ljava/lang/String;I)I"); (*e)->ExceptionClear(e);
    if(!loadPath) return 0;
    int nfd = open(path, O_RDONLY);
    if(nfd < 0){ LOGE("sfx: open fail errno=%d %s", errno, path); return 0; }
    close(nfd);
    jstring jpath=(*e)->NewStringUTF(e,path);
    if(!jpath) return 0;
    int sid=(*e)->CallIntMethod(e,g_sfxpool,loadPath,jpath,(jint)1);
    jobject exA=(*e)->ExceptionOccurred(e);
    if(exA){
        jclass clsT=(*e)->FindClass(e,"java/lang/Throwable");
        jmethodID gmsg=(*e)->GetMethodID(e,clsT,"toString","()Ljava/lang/String;");
        jstring ms=(*e)->CallObjectMethod(e,exA,gmsg);
        const char* cs=(*e)->GetStringUTFChars(e,ms,0);
        LOGE("sfx: load EX: %s", cs?cs:"?");
        if(cs)(*e)->ReleaseStringUTFChars(e,ms,cs);
        (*e)->DeleteLocalRef(e,ms); (*e)->ExceptionClear(e); sid=0;
    }
    (*e)->DeleteLocalRef(e,jpath);
    return sid;
}


static void sfx_scan_load(void){
    int d0=0; JNIEnv* e=jenv(&d0);
    if(!e) return;
    for(int i=0;i<g_sfxcount;i++){
        if(!g_sfxitems[i].enable || g_sfxitems[i].type<=0 || !g_sfxitems[i].file[0]) continue;
        char full[768];
        if(g_sfxitems[i].pack[0]) snprintf(full,sizeof(full),"%s/sfx_packs/%s/%s",g_dir,g_sfxitems[i].pack,g_sfxitems[i].file);
        else snprintf(full,sizeof(full),"%s/sfx_packs/%s",g_dir,g_sfxitems[i].file);
        int sid=sfx_load_file(e,full);
        if(sid>0 && g_sfx_n<SFX_MAX){
            g_sfx_type[g_sfx_n]=g_sfxitems[i].type; g_sfx_sid[g_sfx_n]=sid; g_sfx_n++;
            LOGI("sfx: loaded type=%d sid=%d file=%s", g_sfxitems[i].type, sid, g_sfxitems[i].file);
        } else {
            LOGE("sfx: load fail type=%d file=%s", g_sfxitems[i].type, g_sfxitems[i].file);
        }
    }
    LOGI("sfx: total loaded=%d", g_sfx_n);
    jdet(d0);
}
static void sfx_init(void){
    if(g_sfx_ok || !g_vm) return;
    int d=0; JNIEnv* e=jenv(&d); if(!e){ LOGE("sfx: no env"); return; }
    jclass clsSP=(*e)->FindClass(e,"android/media/SoundPool$Builder");
    if(!clsSP){ LOGE("sfx: no SoundPool.Builder"); (*e)->ExceptionClear(e); jdet(d); return; }
    jmethodID ctorSP=(*e)->GetMethodID(e,clsSP,"<init>","()V");
    jmethodID setMax=(*e)->GetMethodID(e,clsSP,"setMaxStreams","(I)Landroid/media/SoundPool$Builder;");
    jmethodID setAttr=(*e)->GetMethodID(e,clsSP,"setAudioAttributes","(Landroid/media/AudioAttributes;)Landroid/media/SoundPool$Builder;");
    jmethodID build=(*e)->GetMethodID(e,clsSP,"build","()Landroid/media/SoundPool;");
    jclass clsAA=(*e)->FindClass(e,"android/media/AudioAttributes$Builder");
    jmethodID ctorAA=clsAA?(*e)->GetMethodID(e,clsAA,"<init>","()V"):0;
    jmethodID setUsage=clsAA?(*e)->GetMethodID(e,clsAA,"setUsage","(I)Landroid/media/AudioAttributes$Builder;"):0;
    jmethodID setContent=clsAA?(*e)->GetMethodID(e,clsAA,"setContentType","(I)Landroid/media/AudioAttributes$Builder;"):0;
    jmethodID buildAA=clsAA?(*e)->GetMethodID(e,clsAA,"build","()Landroid/media/AudioAttributes;"):0;
    jobject aa=0;
    if(clsAA&&ctorAA&&buildAA){
        jobject ab=(*e)->NewObject(e,clsAA,ctorAA);
        if(setUsage){ jobject r=(*e)->CallObjectMethod(e,ab,setUsage,14); if(r)(*e)->DeleteLocalRef(e,r); }
        if(setContent){ jobject r=(*e)->CallObjectMethod(e,ab,setContent,4); if(r)(*e)->DeleteLocalRef(e,r); }
        aa=(*e)->CallObjectMethod(e,ab,buildAA);
    }
    jobject b=(*e)->NewObject(e,clsSP,ctorSP);
    if(setMax){ jobject r=(*e)->CallObjectMethod(e,b,setMax,8); if(r)(*e)->DeleteLocalRef(e,r); }
    if(setAttr&&aa){ jobject r=(*e)->CallObjectMethod(e,b,setAttr,aa); if(r)(*e)->DeleteLocalRef(e,r); }
    jobject sp=build?(*e)->CallObjectMethod(e,b,build):0;
    if(!sp){ LOGE("sfx: build fail"); if(aa)(*e)->DeleteLocalRef(e,aa); (*e)->DeleteLocalRef(e,b); jdet(d); return; }
    g_sfxpool=(*e)->NewGlobalRef(e,sp);
    (*e)->DeleteLocalRef(e,sp);
    jdet(d);
    g_sfx_ok=1;
    sfx_scan_load();
}

static void sfx_play(int type, float vol){
    if(!g_sfx_ok||!g_sfxpool) return;
    int sid=sfx_lookup(type);
    if(sid<=0) return;
    int d=0; JNIEnv* e=jenv(&d); if(!e) return;
    jclass c=(*e)->GetObjectClass(e,g_sfxpool);
    if(c){ jmethodID pl=(*e)->GetMethodID(e,c,"play","(IFFIIF)I");
        if(pl){ float vv=vol; if(vv<0)vv=0; if(vv>1)vv=1;
            (*e)->CallIntMethod(e,g_sfxpool,pl,(jint)sid,vv,vv,(jint)1,(jint)0,(jfloat)1.0f);
            jobject ex=(*e)->ExceptionOccurred(e);
            if(ex){ (*e)->ExceptionClear(e); }
        }
        (*e)->DeleteLocalRef(e,c); }
    jdet(d);
}

static bool sfx_prefix(patch_handle_t inst, void** args, const patch_method_signature_t* sig, void* result){
    (void)inst;(void)sig;(void)result;
    if(!args) return false;
    if(!g_sfx_ok) return false;
    if(!args[0]) return false;
    int type=*(int*)args[0];
    int sid=sfx_lookup(type);
    if(sid<=0) return false;
    float vol=1.0f;
    if(args[4]){ float v=*(float*)args[4]; if(v>=0.0f && v<=1.0f) vol=v; }
    sfx_play(type, vol);
    return true;
}


static bool hook_prefix(patch_handle_t instance, void** args, const void* sig, void* result){

    (void)instance; (void)result; (void)sig;
    if(!args) return false;
    g_last_hook_ms = now_ms();
    if(g_bg_paused) resume_player();
    int idx = args[1] ? *(int*)args[1] : -1;
    { static int last_idx=-999; if(idx!=last_idx){ last_idx=idx; LOGI("IDX -> %d", idx); } }
    float* vol = args[2] ? (float*)args[2] : 0;
    if(vol){ float b=*vol; if(b>0.0f && b<2.0f) g_game_vol=b; }
    static float last_gv = -1.0f;
    if(g_game_vol>0.0f && g_game_vol!=last_gv){ last_gv=g_game_vol; update_player_vol(); }
    if(g_cover && g_active==-1 && g_cover_ms>0 && now_ms()-g_cover_ms > 1500){ LOGI("cover timeout force clear"); g_cover=0; }
    int hit = -1;
    for(int i=0;i<g_count;i++){
        if(g_items[i].enable && !g_items[i].bad && g_items[i].music==idx){ hit=i; break; }
    }
    int failed = (g_play_fail && g_fail_music==idx);
    if(hit>=0){
        if(!failed){
            if(vol) *vol = 0.0f;
            g_cover = 1; g_cover_ms = now_ms();
            if(g_fading){ LOGI("HIT during fade -> cancel fade, resume"); g_fade_running = 0; g_fading = 0; update_player_vol(); }
            if(g_release){ LOGI("HIT during release-delay -> cancel release"); g_release = 0; }
            if(g_leave_ms){ LOGI("HIT cancels leave timer"); g_leave_ms = 0; }
            if(g_active != idx){
                g_active = idx;
                g_pack_sel = g_items[hit].pack;
                LOGI("HIT music=%d play %s", idx, g_items[hit].file);
                play_ogg_async(g_items[hit].file, idx);
            }
        } else {
            if(g_active != idx){ LOGI("FAIL music=%d, keep original", idx); g_active = idx; }
        }
    } else {
        if(g_active != -1 && !g_fading){
            if(g_leave_ms==0){ g_leave_ms = now_ms(); LOGI("leave %d (hold)", g_active); }
            g_play_fail = 0;
            g_fail_music = -1;
            if(now_ms()-g_leave_ms > 3000){
                LOGI("leave delay done -> fade");
                g_leave_ms = 0;
                start_fade();
            }
            if(vol) *vol = 0.0f;
        } else {
            if(vol){ if(g_fading || g_cover || g_active!=-1 || g_release) *vol = 0.0f; else *vol = g_game_vol>0.0f ? g_game_vol : 0.75f; }
        }
    }
    if(g_release){
        if(!g_fading && g_active==-1 && now_ms()-g_release_ms > 1500){
            LOGI("release original vol after %ldms", now_ms()-g_release_ms);
            g_release = 0; g_cover = 0;
            if(vol) *vol = g_game_vol>0.0f ? g_game_vol : 0.75f;
        }
    }
    return false;
}
static bool init_module(module_entry_t* entry){
    LOGI("===== MusicPack init =====");
    if(entry && entry->private_dir){
        strncpy(g_dir, entry->private_dir, sizeof(g_dir)-1);
        LOGI("private_dir=%s", g_dir);
        mp_ensure_files(g_dir);
        mp_cache_dir_init();
    }
    typedef int (*GetVMs_t)(JavaVM**, jsize, jsize*);
    GetVMs_t fn = 0;
    dl_iterate_phdr(phdr_cb, 0);
    LOGI("libart base=%p path=%s", g_libart_base, g_libart_path[0]?g_libart_path:"(none)");
    if(g_libart_base) fn = (GetVMs_t)find_sym_loaded("JNI_GetCreatedJavaVMs");
    if(!fn) fn = (GetVMs_t)dlsym(RTLD_DEFAULT,"JNI_GetCreatedJavaVMs");
    if(fn){ JavaVM* vm=0; jsize n=0; fn(&vm,1,&n); g_vm=vm; LOGI("vm=%p n=%d",(void*)vm,(int)n); }
    else LOGE("no JNI_GetCreatedJavaVMs");
    load_cfg_auto();
    sfx_init();
    start_verify_all();
    start_watchdog();

    patch_handle_t TLAS = patchlib_type_get_type("Terraria.Audio","LegacyAudioSystem");
    LOGI("TLAS=%p", TLAS);
    if(TLAS){
        patch_handle_t mUCT = patchlib_type_get_method(TLAS,"UpdateCommonTrack");
        LOGI("m.UCT=%p pc=%d", mUCT, mUCT?patchlib_method_get_param_count(mUCT):-1);
        if(mUCT){
            patchlib_install_prepost_hook(mUCT, hook_prefix, 0);
        }
    }
    {
        patch_handle_t TLP = patchlib_type_get_type("Terraria.Audio","LegacySoundPlayer");
        LOGI("TLP=%p", TLP);
        if(TLP){
            patch_handle_t mPS = patchlib_type_get_method_by_param_count(TLP,"PlaySound",6);
            LOGI("mPS=%p", mPS);
            if(mPS) LOGI("PShook=%d",(int)patchlib_install_prepost_hook(mPS, sfx_prefix, 0));
        }
    }
    return true;
}
static bool cleanup_module(module_entry_t* e){ (void)e; g_wd_running=0; g_fade_running=0; stop_player(); return true; }
static void hot_reload(module_entry_t* e){ (void)e; }
static const module_info_t* get_info(void){ return &g_info; }
static const module_ops_t g_ops = { init_module, cleanup_module, hot_reload, get_info };
API_EXPORT const module_ops_t* API_CALL module_create(void){ return &g_ops; }
