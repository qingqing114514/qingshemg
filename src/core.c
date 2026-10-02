#include "jni_helper.h"
#include "mkdirs.h"
#include "opensl.h"
#include "sxfnames.h"
#include "tefkernel-cpp-wrapper/tefkernel/module/module_core.h"
#include "tefkernel-cpp-wrapper/tefkernel/patchlib/field.h"
#include "tefkernel-cpp-wrapper/tefkernel/patchlib/method.h"
#include "tefkernel-cpp-wrapper/tefkernel/patchlib/property.h"
#include "tefkernel-cpp-wrapper/tefkernel/patchlib/struct/string.h"
#include "tefkernel-cpp-wrapper/tefkernel/patchlib/thread.h"
#include "tefkernel-cpp-wrapper/tefkernel/patchlib/type.h"
#include "xnbsound.h"
#include <android/log.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#define LOG_TAG "MusicPackZ"
/* 日志开关: 0=关闭(发布版, 编译时 -DMP_LOG=0 不编入日志代码), 1=开启(调试版) */
#ifndef MP_LOG
#define MP_LOG 1
#endif

static char g_dir[512] = {0};
static void parse_packs(const char *s, size_t n);
static void import_titles_sync(void);
static void import_all_packs(void);
static void mp_rmtree(const char *path);
static void pack_cache_name(const char *zip, char *out, size_t cap);
#if MP_LOG
static void mpz_filelog(const char *fmt, ...) {
    if (!g_dir[0]) return;
    char lp[768];
    snprintf(lp, sizeof(lp), "%s/mpz.log", g_dir);

    const long LOG_MAX = 256 * 1024;
    struct stat st;
    long sz = (stat(lp, &st) == 0) ? (long)st.st_size : 0;
    if (sz >= LOG_MAX) {
        FILE *rf = fopen(lp, "rb");
        if (rf) {
            fseek(rf, 0, SEEK_END);
            long total = ftell(rf);
            long keep = LOG_MAX / 2;
            long start = total - keep;
            if (start < 0) start = 0;
            fseek(rf, start, SEEK_SET);
            char *buf = (char *)malloc(keep + 2);
            if (buf) {
                long rd = (long)fread(buf, 1, (size_t)keep, rf);
                buf[rd] = 0;
                char *nl = strchr(buf, '\n');
                FILE *wf = fopen(lp, "wb");
                if (wf) {
                    if (nl) fwrite(nl + 1, 1, strlen(nl + 1), wf);
                    fclose(wf);
                }
                free(buf);
            }
            fclose(rf);
        }
    }
    FILE *f = fopen(lp, "a");
    if (!f) return;
    time_t t = time(0);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char ts[32];
    strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &tmv);
    fprintf(f, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}
extern void (*g_sl_log)(const char *msg);
static void sl_log_bridge(const char *msg) {
    mpz_filelog("[SL] %s", msg);
}
#define LOGI(...)                                                    \
    do {                                                             \
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__); \
        mpz_filelog(__VA_ARGS__);                                    \
    } while (0)
#define LOGE(...)                                                     \
    do {                                                              \
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__); \
        mpz_filelog("ERR " __VA_ARGS__);                              \
    } while (0)
#else
#define LOGI(...) \
    do {          \
    } while (0)
#define LOGE(...) \
    do {          \
    } while (0)
#endif
static const module_info_t g_info = {
    .pkg_id = "eternal.future.audiopackextension",
    .name = "MusicPack Extension",
    .author = "qing",
    .version = "1.2.6",
    .version_code = 126,
    .api_version = 1,
    .plugin_dependencies_sizes = 0,
    .plugin_dependencies = 0,
};
#define MAX_ITEMS 256
typedef struct {
    int enable;
    int music;
    int priority;
    int bad;
    char file[200];
    char pack[200];
} item_t;
typedef struct {
    int enable;
    int type;
    char file[200];
    char pack[200];
} sfx_item_t;
static sfx_item_t g_sfxitems[MAX_ITEMS];
static int g_sfxcount = 0;

static item_t g_items[MAX_ITEMS];
static int g_count = 0;
static char g_playing_file[300] = {0};
static volatile int g_playing_alive = 0;
static volatile int g_cover = 0;
static volatile long g_cover_ms = 0;
static volatile int g_fading = 0;
static volatile int g_release = 0;
static volatile long g_release_ms = 0;
static volatile long g_leave_ms = 0;
static float g_game_vol = 0.0f;
static volatile long g_orig_fade_in_ms = 0; /* 原版淡入开始时间, 0=未淡入 */
static volatile int g_play_fail = 0;
static volatile int g_fail_music = -1;
static volatile int g_cur_music = -1;
static float g_vol_prev = -1.0f;
static int g_vol_same_n = 0;
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}
static const char *find_key(const char *start, const char *limit, const char *key) {
    size_t kl = strlen(key);
    const char *p = start;
    while (p < limit) {
        if (*p == '"') {
            if ((size_t)(limit - (p + 1)) > kl && strncmp(p + 1, key, kl) == 0 && p[1 + kl] == '"') {
                const char *q = skip_ws(p + 1 + kl + 1);
                if (*q == ':') return skip_ws(q + 1);
            }
        }
        p++;
    }
    return 0;
}
static void jstr(const char *p, char *out, int cap) {
    out[0] = 0;
    if (!p || *p != '"') return;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
    out[i] = 0;
}
static void parse_cfg(const char *s, size_t n) {
    const char *p = s;
    const char *end = s + n;
    g_count = 0;
    g_sfxcount = 0;
    while (p < end && (g_count < MAX_ITEMS && g_sfxcount < MAX_ITEMS)) {
        const char *o = strchr(p, '{');
        if (!o) break;
        const char *c = strchr(o, '}');
        if (!c) break;
        const char *e;
        int en = 1, mu = 0, ty = 0;
        char file[200] = {0}, pack[200] = {0};
        e = find_key(o, c, "enable");
        if (e && e < c) en = (strncmp(e, "true", 4) == 0);
        e = find_key(o, c, "music");
        if (e && e < c) mu = atoi(e);
        e = find_key(o, c, "type");
        if (e && e < c) ty = atoi(e);
        e = find_key(o, c, "file");
        if (e && e < c) jstr(e, file, sizeof(file));
        e = find_key(o, c, "pack");
        if (e && e < c) jstr(e, pack, sizeof(pack));
        if (ty > 0 && file[0]) {
            if (g_sfxcount < MAX_ITEMS) {
                sfx_item_t *si = &g_sfxitems[g_sfxcount];
                memset(si, 0, sizeof(*si));
                si->enable = en;
                si->type = ty;
                strncpy(si->file, file, sizeof(si->file) - 1);
                strncpy(si->pack, pack, sizeof(si->pack) - 1);
                g_sfxcount++;
            }
        } else if (mu > 0 && file[0]) {
            if (g_count < MAX_ITEMS) {
                item_t *it = &g_items[g_count];
                memset(it, 0, sizeof(*it));
                it->enable = en;
                it->music = mu;
                strncpy(it->file, file, sizeof(it->file) - 1);
                strncpy(it->pack, pack, sizeof(it->pack) - 1);
                g_count++;
            }
        }
        p = c + 1;
    }
    LOGI("cfg items=%d sfx=%d", g_count, g_sfxcount);
    for (int i = 0; i < g_count; i++) LOGI("  [music] music=%d file=%s", g_items[i].music, g_items[i].file);
    for (int i = 0; i < g_sfxcount; i++) LOGI("  [sfx] type=%d file=%s", g_sfxitems[i].type, g_sfxitems[i].file);
}

