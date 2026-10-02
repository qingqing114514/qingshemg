#include "opensl.h"
#include "xnbsound.h"
#include <android/log.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void (*g_sl_log)(const char *msg) = 0;
#ifndef MP_LOG
#define MP_LOG 1
#endif
#if MP_LOG
static void SLOG(const char *fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "MusicPackZ", "%s", b);
    if (g_sl_log) g_sl_log(b);
}
#else
#define SLOG(...) \
    do {          \
    } while (0)
#endif

typedef void *SLObjectItf;
typedef void *SLInterfaceID;
typedef void *SLEngineItf;
typedef void *SLPlayItf;
typedef void *SLAndroidSimpleBufferQueueItf;
typedef void *SLVolumeItf;
typedef uint32_t SLuint32;
typedef uint8_t SLboolean;
typedef int32_t SLmillibel;

#define SL_RESULT_SUCCESS 0
#define SL_BOOLEAN_FALSE 0
#define SL_BOOLEAN_TRUE 1
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE 0x800007BDu
#define SL_DATALOCATOR_OUTPUTMIX 0x00000004u
#define SL_DATAFORMAT_PCM 0x00000002u
#define SL_PCMSAMPLEFORMAT_FIXED_16 0x0010u
#define SL_SPEAKER_FRONT_LEFT 0x1u
#define SL_SPEAKER_FRONT_RIGHT 0x2u
#define SL_SPEAKER_FRONT_CENTER 0x4u
#define SL_BYTEORDER_LITTLEENDIAN 0x00000002u
#define SL_PLAYSTATE_PLAYING 3u

struct SLDataLocator_AndroidSimpleBufferQueue {
    SLuint32 locatorType;
    SLuint32 numBuffers;
};
struct SLDataFormat_PCM {
    SLuint32 formatType;
    SLuint32 numChannels;
    SLuint32 samplesPerSec;
    SLuint32 bitsPerSample;
    SLuint32 containerSize;
    SLuint32 channelMask;
    SLuint32 endianness;
};
struct SLDataSource {
    void *pLocator;
    void *pFormat;
};
struct SLDataLocator_OutputMix {
    SLuint32 locatorType;
    SLObjectItf outputMix;
};
struct SLDataSink {
    void *pLocator;
    void *pFormat;
};

#define VT(p) (*(void ***)(p))
#define FN(p, n) (VT(p)[(n)])

static void *g_sl = 0;
static int (*p_slCreateEngine)(SLObjectItf *, SLuint32, const SLInterfaceID *, const SLboolean *, SLuint32, void *) = 0;
static SLInterfaceID *g_iid_engine = 0;
static SLInterfaceID *g_iid_play = 0;
static SLInterfaceID *g_iid_bq = 0;
static SLInterfaceID *g_iid_volume = 0;

static const int g_rates[9] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
static int nearest_rate(int r) {
    int b = g_rates[0], bd = abs(r - g_rates[0]);
    for (int i = 1; i < 9; i++) {
        int d = abs(r - g_rates[i]);
        if (d < bd) {
            b = g_rates[i];
            bd = d;
        }
    }
    return b;
}
static float my_exp2f(float x) {
    float y = x * 0.6931471805599453f;
    float t = 1.0f;
    float sum = 1.0f;
    for (int i = 1; i <= 12; i++) {
        t *= y / (float)i;
        sum += t;
    }
    return sum;
}
static float my_log10f(float x) {
    if (x <= 0.0f) return 0.0f;
    int e = 0;
    while (x >= 2.0f) {
        x *= 0.5f;
        e++;
    }
    while (x < 1.0f) {
        x *= 2.0f;
        e--;
    }
    float z = (x - 1.0f) / (x + 1.0f);
    float z2 = z * z;
    float sum = 0.0f;
    float num = z;
    for (int i = 0; i < 12; i++) {
        sum += num / (float)(2 * i + 1);
        num *= z2;
    }
    float ln = 2.0f * sum + (float)e * 0.6931471805599453f;
    return ln * 0.4342944819032518f;
}
int nearest_playback_rate(float base, float pitch) {
    if (!(pitch >= -1.0f && pitch <= 1.0f)) pitch = 0.0f;
    int w = (int)(base * my_exp2f(pitch) + 0.5f);
    if (w < 8000) w = 8000;
    if (w > 48000) w = 48000;
    return nearest_rate(w);
}

static SLObjectItf g_engine = 0, g_mix = 0;
static SLEngineItf g_eng = 0;
static int g_init = 0, g_bad = 0;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char key[192];
    char path[768];
    int rate;
    int channels;
} snd_t;
#define SL_MAX_SND 512
static snd_t g_snd[SL_MAX_SND];
static int g_snd_n = 0;
static snd_t *find_snd(const char *key) {
    for (int i = 0; i < g_snd_n; i++)
        if (strcmp(g_snd[i].key, key) == 0) return &g_snd[i];
    return 0;
}

static long long g_now_ms(void);
#define POOL_MAX 128
/* 每个 player 挂多块 buffer 轮转, 避免高频播放时覆盖正在播的 PCM */
#define NBS 1
typedef struct {
    SLObjectItf player;
    SLPlayItf play;
    SLAndroidSimpleBufferQueueItf bq;
    unsigned char *bufs[NBS];
    int bufcap[NBS];
    int bidx;
    int bytes;
    int rate;
    int channels;
    int inuse;
    long long end_ms;
} pslot_t;
static pslot_t g_pool[POOL_MAX];
static int g_pool_n = 0;

static int pb_playing(pslot_t *p) {
    if (!p->play) return 0;
    typedef int (*GetPs_t)(SLPlayItf, SLuint32 *);
    SLuint32 st = 0;
    if (((GetPs_t)FN(p->play, 1))(p->play, &st) != 0) return 0;
    return st == SL_PLAYSTATE_PLAYING;
}

static int pool_create_in(pslot_t *p, int rate, int ch) {
    memset(p, 0, sizeof(*p));
    struct SLDataLocator_AndroidSimpleBufferQueue bq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, NBS};
    struct SLDataFormat_PCM fmt = {SL_DATAFORMAT_PCM, (SLuint32)ch, (SLuint32)(rate * 1000), SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16, ch == 1 ? SL_SPEAKER_FRONT_CENTER : (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT), SL_BYTEORDER_LITTLEENDIAN};
    struct SLDataSource src = {&bq, &fmt};
    struct SLDataLocator_OutputMix outm = {SL_DATALOCATOR_OUTPUTMIX, g_mix};
    struct SLDataSink snk = {&outm, 0};
    SLInterfaceID ids[1] = {*g_iid_bq};
    SLboolean req[1] = {SL_BOOLEAN_TRUE};
    {
        typedef int (*CreatePlayer_t)(SLEngineItf, SLObjectItf *, void *, void *, SLuint32, const SLInterfaceID *, const SLboolean *);
        if (((CreatePlayer_t)FN(g_eng, 2))(g_eng, &p->player, &src, &snk, 1, ids, req) != SL_RESULT_SUCCESS || !p->player) goto fail;
    }
    {
        typedef int (*Realize_t)(SLObjectItf, SLboolean);
        if (((Realize_t)FN(p->player, 0))(p->player, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
            goto fail;
        }
    }
    {
        typedef int (*GetIf_t)(SLObjectItf, SLInterfaceID, void *);
        if (((GetIf_t)FN(p->player, 3))(p->player, *g_iid_play, &p->play) != SL_RESULT_SUCCESS || !p->play) {
            goto fail;
        }
    }
    {
        typedef int (*GetIf_t)(SLObjectItf, SLInterfaceID, void *);
        if (((GetIf_t)FN(p->player, 3))(p->player, *g_iid_bq, &p->bq) != SL_RESULT_SUCCESS || !p->bq) {
            goto fail;
        }
    }
    p->rate = rate;
    p->channels = ch;
    return 0;
fail:
    if (p->player) {
        typedef void (*Destroy_t)(SLObjectItf);
        ((Destroy_t)FN(p->player, 6))(p->player);
        p->player = 0;
    }
    p->play = 0;
    p->bq = 0;
    return -1;
}