static void load_cfg(void) {
    char path[600];
    snprintf(path, sizeof(path), "%s/config.json", g_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOGE("no cfg %s", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 131072) {
        fclose(f);
        return;
    }
    char *b = (char *)malloc(n + 1);
    if (!b) {
        fclose(f);
        return;
    }
    size_t rd = fread(b, 1, n, f);
    b[rd] = 0;
    fclose(f);
    parse_packs(b, rd);
    free(b);
}
static void load_cfg_auto(void) {
    load_cfg();
}

/* ===================== PACK IMPORT (Terraria Workshop 音乐包) ===================== */
#include "lib/miniz.h"

#define MAX_MAP 150
typedef struct {
    int id;
    char path[256];
} mapent_t;
static mapent_t g_map[MAX_MAP];
static int g_map_n = 0;
static pthread_mutex_t g_tblk = PTHREAD_MUTEX_INITIALIZER;
static inline void tbl_lock(void) {
    pthread_mutex_lock(&g_tblk);
}
static inline void tbl_unlock(void) {
    pthread_mutex_unlock(&g_tblk);
}

/* 包列表(来自 config.json): file/enable/priority/type */
#define MAX_PACKS 64
typedef struct {
    int enable;
    int priority;
    int type;
    char file[200];
} pack_t;
static pack_t g_packs[MAX_PACKS];
static int g_pack_n = 0;

/* 解析 TEF 的 config.json(纯 JSON 数组): [{file,enable,priority,type},...] */
static void parse_packs(const char *s, size_t n) {
    const char *p = s;
    const char *end = s + n;
    g_pack_n = 0;
    while (p < end && g_pack_n < MAX_PACKS) {
        const char *o = strchr(p, 0x7b);
        if (!o) break;
        const char *c = strchr(o, 0x7d);
        if (!c || c > end) break;
        const char *e;
        int en = 0, pr = 0, ty = 0;
        char file[200] = {0};
        e = find_key(o, c, "enable");
        if (e && e < c) en = (strncmp(e, "true", 4) == 0);
        e = find_key(o, c, "priority");
        if (e && e < c) pr = atoi(e);
        e = find_key(o, c, "type");
        if (e && e < c) ty = atoi(e);
        e = find_key(o, c, "file");
        if (e && e < c) jstr(e, file, sizeof(file));
        if (file[0]) {
            pack_t *pk = &g_packs[g_pack_n++];
            memset(pk, 0, sizeof(*pk));
            pk->enable = en;
            pk->priority = pr;
            pk->type = ty;
            strncpy(pk->file, file, sizeof(pk->file) - 1);
        }
        p = c + 1;
    }
    LOGI("packs=%d", g_pack_n);
    for (int i = 0; i < g_pack_n; i++) LOGI("  [pack] en=%d pr=%d ty=%d file=%s", g_packs[i].enable, g_packs[i].priority, g_packs[i].type, g_packs[i].file);
}

static void map_add(int id, const char *path) {
    tbl_lock();
    for (int i = 0; i < g_map_n; i++) {
        if (g_map[i].id == id) {
            /* 先到先得: 已存在则保留(不覆盖), 高优先级先加载即占住 */
            tbl_unlock();
            return;
        }
    }
    if (g_map_n < MAX_MAP) {
        g_map[g_map_n].id = id;
        strncpy(g_map[g_map_n].path, path, sizeof(g_map[g_map_n].path) - 1);
        g_map[g_map_n].path[sizeof(g_map[g_map_n].path) - 1] = 0;
        g_map_n++;
    }
    tbl_unlock();
}

/* audio ext check: .ogg .mp3 .wav (case-insensitive) */
static int is_audio_ext(const char *b) {
    if (b[0] != 0x2e) return 0;
    char c1 = b[1] | 0x20, c2 = b[2] | 0x20, c3 = b[3] | 0x20;
    if (c1 == 0x6f && c2 == 0x67 && c3 == 0x67) return 1;
    if (c1 == 0x6d && c2 == 0x70 && c3 == 0x33) return 1;
    if (c1 == 0x77 && c2 == 0x61 && c3 == 0x76 && (b[4] | 0x20) == 0x65) return 1;
    return 0;
}
/* Music_<ID>.<ext> -> ID */
static int parse_music_id(const char *entry) {
    const char *b = strstr(entry, "Music_");
    if (!b) return -1;
    b += 6;
    if (*b < 0x30 || *b > 0x39) return -1;
    int v = 0;
    while (*b >= 0x30 && *b <= 0x39) {
        v = v * 10 + (*b - 0x30);
        b++;
    }
    if (!is_audio_ext(b)) return -1;
    return v;
}
/* Sounds/<digits>.<ext> -> sfx type */
static void sfx_map_add(int type, const char *path);

static int parse_sfx_id(const char *entry) {
    const char *b = strstr(entry, "Sounds/");
    if (!b) return -1;
    b += 7;
    if (*b < 0x30 || *b > 0x39) return -1;
    int v = 0;
    while (*b >= 0x30 && *b <= 0x39) {
        v = v * 10 + (*b - 0x30);
        b++;
    }
    if (!is_audio_ext(b)) return -1;
    return v;
}

/* 扫描已解压 cache 目录, 生成 index.txt (一次读索引, 避免每次启动逐文件 IO) */
static void write_index_from_dir(const char *cdir) {
    DIR *d = opendir(cdir);
    if (!d) return;
    char idxp[920];
    snprintf(idxp, sizeof(idxp), "%s/index.txt", cdir);
    char tmp[940];
    snprintf(tmp, sizeof(tmp), "%s/index.txt.tmp", cdir);
    FILE *o = fopen(tmp, "w");
    if (!o) {
        closedir(d);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *bn = e->d_name;
        if (bn[0] == 0x2e) continue;
        char full[900];
        snprintf(full, sizeof(full), "%s/%s", cdir, bn);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        int id = parse_music_id(bn);
        if (id >= 0) {
            fprintf(o, "M%d %s\n", id, bn);
            continue;
        }
        if (bn[0] == 0x78 && bn[1] == 0x5f) {
            const char *nb = bn + 2;
            const char *dot = strrchr(nb, 0x2e);
            if (!dot) continue;
            if (strcasecmp(dot, ".xnb") != 0) continue;
            char pcm[920];
            snprintf(pcm, sizeof(pcm), "%s.pcm", full);
            char meta[940];
            snprintf(meta, sizeof(meta), "%s.meta", pcm);
            int rate = 0, ch = 0, cb = 0;
            FILE *mf = fopen(meta, "r");
            if (!mf) continue;
            if (fscanf(mf, "%d %d %d", &rate, &ch, &cb) != 3 || rate <= 0) {
                fclose(mf);
                continue;
            }
            fclose(mf);
            char base[256];
            size_t bl = (size_t)(dot - nb);
            if (bl >= sizeof(base)) bl = sizeof(base) - 1;
            memcpy(base, nb, bl);
            base[bl] = 0;
            int xt = -1, xst = -1;
            if (!sfx_name_lookup(base, &xt, &xst)) xt = -1;
            if (xt >= 0) {
                char key[64];
                if (xst >= 0)
                    snprintf(key, sizeof(key), "sfx:%d:%d", xt, xst);
                else
                    snprintf(key, sizeof(key), "sfx:%d", xt);
                fprintf(o, "X%s %s %d %d\n", key, bn, rate, ch);
                if (xt == 2 && xst >= 133 && xst <= 138) {
                    char k2[64];
                    snprintf(k2, sizeof(k2), "sfx:%d", 47 + xst - 133);
                    fprintf(o, "X%s %s %d %d\n", k2, bn, rate, ch);
                }
                if (xt == 2 && xst >= 139 && xst <= 148) {
                    char k2[64];
                    snprintf(k2, sizeof(k2), "sfx:%d", 53 + xst - 139);
                    fprintf(o, "X%s %s %d %d\n", k2, bn, rate, ch);
                }
            }
        } else if (!strncmp(bn, "sfx_", 4)) {
            char base[256];
            snprintf(base, sizeof(base), "%s", bn + 4);
            char *dot = strrchr(base, 0x2e);
            if (dot) *dot = 0;
            int st2 = -1, ss = -1;
            if (base[0] >= 0x30 && base[0] <= 0x39) {
                int v = 0, ok = 1;
                for (char *q = base; *q; q++) {
                    if (*q < 0x30 || *q > 0x39) {
                        ok = 0;
                        break;
                    }
                    v = v * 10 + (*q - 0x30);
                }
                if (ok) st2 = v;
            }
            if (st2 < 0) sfx_name_lookup(base, &st2, &ss);
            if (st2 >= 0) {
                char key[64];
                if (ss >= 0)
                    snprintf(key, sizeof(key), "sfx:%d:%d", st2, ss);
                else
                    snprintf(key, sizeof(key), "sfx:%d", st2);
                fprintf(o, "S%s %s\n", key, bn);
            }
        }
    }
    closedir(d);
    fclose(o);
    rename(tmp, idxp);
}

/* 从 index.txt 快速重建 (一次读文件); 成功返回1, 无索引返回0 */
static int rebuild_from_index(const char *cdir) {
    char idxp[920];
    snprintf(idxp, sizeof(idxp), "%s/index.txt", cdir);
    FILE *f = fopen(idxp, "r");
    if (!f) return 0;
    char line[1200];
    while (fgets(line, sizeof(line), f)) {
        char full[960];
        if (line[0] == 'M') {
            int id = 0;
            char bn[256] = {0};
            if (sscanf(line + 1, "%d %255s", &id, bn) != 2) continue;
            snprintf(full, sizeof(full), "%s/%s", cdir, bn);
            map_add(id, full);
        } else if (line[0] == 'X') {
            char key[64] = {0}, bn[256] = {0};
            int rate = 0, ch = 0;
            if (sscanf(line + 1, "%63s %255s %d %d", key, bn, &rate, &ch) != 4) continue;
            snprintf(full, sizeof(full), "%s/%s", cdir, bn);
            char pcm[980];
            snprintf(pcm, sizeof(pcm), "%s.pcm", full);
            if (sl_init()) sl_register(key, pcm, rate, ch);
        } else if (line[0] == 'S') {
            char key[64] = {0}, bn[256] = {0};
            if (sscanf(line + 1, "%63s %255s", key, bn) != 2) continue;
            int t = 0, s2 = -1;
            if (sscanf(key, "sfx:%d:%d", &t, &s2) == 1) s2 = -1;
            snprintf(full, sizeof(full), "%s/%s", cdir, bn);
            sfx_map_add(t, full);
        }
    }
    fclose(f);
    return 1;
}

/* 从已解压 cache 目录重建映射表, 跳过解压 */
static void rebuild_from_cache(const char *cdir, int pidx) {
    /* 优先走 index.txt 快路径 (一次读全部记录) */
    if (rebuild_from_index(cdir)) {
        LOGI("rebuild cache p%d: via index.txt", pidx);
        return;
    }
    DIR *d = opendir(cdir);
    if (!d) return;
    struct dirent *e;
    int gm = 0, gs = 0;
    while ((e = readdir(d))) {
        const char *bn = e->d_name;
        if (bn[0] == 0x2e) continue;
        char full[900];
        snprintf(full, sizeof(full), "%s/%s", cdir, bn);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        int id = parse_music_id(bn);
        if (id >= 0) {
            map_add(id, full);
            gm++;
            continue;
        }
        if (bn[0] == 0x78 && bn[1] == 0x5f) {
            const char *nb = bn + 2;
            const char *dot = strrchr(nb, 0x2e);
            if (!dot) continue;
            if (strcasecmp(dot, ".xnb") != 0) continue;
            char pcm[920];
            snprintf(pcm, sizeof(pcm), "%s.pcm", full);
            char meta[940];
            snprintf(meta, sizeof(meta), "%s.meta", pcm);
            int rate = 0, ch = 0, cb = 0;
            FILE *mf = fopen(meta, "r");
            if (!mf) continue;
            if (fscanf(mf, "%d %d %d", &rate, &ch, &cb) != 3 || rate <= 0) {
                fclose(mf);
                continue;
            }
            fclose(mf);
            char base[256];
            size_t bl = (size_t)(dot - nb);
            if (bl >= sizeof(base)) bl = sizeof(base) - 1;
            memcpy(base, nb, bl);
            base[bl] = 0;
            int xt = -1, xst = -1;
            if (!sfx_name_lookup(base, &xt, &xst)) {
                xt = -1;
            }
            if (xt >= 0 && sl_init()) {
                char key[64];
                if (xst >= 0)
                    snprintf(key, sizeof(key), "sfx:%d:%d", xt, xst);
                else
                    snprintf(key, sizeof(key), "sfx:%d", xt);
                if (sl_register(key, pcm, rate, ch)) gs++;
                if (xt == 2 && xst >= 133 && xst <= 138) {
                    char k2[64];
                    snprintf(k2, sizeof(k2), "sfx:%d", 47 + xst - 133);
                    sl_register(k2, pcm, rate, ch);
                }
                if (xt == 2 && xst >= 139 && xst <= 148) {
                    char k2[64];
                    snprintf(k2, sizeof(k2), "sfx:%d", 53 + xst - 139);
                    sl_register(k2, pcm, rate, ch);
                }
            }
        } else if (!strncmp(bn, "sfx_", 4)) {
            char base[256];
            snprintf(base, sizeof(base), "%s", bn + 4);
            char *dot = strrchr(base, 0x2e);
            if (dot) *dot = 0;
            int st2 = -1, ss = -1;
            if (base[0] >= 0x30 && base[0] <= 0x39) {
                int v = 0, ok = 1;
                for (char *q = base; *q; q++) {
                    if (*q < 0x30 || *q > 0x39) {
                        ok = 0;
                        break;
                    }
                    v = v * 10 + (*q - 0x30);
                }
                if (ok) st2 = v;
            }
            if (st2 < 0) sfx_name_lookup(base, &st2, &ss);
            if (st2 >= 0) {
                sfx_map_add(st2, full);
                gs++;
            }
        }
    }
    closedir(d);
    /* 老扫描完成后补建 index.txt, 下次启动即可走快路径 */
    write_index_from_dir(cdir);
    LOGI("rebuild cache p%d: music=%d sfx=%d (index built)", pidx, gm, gs);
}

/* 解压单个包的 Content/Music/Music_<ID>.ogg 到 cache 目录并登记映射 */
static void import_pack(const char *zipname, int pidx) {
    char zpath[700];
    snprintf(zpath, sizeof(zpath), "%s/audio_packs/%s", g_dir, zipname);
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zpath, 0)) {
        LOGE("zip open fail: %s", zpath);
        return;
    }
    char cdirname[300];
    pack_cache_name(zipname, cdirname, sizeof(cdirname));
    char cdir[700];
    snprintf(cdir, sizeof(cdir), "%s/cache/%s", g_dir, cdirname);
    mp_mkdirs(cdir);

    /* 已解压判断: .src(zip名+size+mtime) 匹配且 .done 存在 -> 从 cache 重建, 跳过解压 */
    {
        char sp[800];
        snprintf(sp, sizeof(sp), "%s/.src", cdir);
        char oldname[256] = {0};
        long long oldsz = -1;
        long long oldmt = -1;
        FILE *rf = fopen(sp, "r");
        if (rf) {
            char l1[256] = {0}, l2[64] = {0}, l3[64] = {0};
            if (fgets(l1, sizeof(l1), rf)) {
                l1[strcspn(l1, "\r\n")] = 0;
                snprintf(oldname, sizeof(oldname), "%s", l1);
            }
            if (fgets(l2, sizeof(l2), rf)) {
                oldsz = atoll(l2);
            }
            if (fgets(l3, sizeof(l3), rf)) {
                oldmt = atoll(l3);
            }
            fclose(rf);
        }
        char zp2[700];
        snprintf(zp2, sizeof(zp2), "%s/audio_packs/%s", g_dir, zipname);
        struct stat zs;
        long long csz = -1, cmt = -1;
        if (stat(zp2, &zs) == 0) {
            csz = (long long)zs.st_size;
            cmt = (long long)zs.st_mtime;
        }
        char dn[820];
        snprintf(dn, sizeof(dn), "%s/.done", cdir);
        struct stat ds;
        int hasdone = (stat(dn, &ds) == 0);
        int match = hasdone && strcmp(oldname, zipname) == 0 && oldsz == csz && oldmt == cmt && csz > 0;
        if (match) {
            mz_zip_reader_end(&zip);
            rebuild_from_cache(cdir, pidx);
            return;
        }
        FILE *sf = fopen(sp, "w");
        if (sf) {
            fprintf(sf, "%s\n%lld\n%lld\n", zipname, csz, cmt);
            fclose(sf);
        }
    }

    mz_uint num = mz_zip_reader_get_num_files(&zip);
    int got = 0, gotsfx = 0;
    int exfail = 0;
    for (mz_uint i = 0; i < num; i++) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        char name[512];
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        const char *sl = strrchr(name, 0x2f);
        const char *base = sl ? (sl + 1) : name;
        int id = parse_music_id(name);
        if (id >= 0) {
            char dst[800];
            snprintf(dst, sizeof(dst), "%s/%s", cdir, base);
            struct stat es;
            if (stat(dst, &es) == 0 && es.st_size > 0) {
                /* 已存在 -> 不重写(防打断播放), 直接登记 */
                map_add(id, dst);
                continue;
            }
            if (mz_zip_reader_extract_to_file(&zip, i, dst, 0)) {
                map_add(id, dst);
                got++;
                LOGI("pack%d: music id=%d -> %s", pidx, id, dst);
            } else {
                exfail++;
                LOGE("pack%d: music extract fail id=%d", pidx, id);
            }
            continue;
        }
        if (strstr(name, "Sounds/") && (strstr(name, ".xnb") || strstr(name, ".XNB"))) {
            char dst[800];
            snprintf(dst, sizeof(dst), "%s/x_%s", cdir, base);
            if (mz_zip_reader_extract_to_file(&zip, i, dst, 0)) {
                char pcm[820];
                snprintf(pcm, sizeof(pcm), "%s.pcm", dst);
                char pmeta[840];
                snprintf(pmeta, sizeof(pmeta), "%s.meta", pcm);
                int xr = 0, xc = 0, xb = 0;
                FILE *cm = fopen(pmeta, "r");
                if (cm) {
                    if (fscanf(cm, "%d %d %d", &xr, &xc, &xb) != 3) xr = 0;
                    fclose(cm);
                }
                if (xr <= 0) {
                    xnb_sound_t xs;
                    char xerr[128];
                    if (xnb_parse_sound(dst, &xs, xerr, sizeof(xerr))) {
                        unsigned char *xp = 0;
                        int rb = 0;
                        if (xnb_normalize_pcm16(&xs, &xp, &rb, &xr, &xc, xerr, sizeof(xerr))) {
                            FILE *pf = fopen(pcm, "wb");
                            if (pf) {
                                fwrite(xp, 1, rb, pf);
                                fclose(pf);
                            }
                            FILE *mf = fopen(pmeta, "w");
                            if (mf) {
                                fprintf(mf, "%d %d %d", xr, xc, rb);
                                fclose(mf);
                            }
                            xb = rb;
                        } else
                            xr = 0;
                        free(xp);
                        xnb_sound_free(&xs);
                    } else {
                        LOGE("pack%d: xnb parse fail %s (%s)", pidx, base, xerr);
                        xr = 0;
                    }
                }
                if (xr > 0) {
                    char pcmdst[820];
                    snprintf(pcmdst, sizeof(pcmdst), "%s", pcm);
                    int xt = -1, xst = -1;
                    if (!sfx_name_lookup(base, &xt, &xst)) xt = -1;
                    if (xt >= 0 && sl_init()) {
                        char xkey[64];
                        if (xst >= 0)
                            snprintf(xkey, sizeof(xkey), "sfx:%d:%d", xt, xst);
                        else
                            snprintf(xkey, sizeof(xkey), "sfx:%d", xt);
                        if (sl_register(xkey, pcmdst, xr, xc)) {
                            gotsfx++;
                            LOGI("pack%d: xnb sfx t=%d s=%d -> %s", pidx, xt, xst, base);
                        }
                        if (xt == 2 && xst >= 133 && xst <= 138) {
                            char k2[64];
                            snprintf(k2, sizeof(k2), "sfx:%d", 47 + xst - 133);
                            sl_register(k2, pcmdst, xr, xc);
                        }
                        if (xt == 2 && xst >= 139 && xst <= 148) {
                            char k2[64];
                            snprintf(k2, sizeof(k2), "sfx:%d", 53 + xst - 139);
                            sl_register(k2, pcmdst, xr, xc);
                        }
                    }
                }
            } else {
                exfail++;
                LOGE("pack%d: xnb extract fail %s", pidx, base);
            }
            continue;
        }
        int st = parse_sfx_id(name);
        int sst = -1;
        if (st < 0) {
            int nt = -1, ns = -1;
            if (sfx_name_lookup(base, &nt, &ns)) {
                st = nt;
                sst = ns;
            }
        }
        if (st >= 0) {
            char dst[800];
            snprintf(dst, sizeof(dst), "%s/sfx_%s", cdir, base);
            if (mz_zip_reader_extract_to_file(&zip, i, dst, 0)) {
                sfx_map_add(st, dst);
                gotsfx++;
                LOGI("pack%d: sfx type=%d s=%d -> %s", pidx, st, sst, dst);
            } else {
                exfail++;
                LOGE("pack%d: sfx extract fail type=%d", pidx, st);
            }
            continue;
        }
    }
    mz_zip_reader_end(&zip);
    if (exfail == 0) {
        char dn[820];
        snprintf(dn, sizeof(dn), "%s/.done", cdir);
        FILE *df = fopen(dn, "w");
        if (df) {
            fprintf(df, "%d %d", got, gotsfx);
            fclose(df);
        }
        write_index_from_dir(cdir);
    }
    LOGI("pack%d done music=%d sfx=%d exfail=%d", pidx, got, gotsfx, exfail);
}