static int pool_new(int rate, int ch) {
    if (g_pool_n >= POOL_MAX) {
        SLOG("POOL FULL %d", g_pool_n);
        return -1;
    }
    pslot_t *p = &g_pool[g_pool_n];
    if (pool_create_in(p, rate, ch) != 0) {
        SLOG("pool create fail");
        return -1;
    }
    g_pool_n++;
    return g_pool_n - 1;
}

#define GROUP_MAX 8
static pslot_t *pool_acquire(int rate, int ch) {
    /* count slots of this combo, find first idle */
    int cnt = 0;
    pslot_t *idle = 0;
    for (int i = 0; i < g_pool_n; i++) {
        pslot_t *p = &g_pool[i];
        if (p->rate != rate || p->channels != ch) continue;
        cnt++;
        if (!idle && (!p->inuse || g_now_ms() >= p->end_ms)) idle = p;
    }
    if (idle) {
        idle->inuse = 0;
        return idle;
    }
    /* none idle: allow up to GROUP_MAX players for this combo (concurrent overlap) */
    if (cnt < GROUP_MAX) {
        int idx = pool_new(rate, ch);
        if (idx >= 0) return &g_pool[idx];
    }
    /* group full (or pool full): reuse oldest-ends slot of this combo */
    pslot_t *oldest = 0;
    long long ot = 0;
    for (int i = 0; i < g_pool_n; i++) {
        pslot_t *p = &g_pool[i];
        if (p->rate != rate || p->channels != ch) continue;
        long long e = p->inuse ? p->end_ms : 0;
        if (!oldest || e < ot) {
            oldest = p;
            ot = e;
        }
    }
    if (oldest) return oldest;
    /* fallback: steal any oldest (pool totally full) */
    int bj = -1;
    long long bt = 0;
    for (int i = 0; i < g_pool_n; i++) {
        pslot_t *p = &g_pool[i];
        long long e = p->inuse ? p->end_ms : 0;
        if (bj < 0 || e < bt) {
            bj = i;
            bt = e;
        }
    }
    if (bj >= 0) {
        pslot_t *p = &g_pool[bj];
        if (p->player) {
            typedef void (*Destroy_t)(SLObjectItf);
            ((Destroy_t)FN(p->player, 6))(p->player);
            p->player = 0;
        }
        p->play = 0;
        p->bq = 0;
        for (int i = 0; i < NBS; i++) {
            if (p->bufs[i]) {
                free(p->bufs[i]);
                p->bufs[i] = 0;
            }
            p->bufcap[i] = 0;
        }
        p->bidx = 0;
        if (pool_create_in(p, rate, ch) != 0) {
            SLOG("POOL steal rebuild fail");
            return 0;
        }
        SLOG("POOL steal ok idx=%d", bj);
        return p;
    }
    return 0;
}