/* init 同步专用: 只把标题曲(6/50/51/60)解压到该包的 cache 文件夹, 已存在则跳过(不重写) */
static void import_pack_titles(const char *zipname) {
    char zpath[700];
    snprintf(zpath, sizeof(zpath), "%s/audio_packs/%s", g_dir, zipname);
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zpath, 0)) {
        LOGE("title zip open fail: %s", zpath);
        return;
    }
    char cdirname[300];
    pack_cache_name(zipname, cdirname, sizeof(cdirname));
    char cdir[700];
    snprintf(cdir, sizeof(cdir), "%s/cache/%s", g_dir, cdirname);
    mp_mkdirs(cdir); /* 只建, 不删 */
    mz_uint num = mz_zip_reader_get_num_files(&zip);
    int got = 0;
    for (mz_uint i = 0; i < num; i++) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        char name[512];
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        int id = parse_music_id(name);
        if (id != 6 && id != 50 && id != 51 && id != 60) continue;
        const char *sl = strrchr(name, 0x2f);
        const char *base = sl ? (sl + 1) : name;
        char dst[800];
        snprintf(dst, sizeof(dst), "%s/%s", cdir, base);
        struct stat ds;
        if (stat(dst, &ds) == 0) { /* 已存在 -> 跳过(不重写, 防打断播放) */
            map_add(id, dst);
            continue;
        }
        if (mz_zip_reader_extract_to_file(&zip, i, dst, 0)) {
            map_add(id, dst);
            got++;
            LOGI("title id=%d -> %s", id, dst);
        } else {
            LOGE("title extract fail id=%d", id);
        }
    }
    mz_zip_reader_end(&zip);
    LOGI("title pack done (%s) music=%d", zipname, got);
}

static int map_lookup(int id, char *out, int cap) {
    tbl_lock();
    for (int i = 0; i < g_map_n; i++) {
        if (g_map[i].id == id) {
            snprintf(out, cap, "%s", g_map[i].path);
            tbl_unlock();
            return 1;
        }
    }
    tbl_unlock();
    return 0;
}

/* 扫 audio_packs/ 目录, 把所有 zip 加入 g_packs (不依赖 config.json) */
#include <dirent.h>
static void scan_packs_dir(void) {
    char dp[640];
    snprintf(dp, sizeof(dp), "%s/audio_packs", g_dir);
    DIR *d = opendir(dp);
    if (!d) {
        LOGE("no audio_packs dir %s", dp);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) && g_pack_n < MAX_PACKS) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        if (l < 5) continue;
        char c1 = nm[l - 1] | 0x20, c2 = nm[l - 2] | 0x20, c3 = nm[l - 3] | 0x20;
        if (!(c3 == 0x7a && c2 == 0x69 && c1 == 0x70)) continue; /* .zip */
        pack_t *pk = &g_packs[g_pack_n++];
        memset(pk, 0, sizeof(*pk));
        pk->enable = 1;
        pk->priority = 0;
        pk->type = 0;
        strncpy(pk->file, nm, sizeof(pk->file) - 1);
        LOGI("scan zip: %s", nm);
    }
    closedir(d);
    LOGI("scan_packs_dir total=%d", g_pack_n);
}

/* 递归删除目录 */
/* 由压缩包名得到 cache 目录名 (去掉尾部 .zip, 过滤非法字符) */
static void pack_cache_name(const char *zip, char *out, size_t cap) {
    size_t wr = 0;
    for (size_t i = 0; zip[i] && wr + 1 < cap; i++) {
        char c = zip[i];
        if (c == '/' || c == '\\') continue;
        out[wr++] = c;
    }
    if (wr >= 4) {
        if ((out[wr-1]|0x20)=='p' && (out[wr-2]|0x20)=='i' && (out[wr-3]|0x20)=='z' && out[wr-4]=='.') wr -= 4;
    }
    out[wr] = 0;
    if (wr == 0) snprintf(out, cap, "_pkg");
}

static void mp_rmtree(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char sub[900];
            snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
            struct stat st;
            if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode))
                mp_rmtree(sub);
            else
                unlink(sub);
        }
        closedir(d);
    }
    rmdir(path);
}

static void clean_orphan_cache(void) {
    char croot[700];
    snprintf(croot, sizeof(croot), "%s/cache", g_dir);
    DIR *d = opendir(croot);
    if (!d) return;
    if (g_pack_n == 0) scan_packs_dir();
    struct dirent *e;
    int removed = 0;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char cdir[800];
        snprintf(cdir, sizeof(cdir), "%s/%s", croot, e->d_name);
        struct stat st;
        if (stat(cdir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        /* 保留: 目录名 == 当前某个包的 cache 名 */
        int keep = 0;
        for (int i = 0; i < g_pack_n; i++) {
            char cn[300];
            pack_cache_name(g_packs[i].file, cn, sizeof(cn));
            if (strcmp(cn, e->d_name) == 0) { keep = 1; break; }
        }
        if (!keep) {
            LOGI("clean orphan cache: %s", cdir);
            mp_rmtree(cdir);
            removed++;
        }
    }
    closedir(d);
    if (removed) LOGI("clean_orphan_cache removed=%d", removed);
}

/* 全部导入: 过滤 enable+type0, 按 priority 升序, 逐包解压 */
/* 包内是否含标题曲 (Music_6/50/51/60) */
static int zip_has_title(const char *zipname) {
    char zpath[700];
    snprintf(zpath, sizeof(zpath), "%s/audio_packs/%s", g_dir, zipname);
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zpath, 0)) return 0;
    mz_uint num = mz_zip_reader_get_num_files(&zip);
    int found = 0;
    for (mz_uint i = 0; i < num; i++) {
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        char name[512];
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        int id = parse_music_id(name);
        if (id == 6 || id == 50 || id == 51 || id == 60) { found = 1; break; }
    }
    mz_zip_reader_end(&zip);
    return found;
}

/* titles_only=1: 只导含标题曲的包(init 同步用); =0: 导全部 */
/* mode=0: 导入全部(后台); mode=2: 只导含标题曲的包, 且只解压标题曲(init 同步) */
static void import_filtered(int mode) {
    if (g_pack_n == 0) scan_packs_dir();
    int idx[MAX_PACKS];
    int tit[MAX_PACKS];
    int nn = 0;
    for (int i = 0; i < g_pack_n; i++) {
        if (!g_packs[i].enable) continue;
        if (g_packs[i].type != 0) {
            LOGI("skip pack type=%d file=%s", g_packs[i].type, g_packs[i].file);
            continue;
        }
        idx[nn] = i;
        tit[nn] = zip_has_title(g_packs[i].file);
        nn++;
    }
    /* 排序: 含标题曲的包在前; priority 升序(数字小的先加载) -> 先到先得即小数字占住 */
    for (int a = 0; a < nn; a++)
        for (int b = a + 1; b < nn; b++) {
            int swap = 0;
            if (tit[b] > tit[a])
                swap = 1;
            else if (tit[b] == tit[a] && g_packs[idx[b]].priority < g_packs[idx[a]].priority)
                swap = 1;
            if (swap) {
                int t = idx[a]; idx[a] = idx[b]; idx[b] = t;
                t = tit[a]; tit[a] = tit[b]; tit[b] = t;
            }
        }
    for (int a = 0; a < nn; a++) {
        int i = idx[a];
        if (mode == 2) {
            if (!tit[a]) continue; /* 只处理含标题曲的包 */
            LOGI("titleimport[%d] pr=%d title=%d file=%s", a, g_packs[i].priority, tit[a], g_packs[i].file);
            import_pack_titles(g_packs[i].file);
        } else {
            LOGI("import[%d] pr=%d title=%d file=%s", a, g_packs[i].priority, tit[a], g_packs[i].file);
            import_pack(g_packs[i].file, i);
        }
    }
}

/* init 同步: 只解压标题曲(6/50/51/60), 保证进主菜单即可放替换曲 */
static void import_titles_sync(void) {
    g_map_n = 0;
    clean_orphan_cache();
    import_filtered(2);
    LOGI("=== title map total=%d ===", g_map_n);
    for (int i = 0; i < g_map_n; i++) LOGI("  TMAP id=%d %s", g_map[i].id, g_map[i].path);
}

/* 后台: 补齐其余所有包 (已在 map 里的不覆盖, 先到先得) */
static void import_all_packs(void) {
    import_filtered(0);
    LOGI("=== map total=%d ===", g_map_n);
    for (int i = 0; i < g_map_n; i++) LOGI("  MAP id=%d %s", g_map[i].id, g_map[i].path);
}