int sl_init(void) {
    pthread_mutex_lock(&g_mtx);
    if (g_init) {
        pthread_mutex_unlock(&g_mtx);
        return 1;
    }
    if (g_bad) {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }
    if (!g_sl) {
        g_sl = dlopen("libOpenSLES.so", RTLD_NOW);
        if (!g_sl) {
            g_bad = 1;
            pthread_mutex_unlock(&g_mtx);
            SLOG("sl: dlopen fail");
            return 0;
        }
    }
    p_slCreateEngine = (int (*)(SLObjectItf *, SLuint32, const SLInterfaceID *, const SLboolean *, SLuint32, void *))dlsym(g_sl, "slCreateEngine");
    g_iid_engine = (SLInterfaceID *)dlsym(g_sl, "SL_IID_ENGINE");
    g_iid_play = (SLInterfaceID *)dlsym(g_sl, "SL_IID_PLAY");
    g_iid_bq = (SLInterfaceID *)dlsym(g_sl, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
    g_iid_volume = (SLInterfaceID *)dlsym(g_sl, "SL_IID_VOLUME");
    if (!p_slCreateEngine) {
        g_bad = 1;
        pthread_mutex_unlock(&g_mtx);
        SLOG("sl: no slCreateEngine");
        return 0;
    }
    if (p_slCreateEngine(&g_engine, 0, 0, 0, 0, 0) != SL_RESULT_SUCCESS || !g_engine) {
        g_bad = 1;
        pthread_mutex_unlock(&g_mtx);
        SLOG("sl: slCreateEngine fail");
        return 0;
    }
    {
        typedef int (*Realize_t)(SLObjectItf, SLboolean);
        if (((Realize_t)FN(g_engine, 0))(g_engine, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
            g_bad = 1;
            pthread_mutex_unlock(&g_mtx);
            SLOG("sl: Realize fail");
            return 0;
        }
    }
    {
        typedef int (*GetIf_t)(SLObjectItf, SLInterfaceID, void *);
        if (((GetIf_t)FN(g_engine, 3))(g_engine, *g_iid_engine, &g_eng) != SL_RESULT_SUCCESS || !g_eng) {
            g_bad = 1;
            pthread_mutex_unlock(&g_mtx);
            SLOG("sl: ENGINE fail");
            return 0;
        }
    }
    {
        typedef int (*CreateOutMix_t)(SLEngineItf, SLObjectItf *, SLuint32, const SLInterfaceID *, const SLboolean *);
        if (((CreateOutMix_t)FN(g_eng, 7))(g_eng, &g_mix, 0, 0, 0) != SL_RESULT_SUCCESS || !g_mix) {
            g_bad = 1;
            pthread_mutex_unlock(&g_mtx);
            SLOG("sl: CreateOutputMix fail");
            return 0;
        }
    }
    {
        typedef int (*Realize_t)(SLObjectItf, SLboolean);
        if (((Realize_t)FN(g_mix, 0))(g_mix, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
            g_bad = 1;
            pthread_mutex_unlock(&g_mtx);
            SLOG("sl: mix Realize fail");
            return 0;
        }
    }
    g_init = 1;
    pthread_mutex_unlock(&g_mtx);
    SLOG("sl: init ok");
    return 1;
}
int sl_is_init(void) {
    return g_init;
}

static long long g_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}
#define SL_DEBOUNCE_MS 40
typedef struct {
    char key[192];
    long long t;
} dbn_t;
static dbn_t g_db[256];
static int g_dbn = 0;

#define SND_CACHE_MAX 128
typedef struct {
    char path[768];
    unsigned char *pcm;
    int bytes;
    int rate;
    int ch;
    long long last;
} scache_t;
static scache_t g_sc[SND_CACHE_MAX];
static int g_sc_n = 0;
static long long g_sc_tick = 0;

static void sc_clear(void) {
    for (int i = 0; i < g_sc_n; i++) {
        if (g_sc[i].pcm) {
            free(g_sc[i].pcm);
            g_sc[i].pcm = 0;
        }
        g_sc[i].bytes = 0;
        g_sc[i].path[0] = 0;
    }
    g_sc_n = 0;
}

static int sc_fetch(const char *path, unsigned char **pcm, int *bytes, int *rate, int *ch) {
    for (int i = 0; i < g_sc_n; i++) {
        if (g_sc[i].pcm && strcmp(g_sc[i].path, path) == 0) {
            *pcm = g_sc[i].pcm;
            *bytes = g_sc[i].bytes;
            *rate = g_sc[i].rate;
            *ch = g_sc[i].ch;
            g_sc[i].last = ++g_sc_tick;
            return 1;
        }
    }
    /* fast path: <path> is a raw .pcm with sibling <path>.meta (rate ch bytes) */
    {
        char mp[800];
        snprintf(mp, sizeof(mp), "%s.meta", path);
        FILE *mf = fopen(mp, "r");
        if (mf) {
            int mr = 0, mc = 0, mb = 0;
            if (fscanf(mf, "%d %d %d", &mr, &mc, &mb) == 3 && mr > 0 && mc > 0 && mb > 0) {
                fclose(mf);
                FILE *pf = fopen(path, "rb");
                if (pf) {
                    unsigned char *pp = (unsigned char *)malloc(mb);
                    if (pp && fread(pp, 1, mb, pf) == (size_t)mb) {
                        fclose(pf);
                        int slot = -1;
                        if (g_sc_n < SND_CACHE_MAX) {
                            slot = g_sc_n++;
                        } else {
                            long long mn = g_sc[0].last;
                            int mi = 0;
                            for (int i = 1; i < g_sc_n; i++) {
                                if (g_sc[i].last < mn) {
                                    mn = g_sc[i].last;
                                    mi = i;
                                }
                            }
                            slot = mi;
                            if (g_sc[slot].pcm) free(g_sc[slot].pcm);
                        }
                        g_sc[slot].pcm = pp;
                        g_sc[slot].bytes = mb;
                        g_sc[slot].rate = mr;
                        g_sc[slot].ch = mc;
                        g_sc[slot].last = ++g_sc_tick;
                        strncpy(g_sc[slot].path, path, 767);
                        g_sc[slot].path[767] = 0;
                        *pcm = pp;
                        *bytes = mb;
                        *rate = mr;
                        *ch = mc;
                        return 1;
                    }
                    if (pp) free(pp);
                    fclose(pf);
                }
            } else
                fclose(mf);
        }
    }
    xnb_sound_t xs;
    char err[128];
    if (!xnb_parse_sound(path, &xs, err, sizeof(err))) return 0;
    unsigned char *p = 0;
    int b = 0, r = 0, c = 0;
    int ok = xnb_normalize_pcm16(&xs, &p, &b, &r, &c, err, sizeof(err));
    xnb_sound_free(&xs);
    if (!ok || !p) return 0;
    int slot = -1;
    if (g_sc_n < SND_CACHE_MAX) {
        slot = g_sc_n++;
    } else {
        long long mn = g_sc[0].last;
        int mi = 0;
        for (int i = 1; i < g_sc_n; i++) {
            if (g_sc[i].last < mn) {
                mn = g_sc[i].last;
                mi = i;
            }
        }
        slot = mi;
        if (g_sc[slot].pcm) free(g_sc[slot].pcm);
    }
    g_sc[slot].pcm = p;
    g_sc[slot].bytes = b;
    g_sc[slot].rate = r;
    g_sc[slot].ch = c;
    g_sc[slot].last = ++g_sc_tick;
    strncpy(g_sc[slot].path, path, 767);
    g_sc[slot].path[767] = 0;
    *pcm = p;
    *bytes = b;
    *rate = r;
    *ch = c;
    return 1;
}

int sl_register(const char *key, const char *path, int rate, int channels) {
    if (!key || !path || !*path || rate <= 0 || channels <= 0) return 0;
    pthread_mutex_lock(&g_mtx);
    snd_t *s = find_snd(key);
    if (!s) {
        if (g_snd_n >= SL_MAX_SND) {
            pthread_mutex_unlock(&g_mtx);
            return 0;
        }
        s = &g_snd[g_snd_n];
        strncpy(s->key, key, sizeof(s->key) - 1);
        s->key[sizeof(s->key) - 1] = 0;
        g_snd_n++;
    }
    strncpy(s->path, path, sizeof(s->path) - 1);
    s->path[sizeof(s->path) - 1] = 0;
    s->rate = rate;
    s->channels = channels;
    pthread_mutex_unlock(&g_mtx);
    SLOG("REG key=%s n=%d", key, g_snd_n);
    return 1;
}

int sl_has(const char *key) {
    pthread_mutex_lock(&g_mtx);
    int r = find_snd(key) ? 1 : 0;
    int n = g_snd_n;
    pthread_mutex_unlock(&g_mtx);
    return r;
}

/* 内部体: 调用者必须已持 g_mtx. 返回1成功/0失败, 去抖命中返回1(已处理) */
static int play_vol_locked(const char *key, snd_t *in_s, float pitch_offset, float volume) {
    if (!g_init) return 0;
    for (int i = 0; i < g_dbn; i++) {
        if (strcmp(g_db[i].key, key) == 0) {
            long long now = g_now_ms();
            if (now - g_db[i].t < SL_DEBOUNCE_MS) return 1;
            g_db[i].t = now;
            goto db_ok;
        }
    }
    if (g_dbn < 256) {
        strncpy(g_db[g_dbn].key, key, sizeof(g_db[0].key) - 1);
        g_db[g_dbn].key[sizeof(g_db[0].key) - 1] = 0;
        g_db[g_dbn].t = g_now_ms();
        g_dbn++;
    }
db_ok:;
    snd_t *s = in_s ? in_s : find_snd(key);
    if (!s || !s->path[0]) return 0;
    unsigned char *sptr = 0;
    int sb = 0, sr = 0, scn = 0;
    if (!sc_fetch(s->path, &sptr, &sb, &sr, &scn)) return 0;
    int rate = nearest_playback_rate((float)sr, pitch_offset);
    int ch = scn;
    pslot_t *p = pool_acquire(rate, ch);
    if (!p) return 0;
    /* 轮转使用多块 buffer, 避免覆盖正在播的 PCM */
    int bi = p->bidx;
    p->bidx = (p->bidx + 1) % NBS;
    if (!p->bufs[bi] || p->bufcap[bi] < sb) {
        unsigned char *nb = (unsigned char *)malloc(sb);
        if (!nb) {
            p->bidx = bi;
            return 0;
        }
        if (p->bufs[bi]) free(p->bufs[bi]);
        p->bufs[bi] = nb;
        p->bufcap[bi] = sb;
    }
    memcpy(p->bufs[bi], sptr, sb);
    p->bytes = sb;
    {
        typedef int (*Clr_t)(SLAndroidSimpleBufferQueueItf);
        ((Clr_t)FN(p->bq, 1))(p->bq);
    }
    {
        typedef int (*Enq_t)(SLAndroidSimpleBufferQueueItf, const void *, SLuint32);
        if (((Enq_t)FN(p->bq, 0))(p->bq, p->bufs[bi], (SLuint32)p->bytes) != SL_RESULT_SUCCESS) return 0;
    }
    if (volume > 0.0f && volume < 2.0f) {
        SLVolumeItf vol = 0;
        typedef int (*GetIf_t)(SLObjectItf, SLInterfaceID, void *);
        if (((GetIf_t)FN(p->player, 3))(p->player, *g_iid_volume, &vol) == SL_RESULT_SUCCESS && vol) {
            typedef int (*SetVol_t)(SLVolumeItf, SLmillibel);
            ((SetVol_t)FN(vol, 0))(vol, (SLmillibel)(2000.0f * my_log10f(volume)));
        }
    }
    {
        typedef int (*SetPs_t)(SLPlayItf, SLuint32);
        ((SetPs_t)FN(p->play, 0))(p->play, SL_PLAYSTATE_PLAYING);
    }
    p->inuse = 1;
    {
        long long dur = (rate > 0) ? ((long long)p->bytes * 1000LL / (2LL * (rate / 1000) * (long long)ch)) : 1000LL;
        p->end_ms = g_now_ms() + dur + 200;
    }
    return 1;
}

int sl_play_vol(const char *key, float pitch_offset, float volume) {
    pthread_mutex_lock(&g_mtx);
    int r = play_vol_locked(key, 0, pitch_offset, volume);
    pthread_mutex_unlock(&g_mtx);
    return r;
}

/* 一次加锁: 存在则直接播放并返回1(无论实际是否出声, 与 sl_has 语义一致); 未注册返回0.
   高频 hook 路径用, 省掉 sl_has + sl_play_vol 两次锁 */
int sl_try_play(const char *key, float pitch_offset, float volume) {
    pthread_mutex_lock(&g_mtx);
    int r = 0;
    snd_t *s = find_snd(key);
    if (s) {
        r = 1;
        play_vol_locked(key, s, pitch_offset, volume);
    }
    pthread_mutex_unlock(&g_mtx);
    return r;
}

/* 一次加锁, 先试 k1 再试 k2 (高频 hook 路径省一次锁) */
int sl_try_play2(const char *k1, const char *k2, float pitch_offset, float volume) {
    pthread_mutex_lock(&g_mtx);
    int r = 0;
    snd_t *s = k1 ? find_snd(k1) : 0;
    if (s) {
        r = 1;
        play_vol_locked(k1, s, pitch_offset, volume);
    } else {
        snd_t *s2 = k2 ? find_snd(k2) : 0;
        if (s2) {
            r = 1;
            play_vol_locked(k2, s2, pitch_offset, volume);
        }
    }
    pthread_mutex_unlock(&g_mtx);
    return r;
}

/* 预加载: 把已注册 key 的 PCM 提前读进缓存(消除首播延迟) */
int sl_preload(const char *key) {
    pthread_mutex_lock(&g_mtx);
    int r = 0;
    snd_t *s = find_snd(key);
    if (s && s->path[0]) {
        unsigned char *ptr = 0;
        int b = 0, rt = 0, cn = 0;
        if (sc_fetch(s->path, &ptr, &b, &rt, &cn)) r = 1;
    }
    pthread_mutex_unlock(&g_mtx);
    return r;
}

/* 预加载全部已注册音效(后台线程调用, 消除首播延迟) */
int sl_preload_all(void) {
    pthread_mutex_lock(&g_mtx);
    int n = 0;
    if (g_snd_n > SND_CACHE_MAX) {
        pthread_mutex_unlock(&g_mtx);
        SLOG("preload skip: %d sounds > cache 128", g_snd_n);
        return 0;
    }
    for (int i = 0; i < g_snd_n; i++) {
        snd_t *s = &g_snd[i];
        if (!s->path[0]) continue;
        unsigned char *ptr = 0;
        int b = 0, rt = 0, cn = 0;
        if (sc_fetch(s->path, &ptr, &b, &rt, &cn)) n++;
    }
    pthread_mutex_unlock(&g_mtx);
    SLOG("preload all: %d/%d", n, g_snd_n);
    return n;
}

int sl_play(const char *key, float pitch_offset) {
    return sl_play_vol(key, pitch_offset, -1.0f);
}
void sl_housekeep(void) {
    pthread_mutex_lock(&g_mtx);
    pthread_mutex_unlock(&g_mtx);
}
void sl_shutdown(void) {
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < g_pool_n; i++) {
        pslot_t *p = &g_pool[i];
        if (p->player) {
            typedef void (*Destroy_t)(SLObjectItf);
            ((Destroy_t)FN(p->player, 6))(p->player);
            p->player = 0;
        }
        for (int j = 0; j < NBS; j++) {
            if (p->bufs[j]) {
                free(p->bufs[j]);
                p->bufs[j] = 0;
            }
            p->bufcap[j] = 0;
        }
    }
    g_pool_n = 0;
    g_snd_n = 0;
    sc_clear();
    if (g_mix) {
        typedef void (*Destroy_t)(SLObjectItf);
        ((Destroy_t)FN(g_mix, 6))(g_mix);
        g_mix = 0;
    }
    if (g_engine) {
        typedef void (*Destroy_t)(SLObjectItf);
        ((Destroy_t)FN(g_engine, 6))(g_engine);
        g_engine = 0;
    }
    g_eng = 0;
    g_init = 0;
    g_bad = 0;
    pthread_mutex_unlock(&g_mtx);
}