static JavaVM *g_vm = 0;
static jobject g_player = 0;
static jobject g_fade_player = 0; /* 淡出专用 global ref，与 g_player 同步 */
static int g_active = -1;
static int g_pend_idx = -1; /* 淡出完要接着播的新曲 idx (-1=无) */
static char g_pend_hit[360] = {0};
/* --- g_player 交接锁：所有对 g_player 的取/置必须走 pl_take/pl_install/pl_ref --- */
static pthread_mutex_t g_plk = PTHREAD_MUTEX_INITIALIZER;
/* 加锁读取当前 player，返回一个新的局部引用(可为 0)；调用方负责 DeleteLocalRef */
static jobject pl_ref(JNIEnv *e) {
    pthread_mutex_lock(&g_plk);
    jobject p = g_player;
    if (p) p = (*e)->NewLocalRef(e, p);
    pthread_mutex_unlock(&g_plk);
    return p;
}
/* 取走当前 player 所有权并置空(返回可能是 0)；调用方负责 stop/release/DeleteGlobalRef */
static jobject pl_take(void) {
    pthread_mutex_lock(&g_plk);
    jobject p = g_player;
    g_player = 0;
    g_fade_player = 0;
    pthread_mutex_unlock(&g_plk);
    return p;
}
/* 安装新 player(globalref)，返回被顶替的旧 player；调用方负责关掉旧值 */
static jobject pl_install(jobject np) {
    pthread_mutex_lock(&g_plk);
    jobject old = g_player;
    g_player = np;
    g_fade_player = np;
    pthread_mutex_unlock(&g_plk);
    return old;
}
const char *g_pack_sel = 0;
static JNIEnv *jenv(int *det) {
    if (!g_vm) return 0;
    JNIEnv *e = 0;
    *det = 0;
    int r = (*g_vm)->GetEnv(g_vm, (void **)&e, JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &e, 0) != JNI_OK) return 0;
        *det = 1;
    } else if (r != JNI_OK)
        return 0;
    return e;
}
static void jdet(int d) {
    if (d && g_vm) (*g_vm)->DetachCurrentThread(g_vm);
}
static void light_stop(void) {
    jobject pl = pl_take();
    if (!pl) return;
    int d = 0;
    JNIEnv *e = jenv(&d);
    if (!e) {
        pl_install(pl);
        return;
    }
    jclass c = (*e)->GetObjectClass(e, pl);
    if (c) {
        jmethodID st = (*e)->GetMethodID(e, c, "stop", "()V");
        if (st) (*e)->CallVoidMethod(e, pl, st);
        jmethodID rl = (*e)->GetMethodID(e, c, "release", "()V");
        if (rl) (*e)->CallVoidMethod(e, pl, rl);
        (*e)->DeleteLocalRef(e, c);
    }
    if ((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e);
    (*e)->DeleteGlobalRef(e, pl);
    g_playing_file[0] = 0;
    g_playing_alive = 0;
    jdet(d);
    LOGI("light_stop");
}
static void stop_player(void) {
    jobject pl = pl_take();
    if (!pl) return;
    int d = 0;
    JNIEnv *e = jenv(&d);
    if (!e) {
        pl_install(pl);
        return;
    }
    jclass c = (*e)->GetObjectClass(e, pl);
    if (c) {
        jmethodID st = (*e)->GetMethodID(e, c, "stop", "()V");
        jmethodID rl = (*e)->GetMethodID(e, c, "release", "()V");
        if (st) (*e)->CallVoidMethod(e, pl, st);
        if (rl) (*e)->CallVoidMethod(e, pl, rl);
        (*e)->DeleteLocalRef(e, c);
    }
    if ((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e);
    (*e)->DeleteGlobalRef(e, pl);
    g_playing_file[0] = 0;
    g_playing_alive = 0;
    jdet(d);
}
static void play_ogg(const char *file);
static char g_pend_file[300] = {0};
static pthread_t g_play_thread;
static void *play_thread_fn(void *a) {
    (void)a;
    char f[300];
    strncpy(f, g_pend_file, sizeof(f) - 1);
    f[sizeof(f) - 1] = 0;
    play_ogg(f);
    return 0;
}
static void update_player_vol(void) {
    if (g_fading || !g_playing_alive) return;
    int d = 0;
    JNIEnv *e = jenv(&d);
    if (!e) return;
    jobject pl = pl_ref(e);
    if (pl) {
        jclass c = (*e)->GetObjectClass(e, pl);
        if (c) {
            jmethodID sv = (*e)->GetMethodID(e, c, "setVolume", "(FF)V");
            if (sv) {
                float vv = g_game_vol > 0.0f ? g_game_vol : 0.75f;
                if (vv > 1.0f) vv = 1.0f;
                (*e)->CallVoidMethod(e, pl, sv, vv, vv);
                if ((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e);
            }
            (*e)->DeleteLocalRef(e, c);
        }
        (*e)->DeleteLocalRef(e, pl);
    }
    jdet(d);
}
static void play_ogg_async(const char *file, int idx) {
    LOGI("play_ogg_async file=%s idx=%d", file, idx);
    g_cur_music = idx;
    g_play_fail = 0;
    if (g_playing_alive && g_playing_file[0] && strcmp(g_playing_file, file) == 0 && !g_fading) {
        LOGI("SKIP replay same file %s", file);
        return;
    }
    g_playing_alive = 1;
    strncpy(g_playing_file, file, sizeof(g_playing_file) - 1);
    g_playing_file[sizeof(g_playing_file) - 1] = 0;
    strncpy(g_pend_file, file, sizeof(g_pend_file) - 1);
    g_pend_file[sizeof(g_pend_file) - 1] = 0;
    if (pthread_create(&g_play_thread, 0, play_thread_fn, 0) == 0) pthread_detach(g_play_thread);
}
static int g_bg_paused = 0;
static void pause_player(void) {
    if (g_bg_paused) return;
    int d = 0;
    JNIEnv *e = jenv(&d);
    if (!e) return;
    jobject pl = pl_ref(e);
    if (pl) {
        jclass c = (*e)->GetObjectClass(e, pl);
        if (c) {
            jmethodID pa = (*e)->GetMethodID(e, c, "pause", "()V");
            if (pa) {
                (*e)->CallVoidMethod(e, pl, pa);
                if ((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e);
                g_bg_paused = 1;
                LOGI("BG pause");
            }
            (*e)->DeleteLocalRef(e, c);
        }
        (*e)->DeleteLocalRef(e, pl);
    }
    jdet(d);
}
static void resume_player(void) {
    if (!g_bg_paused) return;
    int d = 0;
    JNIEnv *e = jenv(&d);
    if (!e) return;
    jobject pl = pl_ref(e);
    if (pl) {
        jclass c = (*e)->GetObjectClass(e, pl);
        if (c) {
            jmethodID st = (*e)->GetMethodID(e, c, "start", "()V");
            if (st) {
                (*e)->CallVoidMethod(e, pl, st);
                if ((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e);
            }
            (*e)->DeleteLocalRef(e, c);
        }
        (*e)->DeleteLocalRef(e, pl);
    }
    g_bg_paused = 0;
    LOGI("BG resume");
    jdet(d);
}
static long now_ms(void);
static volatile int g_fadein_running = 0;
static pthread_t g_fadein_thread;
static int g_fadein_ms = 800;
static void *fadein_thread_fn(void *a) {
    (void)a;
    int d = 0;
    JNIEnv *e = jenv(&d);
    if (!e) {
        g_fadein_running = 0;
        return 0;
    }
    jobject pl = pl_ref(e);
    if (!pl) {
        jdet(d);
        g_fadein_running = 0;
        return 0;
    }
    jclass c = (*e)->GetObjectClass(e, pl);
    jmethodID sv = c ? (*e)->GetMethodID(e, c, "setVolume", "(FF)V") : 0;
    /* 800ms: 40 步 x 20ms */
    int steps = 40;
    for (int i = 0; i <= steps && g_fadein_running; i++) {
        float t = (float)i / (float)steps;
        float base = g_game_vol > 0.0f ? g_game_vol : 0.75f;
        if (base > 1.0f) base = 1.0f;
        float vv = base * t;
        if (sv) (*e)->CallVoidMethod(e, pl, sv, vv, vv);
        if ((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e);
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 20000000L;
        nanosleep(&ts, 0);
    }
    if (sv) {
        float base = g_game_vol > 0.0f ? g_game_vol : 0.75f;
        if (base > 1.0f) base = 1.0f;
        (*e)->CallVoidMethod(e, pl, sv, base, base);
        if ((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e);
    }
    if (c) (*e)->DeleteLocalRef(e, c);
    (*e)->DeleteLocalRef(e, pl);
    jdet(d);
    g_fadein_running = 0;
    return 0;
}
static void start_fadein(void) {
    if (g_fadein_running) g_fadein_running = 0;
    g_fadein_running = 1;
    if (pthread_create(&g_fadein_thread, 0, fadein_thread_fn, 0) != 0)
        g_fadein_running = 0;
    else
        pthread_detach(g_fadein_thread);
}
static long long g_fade_start_ms = 0;
static volatile int g_fade_running = 0;
static pthread_t g_fade_thread;
static void *fade_thread_fn(void *a) {
    (void)a;
    int d = 0;
    JNIEnv *e = jenv(&d);
    if (!e) {
        g_fade_running = 0;
        return 0;
    }
    jobject pl = 0;
    {
        pthread_mutex_lock(&g_plk);
        pl = g_fade_player;
        pthread_mutex_unlock(&g_plk);
    }
    jmethodID sv = 0;
    if (pl) {
        jclass c = (*e)->GetObjectClass(e, pl);
        if (c) {
            sv = (*e)->GetMethodID(e, c, "setVolume", "(FF)V");
            (*e)->DeleteLocalRef(e, c);
        }
    }
    int v = 1000;
    LOGI("FADE begin player=%p", (void *)pl);
    /* 淡出约 3.5s: 250 步 x 14ms */
    while (v > 0 && g_fade_running) {
        v -= 4;
        if (v < 0) v = 0;
        if (pl && sv) {
            float fbase = g_game_vol > 0.0f ? g_game_vol : 0.75f;
            float fv = (float)v / 1000.0f * fbase;
            (*e)->CallVoidMethod(e, pl, sv, fv, fv);
            if ((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e);
        }
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 14000000L;
        nanosleep(&ts, 0);
    }
    LOGI("FADE end v=%d running=%d", v, g_fade_running);
    jdet(d);
    if (v <= 0) {
        light_stop();
        if (g_pend_idx != -1) {
            int pi = g_pend_idx;
            char ph[360];
            strncpy(ph, g_pend_hit, sizeof(ph) - 1);
            ph[sizeof(ph) - 1] = 0;
            g_pend_idx = -1;
            g_pend_hit[0] = 0;
            g_active = -1;
            LOGI("fade done -> play pend idx=%d", pi);
            char ts[16];
            snprintf(ts, sizeof(ts), "%d", pi);
            play_ogg_async(ts, pi);
        } else {
            g_active = -1;
            g_cover = 0;
            g_release = 1;
            g_release_ms = now_ms();
            g_orig_fade_in_ms = now_ms();
        }
    }
    g_fading = 0;
    g_fade_running = 0;
    return 0;
}
static void start_fade(void) {
    int d0 = 0;
    JNIEnv *e0 = jenv(&d0);
    jobject chk = e0 ? pl_ref(e0) : 0;
    LOGI("start_fade fading=%d player=%p ref=%p", g_fading, (void *)g_player, (void *)chk);
    if (chk && e0) (*e0)->DeleteLocalRef(e0, chk);
    if (d0) jdet(d0);
    if (!g_player) {
        g_active = -1;
        g_cover = 0;
        g_release = 1;
        g_release_ms = now_ms();
        g_orig_fade_in_ms = now_ms();
        return;
    }
    if (g_fading) {
        g_fade_start_ms = now_ms();
        g_cover_ms = now_ms();
        return;
    }
    g_fading = 1;
    g_fade_start_ms = now_ms();
    g_fade_running = 1;
    int r = pthread_create(&g_fade_thread, 0, fade_thread_fn, 0);
    if (r != 0) {
        g_fading = 0;
        g_fade_running = 0;
    } else
        pthread_detach(g_fade_thread);
}
static volatile long g_last_hook_ms = 0;
static int g_wd_running = 0;
static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
static void *watchdog_fn(void *a) {
    (void)a;
    while (g_wd_running) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 300000000L;
        nanosleep(&ts, 0);
        long now = now_ms();
        if (g_last_hook_ms > 0 && now - g_last_hook_ms > 500 && g_player && !g_bg_paused) {
            pause_player();
        }
    }
    return 0;
}
static void start_watchdog(void) {
    if (g_wd_running) return;
    g_wd_running = 1;
    pthread_t t;
    if (pthread_create(&t, 0, watchdog_fn, 0) == 0) pthread_detach(t);
}
static char g_cache_dir[600] = {0};
static void mp_cache_dir_init(void) {
    snprintf(g_cache_dir, sizeof(g_cache_dir), "%s/cache", g_dir);
    mp_mkdirs(g_cache_dir);
}

static int resolve_audio(const char *file, const char *pack, char *out, int cap);
/* ---------- file verify ---------- */
static int probe_file(const char *file) {
    int d = 0;
    JNIEnv *e = jenv(&d);
    if (!e) return 1;
    jclass c = (*e)->FindClass(e, "android/media/MediaPlayer");
    if (!c) {
        jdet(d);
        return 1;
    }
    jmethodID ctor = (*e)->GetMethodID(e, c, "<init>", "()V");
    jmethodID sd = (*e)->GetMethodID(e, c, "setDataSource", "(Ljava/lang/String;)V");
    jmethodID pr = (*e)->GetMethodID(e, c, "prepare", "()V");
    jmethodID rl = (*e)->GetMethodID(e, c, "release", "()V");
    if (!ctor || !sd || !pr) {
        (*e)->DeleteLocalRef(e, c);
        jdet(d);
        return 1;
    }
    char full[768];
    resolve_audio(file, g_pack_sel, full, sizeof(full));
    jobject mp = (*e)->NewObject(e, c, ctor);
    if (!mp) {
        (*e)->DeleteLocalRef(e, c);
        jdet(d);
        return 1;
    }
    int ok = 0;
    jstring js = (*e)->NewStringUTF(e, full);
    (*e)->CallVoidMethod(e, mp, sd, js);
    if (!(*e)->ExceptionOccurred(e)) {
        (*e)->CallVoidMethod(e, mp, pr);
        if (!(*e)->ExceptionOccurred(e))
            ok = 1;
        else
            (*e)->ExceptionClear(e);
    } else
        (*e)->ExceptionClear(e);
    if (rl) (*e)->CallVoidMethod(e, mp, rl);
    if ((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e);
    (*e)->DeleteLocalRef(e, js);
    (*e)->DeleteLocalRef(e, mp);
    (*e)->DeleteLocalRef(e, c);
    jdet(d);
    return ok;
}
static volatile int g_verify_running = 0;
static void *verify_thread_fn(void *a) {
    (void)a;
    LOGI("VERIFY start n=%d", g_count);
    int badn = 0;
    for (int i = 0; i < g_count; i++) {
        if (!g_verify_running) break;
        if (!g_items[i].enable || !g_items[i].file[0]) continue;
        g_items[i].bad = probe_file(g_items[i].file) ? 0 : 1;
        if (g_items[i].bad) badn++;
        LOGI("VERIFY i=%d music=%d bad=%d file=%s", i, g_items[i].music, g_items[i].bad, g_items[i].file);
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 150000000L;
        nanosleep(&ts, 0);
    }
    LOGI("VERIFY done bad=%d", badn);
    g_verify_running = 0;
    return 0;
}
static void start_verify_all(void) {
    if (g_verify_running) return;
    g_verify_running = 1;
    pthread_t t;
    if (pthread_create(&t, 0, verify_thread_fn, 0) == 0)
        pthread_detach(t);
    else
        g_verify_running = 0;
}
static int resolve_audio(const char *file, const char *pack, char *out, int cap) {
    (void)pack;
    /* file 现在承载 MusicID 字符串; 查 g_map 得解压路径 */
    int id = atoi(file);
    if (map_lookup(id, out, cap)) return 1;
    out[0] = 0;
    return 0;
}
static void play_ogg(const char *file) {
    g_bg_paused = 0;
    stop_player();
    int d = 0;
    JNIEnv *e = jenv(&d);
    if (!e) return;
    jclass c = (*e)->FindClass(e, "android/media/MediaPlayer");
    if (!c) {
        g_play_fail = 1;
        g_fail_music = g_cur_music;
        LOGE("no MediaPlayer");
        jdet(d);
        return;
    }
    jmethodID ctor = (*e)->GetMethodID(e, c, "<init>", "()V");
    jmethodID sd = (*e)->GetMethodID(e, c, "setDataSource", "(Ljava/lang/String;)V");
    jmethodID pr = (*e)->GetMethodID(e, c, "prepare", "()V");
    jmethodID st = (*e)->GetMethodID(e, c, "start", "()V");
    jmethodID lp = (*e)->GetMethodID(e, c, "setLooping", "(Z)V");
    if (!ctor || !sd || !pr || !st) {
        g_play_fail = 1;
        g_fail_music = g_cur_music;
        LOGE("mp methods missing");
        (*e)->DeleteLocalRef(e, c);
        jdet(d);
        return;
    }
    jobject mp = (*e)->NewObject(e, c, ctor);
    if (!mp) {
        g_play_fail = 1;
        g_fail_music = g_cur_music;
        LOGE("NewObject fail");
        (*e)->DeleteLocalRef(e, c);
        jdet(d);
        return;
    }
    char full[768];
    extern const char *g_pack_sel;
    if (!resolve_audio(file, g_pack_sel, full, sizeof(full))) {
        g_play_fail = 1;
        g_fail_music = g_cur_music;
        LOGE("resolve fail %s", file);
        (*e)->DeleteLocalRef(e, mp);
        (*e)->DeleteLocalRef(e, c);
        jdet(d);
        return;
    }
    int nfd = open(full, O_RDONLY);
    LOGI("open=%d errno=%d %s", nfd, errno, full);
    int useFd = 0;
    if (nfd >= 0) {
        jmethodID sdfd = (*e)->GetMethodID(e, c, "setDataSource", "(Ljava/io/FileDescriptor;)V");
        if (sdfd) {
            jclass fdc = (*e)->FindClass(e, "java/io/FileDescriptor");
            if (fdc) {
                jmethodID fdctor = (*e)->GetMethodID(e, fdc, "<init>", "()V");
                jfieldID ffd = (*e)->GetFieldID(e, fdc, "descriptor", "I");
                if (fdctor && ffd) {
                    jobject fdo = (*e)->NewObject(e, fdc, fdctor);
                    if (fdo) {
                        (*e)->SetIntField(e, fdo, ffd, (jint)nfd);
                        (*e)->CallVoidMethod(e, mp, sdfd, fdo);
                        useFd = 1;
                        close(nfd);
                        (*e)->DeleteLocalRef(e, fdo);
                    }
                }
            }
        }
        if (!useFd) close(nfd);
    }
    if (!useFd) {
        jstring js = (*e)->NewStringUTF(e, full);
        (*e)->CallVoidMethod(e, mp, sd, js);
    }
    (*e)->CallVoidMethod(e, mp, pr);
    if ((*e)->ExceptionOccurred(e)) {
        (*e)->ExceptionClear(e);
        g_play_fail = 1;
        g_fail_music = g_cur_music;
        LOGE("prepare fail %s", full);
        jmethodID rl = (*e)->GetMethodID(e, c, "release", "()V");
        if (rl) (*e)->CallVoidMethod(e, mp, rl);
        (*e)->DeleteLocalRef(e, mp);
        (*e)->DeleteLocalRef(e, c);
        jdet(d);
        return;
    }
    g_fading = 0;
    g_fade_running = 0;
    if (lp) {
        jboolean t = 1;
        (*e)->CallVoidMethod(e, mp, lp, t);
    }
    {
        jmethodID sv = (*e)->GetMethodID(e, c, "setVolume", "(FF)V");
        if (sv) {
            (*e)->CallVoidMethod(e, mp, sv, 0.0f, 0.0f);
        }
    }
    (*e)->CallVoidMethod(e, mp, st);
    if ((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e);
    {
        jobject gnew = (*e)->NewGlobalRef(e, mp);
        jobject old = pl_install(gnew); /* 装上新的，拿回旧的 */
        if (old) {
            /* 关掉被顶替的旧实例 */
            jclass oc = (*e)->GetObjectClass(e, old);
            if (oc) {
                jmethodID ost = (*e)->GetMethodID(e, oc, "stop", "()V");
                jmethodID orl = (*e)->GetMethodID(e, oc, "release", "()V");
                if (ost) (*e)->CallVoidMethod(e, old, ost);
                if (orl) (*e)->CallVoidMethod(e, old, orl);
                (*e)->DeleteLocalRef(e, oc);
            }
            if ((*e)->ExceptionOccurred(e)) (*e)->ExceptionClear(e);
            (*e)->DeleteGlobalRef(e, old);
        }
    }
    LOGI("PLAY %s", full);
    start_fadein();
    jdet(d);
}
/* ===== SFX replace via SoundPool ===== */
static jobject g_sfxpool = 0;
static volatile int g_sfx_ok = 0;
#define SFX_MAX 128
static int g_sfx_type[SFX_MAX];
static int g_sfx_sid[SFX_MAX];
static int g_sfx_n = 0;
/* sfx file map: type -> path (from workshop packs) */
#define SFX_MAP_MAX 256
typedef struct {
    int type;
    char path[256];
} sfxmap_t;
static sfxmap_t g_sfxmap[SFX_MAP_MAX];
static int g_sfxmap_n = 0;
static void sfx_map_add(int type, const char *path) {
    tbl_lock();
    for (int i = 0; i < g_sfxmap_n; i++) {
        if (g_sfxmap[i].type == type) {
            strncpy(g_sfxmap[i].path, path, sizeof(g_sfxmap[i].path) - 1);
            g_sfxmap[i].path[sizeof(g_sfxmap[i].path) - 1] = 0;
            tbl_unlock();
            return;
        }
    }
    if (g_sfxmap_n < SFX_MAP_MAX) {
        g_sfxmap[g_sfxmap_n].type = type;
        strncpy(g_sfxmap[g_sfxmap_n].path, path, sizeof(g_sfxmap[g_sfxmap_n].path) - 1);
        g_sfxmap[g_sfxmap_n].path[sizeof(g_sfxmap[g_sfxmap_n].path) - 1] = 0;
        g_sfxmap_n++;
    }
    tbl_unlock();
}
static const char *sfx_map_lookup(int type) {
    for (int i = 0; i < g_sfxmap_n; i++)
        if (g_sfxmap[i].type == type) return g_sfxmap[i].path;
    return 0;
}

static int sfx_lookup_nolock(int type) {
    for (int i = 0; i < g_sfx_n; i++)
        if (g_sfx_type[i] == type) return g_sfx_sid[i];
    return -1;
}
static int sfx_lookup(int type) {
    tbl_lock();
    int r = sfx_lookup_nolock(type);
    tbl_unlock();
    return r;
}

static int sfx_load_file(JNIEnv *e, const char *path) {
    jclass clsSPc = (*e)->FindClass(e, "android/media/SoundPool");
    (*e)->ExceptionClear(e);
    if (!clsSPc) return 0;
    jmethodID loadPath = (*e)->GetMethodID(e, clsSPc, "load", "(Ljava/lang/String;I)I");
    (*e)->ExceptionClear(e);
    if (!loadPath) return 0;
    int nfd = open(path, O_RDONLY);
    if (nfd < 0) {
        LOGE("sfx: open fail errno=%d %s", errno, path);
        return 0;
    }
    close(nfd);
    jstring jpath = (*e)->NewStringUTF(e, path);
    if (!jpath) return 0;
    int sid = (*e)->CallIntMethod(e, g_sfxpool, loadPath, jpath, (jint)1);
    jobject exA = (*e)->ExceptionOccurred(e);
    if (exA) {
        jclass clsT = (*e)->FindClass(e, "java/lang/Throwable");
        jmethodID gmsg = (*e)->GetMethodID(e, clsT, "toString", "()Ljava/lang/String;");
        jstring ms = (*e)->CallObjectMethod(e, exA, gmsg);
        const char *cs = (*e)->GetStringUTFChars(e, ms, 0);
        LOGE("sfx: load EX: %s", cs ? cs : "?");
        if (cs) (*e)->ReleaseStringUTFChars(e, ms, cs);
        (*e)->DeleteLocalRef(e, ms);
        (*e)->ExceptionClear(e);
        sid = 0;
    }
    (*e)->DeleteLocalRef(e, jpath);
    return sid;
}

static void sfx_xnb_scan(void);
static void sfx_scan_load(void) {
    int d0 = 0;
    JNIEnv *e = jenv(&d0);
    if (!e) return;
    for (int i = 0; i < g_sfxcount; i++) {
        if (!g_sfxitems[i].enable || g_sfxitems[i].type <= 0 || !g_sfxitems[i].file[0]) continue;
        char full[768];
        if (g_sfxitems[i].pack[0])
            snprintf(full, sizeof(full), "%s/sfx_packs/%s/%s", g_dir, g_sfxitems[i].pack, g_sfxitems[i].file);
        else
            snprintf(full, sizeof(full), "%s/sfx_packs/%s", g_dir, g_sfxitems[i].file);
        int sid = sfx_load_file(e, full);
        if (sid > 0 && g_sfx_n < SFX_MAX) {
            g_sfx_type[g_sfx_n] = g_sfxitems[i].type;
            g_sfx_sid[g_sfx_n] = sid;
            g_sfx_n++;
            LOGI("sfx: loaded type=%d sid=%d file=%s", g_sfxitems[i].type, sid, g_sfxitems[i].file);
        } else {
            LOGE("sfx: load fail type=%d file=%s", g_sfxitems[i].type, g_sfxitems[i].file);
        }
    }
    /* load sfx from workshop packs (type = digits filename) */
    for (int i = 0; i < g_sfxmap_n; i++) {
        if (sfx_lookup_nolock(g_sfxmap[i].type) > 0) continue; /* config priority */
        int sid = sfx_load_file(e, g_sfxmap[i].path);
        if (sid > 0 && g_sfx_n < SFX_MAX) {
            g_sfx_type[g_sfx_n] = g_sfxmap[i].type;
            g_sfx_sid[g_sfx_n] = sid;
            g_sfx_n++;
            LOGI("sfx(pack): loaded type=%d sid=%d file=%s", g_sfxmap[i].type, sid, g_sfxmap[i].path);
        } else {
            LOGE("sfx(pack): fail type=%d file=%s", g_sfxmap[i].type, g_sfxmap[i].path);
        }
    }
    LOGI("sfx: total loaded=%d", g_sfx_n);
    jdet(d0);
}
static void sfx_init(void) {
    if (g_sfx_ok || !g_vm) return;
    int d = 0;
    JNIEnv *e = jenv(&d);
    if (!e) {
        LOGE("sfx: no env");
        return;
    }
    jclass clsSP = (*e)->FindClass(e, "android/media/SoundPool$Builder");
    if (!clsSP) {
        LOGE("sfx: no SoundPool.Builder");
        (*e)->ExceptionClear(e);
        jdet(d);
        return;
    }
    jmethodID ctorSP = (*e)->GetMethodID(e, clsSP, "<init>", "()V");
    jmethodID setMax = (*e)->GetMethodID(e, clsSP, "setMaxStreams", "(I)Landroid/media/SoundPool$Builder;");
    jmethodID setAttr = (*e)->GetMethodID(e, clsSP, "setAudioAttributes", "(Landroid/media/AudioAttributes;)Landroid/media/SoundPool$Builder;");
    jmethodID build = (*e)->GetMethodID(e, clsSP, "build", "()Landroid/media/SoundPool;");
    jclass clsAA = (*e)->FindClass(e, "android/media/AudioAttributes$Builder");
    jmethodID ctorAA = clsAA ? (*e)->GetMethodID(e, clsAA, "<init>", "()V") : 0;
    jmethodID setUsage = clsAA ? (*e)->GetMethodID(e, clsAA, "setUsage", "(I)Landroid/media/AudioAttributes$Builder;") : 0;
    jmethodID setContent = clsAA ? (*e)->GetMethodID(e, clsAA, "setContentType", "(I)Landroid/media/AudioAttributes$Builder;") : 0;
    jmethodID buildAA = clsAA ? (*e)->GetMethodID(e, clsAA, "build", "()Landroid/media/AudioAttributes;") : 0;
    jobject aa = 0;
    if (clsAA && ctorAA && buildAA) {
        jobject ab = (*e)->NewObject(e, clsAA, ctorAA);
        if (setUsage) {
            jobject r = (*e)->CallObjectMethod(e, ab, setUsage, 14);
            if (r) (*e)->DeleteLocalRef(e, r);
        }
        if (setContent) {
            jobject r = (*e)->CallObjectMethod(e, ab, setContent, 4);
            if (r) (*e)->DeleteLocalRef(e, r);
        }
        aa = (*e)->CallObjectMethod(e, ab, buildAA);
    }
    jobject b = (*e)->NewObject(e, clsSP, ctorSP);
    if (setMax) {
        jobject r = (*e)->CallObjectMethod(e, b, setMax, 8);
        if (r) (*e)->DeleteLocalRef(e, r);
    }
    if (setAttr && aa) {
        jobject r = (*e)->CallObjectMethod(e, b, setAttr, aa);
        if (r) (*e)->DeleteLocalRef(e, r);
    }
    jobject sp = build ? (*e)->CallObjectMethod(e, b, build) : 0;
    if (!sp) {
        LOGE("sfx: build fail");
        if (aa) (*e)->DeleteLocalRef(e, aa);
        (*e)->DeleteLocalRef(e, b);
        jdet(d);
        return;
    }
    g_sfxpool = (*e)->NewGlobalRef(e, sp);
    (*e)->DeleteLocalRef(e, sp);
    jdet(d);
    g_sfx_ok = 1;
    sfx_scan_load();
}

static void sfx_play(int type, float vol) {
    if (!g_sfx_ok || !g_sfxpool) return;
    int sid = sfx_lookup(type);
    if (sid <= 0) return;
    int d = 0;
    JNIEnv *e = jenv(&d);
    if (!e) return;
    jclass c = (*e)->GetObjectClass(e, g_sfxpool);
    if (c) {
        jmethodID pl = (*e)->GetMethodID(e, c, "play", "(IFFIIF)I");
        if (pl) {
            float vv = vol;
            if (vv < 0) vv = 0;
            if (vv > 1) vv = 1;
            (*e)->CallIntMethod(e, g_sfxpool, pl, (jint)sid, vv, vv, (jint)1, (jint)0, (jfloat)1.0f);
            jobject ex = (*e)->ExceptionOccurred(e);
            if (ex) {
                (*e)->ExceptionClear(e);
            }
        }
        (*e)->DeleteLocalRef(e, c);
    }
    jdet(d);
}

static bool sfx_prefix(patch_handle_t inst, void **args, const patch_method_signature_t *sig, void *result) {
    (void)inst;
    (void)sig;
    (void)result;
    if (!args || !args[0]) return false;
    int type = *(int *)args[0];
    int style = 1;
    if (args[3]) {
        int v = *(int *)args[3];
        style = v;
    }
    float vol = 1.0f;
    if (args[4]) {
        float v = *(float *)args[4];
        if (v >= 0.0f && v <= 1.0f) vol = v;
    }
    /* 1) OpenSL ES (xnb): 一次加锁内先试 sfx:T:S 再试 sfx:T */
    char k1[64], k2[64];
    snprintf(k1, sizeof(k1), "sfx:%d:%d", type, style);
    snprintf(k2, sizeof(k2), "sfx:%d", type);
    if (sl_try_play2(k1, k2, 0.0f, vol)) return true;
    /* 2) fallback SoundPool (ogg) */
    if (!g_sfx_ok) return false;
    int sid = sfx_lookup(type);
    if (sid <= 0) return false;
    sfx_play(type, vol);
    return true;
}

/* 同曲归一：50(旅程开始含前奏) 与 51(旅程开始) 是同一首歌 */
static int music_norm(int idx) {
    if (idx == 50) return 51;
    return idx;
}

/* 场景组: 同组曲目间轮换时接着放, 不重播不淡出 */
static int music_group(int idx) {
    idx = music_norm(idx);
    if (idx == 1 || idx == 18 || idx == 49) return 1; /* 地表白天 */
    return idx;
}

/* 该 idx 是否被我们配置了替换 */
static int music_is_replaced(int idx) {
    if (idx < 0) return 0;
    char tmp[360];
    if (map_lookup(music_norm(idx), tmp, sizeof(tmp))) return 1;
    if (idx != music_norm(idx) && map_lookup(idx, tmp, sizeof(tmp))) return 1;
    return 0;
}

/* 原版"旧曲淡出停止"钩子：当我们的替换曲还在播(g_active!=-1)时，把原版退场曲音量压 0，
   避免进世界时原版主菜单曲与替换曲同时出声 */
static bool hook_stop_prefix(patch_handle_t instance, void **args, const void *sig, void *result) {
    (void)instance;
    (void)result;
    (void)sig;
    /* 该方法每帧被调 600+ 次; 只有"替换曲正在播"时才需压原版退场曲音量, 否则立即返回(性能关键) */
    if (g_active == -1) return false;
    if (!args) return false;
    /* 参数: A[0]=int, A[1]=float(音量), A[2]=double, A[3]=bool */
    if (args[0] && args[1]) {
        int sidx = music_norm(*(int *)args[0]);
        if (sidx >= 0 && music_is_replaced(sidx)) {
            float *vol = (float *)args[1];
            if (*vol > 0.0f) {
                *vol = 0.0f;
            }
        }
    }
    return false;
}
static int g_hp_n = 0;
static bool hook_prefix(patch_handle_t instance, void **args, const void *sig, void *result) {
    (void)instance;
    (void)result;
    (void)sig;
    g_hp_n++;
    if (!args) return false;
    g_last_hook_ms = now_ms();
    if (g_bg_paused) resume_player();
    int idx = args[1] ? *(int *)args[1] : -1;
    idx = music_norm(idx);
    {
        static int last_idx = -999;
        if (idx != last_idx) {
            last_idx = idx;
            LOGI("IDX -> %d", idx);
        }
    }
    float *vol = args[2] ? (float *)args[2] : 0;
    float raw_b = 0.0f;
    if (vol) {
        float b = *vol;
        if (b > 0.0f && b < 2.0f) raw_b = b;
    }
    if (g_cover && g_active == -1 && g_cover_ms > 0 && now_ms() - g_cover_ms > 1500) {
        LOGI("cover timeout force clear");
        g_cover = 0;
    }
    char hitpath[360] = {0};
    int hitid = 0;
    if (map_lookup(music_norm(idx), hitpath, sizeof(hitpath)))
        hitid = music_norm(idx);
    else if (idx != music_norm(idx) && map_lookup(idx, hitpath, sizeof(hitpath)))
        hitid = idx;
    int failed = (g_play_fail && g_fail_music == idx);
    if (hitid) {
        if (!failed) {
            if (vol) *vol = 0.0f;
            if (raw_b > 0.0f) {
                if (raw_b == g_vol_prev) {
                    g_vol_same_n++;
                } else {
                    g_vol_same_n = 0;
                    g_vol_prev = raw_b;
                }
                if (g_vol_same_n >= 1) {
                    static float lgv = -1.0f;
                    if (raw_b != lgv) {
                        lgv = raw_b;
                        g_game_vol = raw_b;
                        update_player_vol();
                        LOGI("vol stable adopt %.3f", raw_b);
                    }
                }
            } else {
                g_vol_same_n = 0;
            }
            g_cover = 1;
            g_cover_ms = now_ms();
            if (g_fading && g_pend_idx == -1) {
                LOGI("HIT during fade -> cancel fade, resume");
                g_fade_running = 0;
                g_fading = 0;
                update_player_vol();
            }
            if (g_release) {
                LOGI("HIT during release-delay -> cancel release");
                g_release = 0;
            }
            if (g_leave_ms) {
                LOGI("HIT cancels leave timer");
                g_leave_ms = 0;
            }
            if (g_orig_fade_in_ms) {
                g_orig_fade_in_ms = 0;
            }
            if (g_active != idx) {
                if (g_active != -1 && music_group(g_active) == music_group(idx)) {
                    LOGI("HIT same group %d->%d keep playing", g_active, idx);
                } else {
                    char ts[16];
                    snprintf(ts, sizeof(ts), "%d", hitid);
                    g_pack_sel = "";
                    if (g_active != -1 && g_playing_alive && (!g_fading || g_fade_running || g_pend_idx != -1)) {
                        LOGI("HIT music=%d hitid=%d -> pend play %s", idx, hitid, hitpath);
                        g_pend_idx = idx;
                        strncpy(g_pend_hit, hitpath, sizeof(g_pend_hit) - 1);
                        g_pend_hit[sizeof(g_pend_hit) - 1] = 0;
                        g_active = idx;
                        if (!g_fading && !g_fade_running) start_fade();
                    } else {
                        g_active = idx;
                        LOGI("HIT music=%d hitid=%d play %s", idx, hitid, hitpath);
                        play_ogg_async(ts, idx);
                    }
                }
            }
        } else {
            if (g_active != idx) {
                LOGI("FAIL music=%d, keep original", idx);
                g_active = idx;
            }
        }
    } else {
        if (g_active != -1 && !g_fading) {
            if (g_leave_ms == 0) {
                g_leave_ms = now_ms();
                LOGI("leave %d (hold 500ms)", g_active);
            }
            g_play_fail = 0;
            g_fail_music = -1;
            if (now_ms() - g_leave_ms > 500) {
                LOGI("leave delay done -> fade");
                g_leave_ms = 0;
                g_pend_idx = -1;
                g_pend_hit[0] = 0; /* leave 路径清换曲 pend */
                start_fade();
            }
            if (vol) *vol = 0.0f;
        } else {
            if (vol) {
                if (g_orig_fade_in_ms > 0) {
                    long long el = now_ms() - g_orig_fade_in_ms;
                    const long long fdur = 800;
                    float gv = g_game_vol > 0.0f ? g_game_vol : 0.75f;
                    if (el >= fdur) {
                        g_orig_fade_in_ms = 0;
                        *vol = gv;
                    } else if (el < 0) {
                        *vol = 0.0f;
                    } else {
                        *vol = gv * ((float)el / (float)fdur);
                    }
                } else if (g_fading || g_cover || g_active != -1) {
                    *vol = 0.0f;
                } else {
                    *vol = g_game_vol > 0.0f ? g_game_vol : 0.75f;
                }
            }
        }
    }
    if (g_release) {
        if (!g_fading && g_active == -1 && now_ms() - g_release_ms > 1500) {
            LOGI("release-delay done (no vol restore)");
            g_release = 0;
            g_cover = 0;
        }
    }
    return false;
}

/* LoadSoundEffect hook: 预留给循环环境音替换 */
static void loadse_postfix(patch_handle_t inst, void **args, void *result, const patch_method_signature_t *sig) {
    (void)inst;
    (void)args;
    (void)result;
    (void)sig;
}

static void *import_thread(void *arg) {
    (void)arg;
    import_all_packs();
    sfx_init();
    sfx_xnb_scan();
    sl_preload_all();
    LOGI("import thread done");
    return 0;
}
static void start_import_thread(void) {
    pthread_t t;
    if (pthread_create(&t, 0, import_thread, 0) == 0)
        pthread_detach(t);
    else {
        LOGE("import thread create fail, run inline");
        import_all_packs();
        sfx_init();
        sfx_xnb_scan();
    }
}

static bool init_module(module_entry_t *entry) {
    LOGI("===== MusicPack init =====");
    if (entry && entry->private_dir) {
        strncpy(g_dir, entry->private_dir, sizeof(g_dir) - 1);
        LOGI("private_dir=%s", g_dir);
    }
    typedef int (*GetVMs_t)(JavaVM **, jsize, jsize *);
    GetVMs_t fn = 0;
    dl_iterate_phdr(phdr_cb, 0);
    LOGI("libart base=%p path=%s", g_libart_base, g_libart_path[0] ? g_libart_path : "(none)");
    if (g_libart_base) fn = (GetVMs_t)find_sym_loaded("JNI_GetCreatedJavaVMs");
    if (!fn) fn = (GetVMs_t)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
    if (fn) {
        JavaVM *vm = 0;
        jsize n = 0;
        fn(&vm, 1, &n);
        g_vm = vm;
        LOGI("vm=%p n=%d", (void *)vm, (int)n);
    } else
        LOGE("no JNI_GetCreatedJavaVMs");
    load_cfg_auto();
    import_titles_sync();
    start_import_thread();
    start_watchdog();

    patch_handle_t TLAS = patchlib_type_get_type("Terraria.Audio", "LegacyAudioSystem");
    LOGI("TLAS=%p", TLAS);
    if (TLAS) {
        {
            tefstd_vector_t mv;
            if (tefstd_vector_init(&mv, sizeof(patch_handle_t))) {
                if (patchlib_type_get_methods(TLAS, 0, &mv)) {
                    int mn = (int)mv.size;
                    patch_handle_t *md = (patch_handle_t *)mv.data;
                    LOGI("LAS methods=%d", mn);
                    for (int mi = 0; mi < mn; mi++) {
                        patch_handle_t mm = md[mi];
                        const char *nm = mm ? patchlib_method_get_name(mm) : 0;
                        if (nm) LOGI("  M[%d]=%s pc=%d", mi, nm, patchlib_method_get_param_count(mm));
                    }
                }
                tefstd_vector_destroy(&mv);
            }
        }
        patch_handle_t mUCT = patchlib_type_get_method(TLAS, "UpdateCommonTrack");
        LOGI("m.UCT=%p pc=%d", mUCT, mUCT ? patchlib_method_get_param_count(mUCT) : -1);
        if (mUCT) {
            LOGI("UCT hook=%d", (int)patchlib_install_prepost_hook(mUCT, hook_prefix, 0));
            patch_handle_t mStop = patchlib_type_get_method(TLAS, "UpdateCommonTrackTowardStopping");
            LOGI("m.Stop=%p pc=%d", mStop, mStop ? patchlib_method_get_param_count(mStop) : -1);
            if (mStop) LOGI("STOP hook=%d", (int)patchlib_install_prepost_hook(mStop, hook_stop_prefix, 0));
        }
    }
    {
        patch_handle_t TLP = patchlib_type_get_type("Terraria.Audio", "LegacySoundPlayer");
        LOGI("TLP=%p", TLP);
        if (TLP) {
            patch_handle_t mPS = patchlib_type_get_method_by_param_count(TLP, "PlaySound", 6);
            LOGI("mPS=%p", mPS);
            if (mPS) LOGI("PShook=%d", (int)patchlib_install_prepost_hook(mPS, sfx_prefix, 0));
        }
    }
    {
        patch_handle_t TCM = patchlib_type_get_type("Microsoft.Xna.Framework.Content", "ContentManager");
        LOGI("TCM=%p", TCM);
        if (TCM) {
            patch_handle_t mLSE = patchlib_type_get_method_by_param_count(TCM, "LoadSoundEffect", 1);
            LOGI("mLSE=%p", mLSE);
            if (mLSE) LOGI("LSEhook=%d", (int)patchlib_install_prepost_hook(mLSE, 0, loadse_postfix));
        }
    }
    return true;
}
static bool cleanup_module(module_entry_t *e) {
    (void)e;
    g_wd_running = 0;
    g_fade_running = 0;
    stop_player();
    return true;
}
static void hot_reload(module_entry_t *e) {
    (void)e;
}
static const module_info_t *get_info(void) {
    return &g_info;
}
static const module_ops_t g_ops = {init_module, cleanup_module, hot_reload, get_info};
API_EXPORT const module_ops_t *API_CALL module_create(void) {
    return &g_ops;
}

/* ===== XNB sfx via OpenSL ES (Content/Sounds/*.xnb) ===== */
static void sfx_xnb_scan(void) {
    if (!sl_init()) {
        LOGE("xnb: sl_init fail");
        return;
    }
    char root[768];
    snprintf(root, sizeof(root), "%s/sfx_xnb", g_dir);
    DIR *d = opendir(root);
    if (!d) {
        LOGI("xnb: no sfx_xnb dir");
        return;
    }
    closedir(d);
    /* 递归深度 <=4 找 Content/Sounds/*.xnb */
    char stack[8][768];
    int depth[8];
    int sp = 0;
    snprintf(stack[0], sizeof(stack[0]), "%s", root);
    depth[0] = 0;
    sp = 1;
    int n = 0;
    while (sp > 0) {
        sp--;
        char cur[768];
        strncpy(cur, stack[sp], sizeof(cur) - 1);
        cur[sizeof(cur) - 1] = 0;
        int dep = depth[sp];
        DIR *dd = opendir(cur);
        if (!dd) continue;
        struct dirent *de;
        while ((de = readdir(dd))) {
            if (de->d_name[0] == 0x2e) continue;
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s", cur, de->d_name);
            struct stat st;
            if (stat(full, &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                if (dep < 4 && sp < 8) {
                    strncpy(stack[sp], full, sizeof(stack[sp]) - 1);
                    stack[sp][sizeof(stack[sp]) - 1] = 0;
                    depth[sp] = dep + 1;
                    sp++;
                }
                continue;
            }
            /* 只收名字里含 Sounds/ 且 .xnb 的 */
            const char *ext = strrchr(de->d_name, 0x2e);
            if (!ext || strcasecmp(ext, ".xnb") != 0) continue;
            if (!strstr(full, "Sounds")) continue;
            const char *sl2 = strrchr(de->d_name, 0x2f);
            const char *base = sl2 ? (sl2 + 1) : de->d_name;
            /* name -> type/style ; fallback digit */
            int type = -1, style = -1;
            if (!sfx_name_lookup(base, &type, &style)) {
                type = -1;
                style = -1;
            }
            if (type < 0) continue;
            char pcmf[1040];
            snprintf(pcmf, sizeof(pcmf), "%s.pcm", full);
            char pmetaf[1060];
            snprintf(pmetaf, sizeof(pmetaf), "%s.meta", pcmf);
            int rate = 0, ch = 0, cb = 0;
            FILE *cm = fopen(pmetaf, "r");
            if (cm) {
                if (fscanf(cm, "%d %d %d", &rate, &ch, &cb) != 3) rate = 0;
                fclose(cm);
            }
            if (rate <= 0) {
                xnb_sound_t s;
                char err[128];
                if (!xnb_parse_sound(full, &s, err, sizeof(err))) {
                    LOGE("xnb: parse fail %s (%s)", base, err);
                    continue;
                }
                unsigned char *pcm = 0;
                int bytes = 0;
                if (!xnb_normalize_pcm16(&s, &pcm, &bytes, &rate, &ch, err, sizeof(err))) {
                    LOGE("xnb: norm fail %s (%s)", base, err);
                    xnb_sound_free(&s);
                    continue;
                }
                FILE *pf = fopen(pcmf, "wb");
                if (pf) {
                    fwrite(pcm, 1, bytes, pf);
                    fclose(pf);
                }
                FILE *mf = fopen(pmetaf, "w");
                if (mf) {
                    fprintf(mf, "%d %d %d", rate, ch, bytes);
                    fclose(mf);
                }
                cb = bytes;
                free(pcm);
                xnb_sound_free(&s);
            }
            if (rate <= 0) continue;
            char key[64];
            if (style >= 0)
                snprintf(key, sizeof(key), "sfx:%d:%d", type, style);
            else
                snprintf(key, sizeof(key), "sfx:%d", type);
            if (sl_register(key, pcmf, rate, ch)) {
                n++;
                LOGI("xnb: reg type=%d rate=%d ch=%d bytes=%d file=%s", type, rate, ch, cb, base);
            } else
                LOGE("xnb: reg fail type=%d", type);
        }
        closedir(dd);
    }
    LOGI("xnb: total registered=%d", n);
}
