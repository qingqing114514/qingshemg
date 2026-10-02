#include "xnbsound.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define XNB_MAX_BYTES (32 * 1024 * 1024)

static int rd_u16(const unsigned char *d, long n, long off, uint16_t *v) {
    if (off < 0 || off + 2 > n) return 0;
    *v = (uint16_t)(d[off] | (d[off + 1] << 8));
    return 1;
}
static int rd_u32(const unsigned char *d, long n, long off, uint32_t *v) {
    if (off < 0 || off + 4 > n) return 0;
    *v = (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) | ((uint32_t)d[off + 2] << 16) | ((uint32_t)d[off + 3] << 24);
    return 1;
}
static int rd_u32c(const unsigned char *d, long n, long *c, uint32_t *v) {
    if (rd_u32(d, n, *c, v)) {
        *c += 4;
        return 1;
    }
    return 0;
}
static int rd_i32c(const unsigned char *d, long n, long *c, int32_t *v) {
    uint32_t u;
    if (!rd_u32c(d, n, c, &u)) return 0;
    *v = (int32_t)u;
    return 1;
}
static int rd_7bit(const unsigned char *d, long n, long *c, uint32_t *v) {
    uint32_t r = 0;
    int sh;
    for (sh = 0; sh < 35; sh += 7) {
        if (*c >= n) return 0;
        unsigned char b = d[(*c)++];
        r |= (uint32_t)(b & 0x7F) << sh;
        if ((b & 0x80) == 0) {
            *v = r;
            return 1;
        }
    }
    return 0;
}
static int skipn(long n, long *c, long cnt) {
    if (*c < 0 || cnt < 0 || *c + cnt > n) return 0;
    *c += cnt;
    return 1;
}

int xnb_parse_sound(const char *path, xnb_sound_t *out, char *err, int errcap) {
    memset(out, 0, sizeof(*out));
    err[0] = 0;
    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(err, errcap, "stat fail");
        return 0;
    }
    if (st.st_size < 10 || st.st_size > XNB_MAX_BYTES) {
        snprintf(err, errcap, "size invalid");
        return 0;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errcap, "open fail");
        return 0;
    }
    long n = (long)st.st_size;
    unsigned char *d = (unsigned char *)malloc(n);
    if (!d) {
        fclose(f);
        snprintf(err, errcap, "oom");
        return 0;
    }
    if ((long)fread(d, 1, n, f) != (long)n) {
        fclose(f);
        free(d);
        snprintf(err, errcap, "read fail");
        return 0;
    }
    fclose(f);
    if (d[0] != 0x58 || d[1] != 0x4E || d[2] != 0x42) {
        free(d);
        snprintf(err, errcap, "not xnb");
        return 0;
    }
    if (d[5] & 0x80) {
        free(d);
        snprintf(err, errcap, "compressed xnb");
        return 0;
    }
    long c = 6;
    uint32_t declared = 0;
    if (!rd_u32c(d, n, &c, &declared) || (long)declared != n) {
        free(d);
        snprintf(err, errcap, "declared size mismatch");
        return 0;
    }
    uint32_t rc = 0;
    if (!rd_7bit(d, n, &c, &rc) || rc == 0 || rc > 64) {
        free(d);
        snprintf(err, errcap, "reader table bad");
        return 0;
    }
    int saw = 0;
    for (uint32_t i = 0; i < rc; i++) {
        uint32_t ns = 0;
        if (!rd_7bit(d, n, &c, &ns) || ns > 4096 || !skipn(n, &c, (long)ns)) {
            free(d);
            snprintf(err, errcap, "reader name bad");
            return 0;
        }
        long noff = c - (long)ns;
        if (noff >= 0 && (long)ns <= n - noff) {
            const char *rn = (const char *)(d + noff);
            for (int k = 0; k + (int)ns <= n - noff; k++) {
                if (k + 16 <= (int)ns && memcmp(rn + k, "SoundEffectReader", 16) == 0) {
                    saw = 1;
                    break;
                }
            }
        }
        uint32_t rv = 0;
        if (!rd_u32c(d, n, &c, &rv)) {
            free(d);
            snprintf(err, errcap, "reader ver missing");
            return 0;
        }
    }
    if (!saw) {
        free(d);
        snprintf(err, errcap, "no SoundEffectReader");
        return 0;
    }
    uint32_t shared = 0, prim = 0;
    if (!rd_7bit(d, n, &c, &shared) || !rd_7bit(d, n, &c, &prim) || prim == 0) {
        free(d);
        snprintf(err, errcap, "primary header bad");
        return 0;
    }
    uint32_t fs = 0;
    if (!rd_u32c(d, n, &c, &fs) || fs < 16 || fs > 256 || !skipn(n, &c, (long)fs)) {
        free(d);
        snprintf(err, errcap, "waveformatex bad");
        return 0;
    }
    long fb = c - (long)fs;
    out->format = (unsigned char *)malloc(fs);
    out->format_len = (int)fs;
    memcpy(out->format, d + fb, fs);
    if (!rd_u16(out->format, fs, 0, &out->format_tag) ||
        !rd_u16(out->format, fs, 2, &out->channel_count) ||
        !rd_u32(out->format, fs, 4, &out->sample_rate) ||
        !rd_u32(out->format, fs, 8, &out->avg_bytes_per_sec) ||
        !rd_u16(out->format, fs, 12, &out->block_align) ||
        !rd_u16(out->format, fs, 14, &out->bits_per_sample) ||
        out->channel_count == 0 || out->sample_rate == 0 || out->block_align == 0) {
        xnb_sound_free(out);
        free(d);
        snprintf(err, errcap, "fmt fields invalid");
        return 0;
    }
    uint32_t ws = 0;
    if (!rd_u32c(d, n, &c, &ws) || ws == 0 || ws > XNB_MAX_BYTES || !skipn(n, &c, (long)ws)) {
        xnb_sound_free(out);
        free(d);
        snprintf(err, errcap, "waveform bad");
        return 0;
    }
    long wb = c - (long)ws;
    out->waveform = (unsigned char *)malloc(ws);
    out->waveform_len = (int)ws;
    memcpy(out->waveform, d + wb, ws);
    if (out->format_tag == 1 && (ws % out->block_align) != 0) {
        xnb_sound_free(out);
        free(d);
        snprintf(err, errcap, "not block aligned");
        return 0;
    }
    if (!rd_i32c(d, n, &c, &out->loop_start) || !rd_i32c(d, n, &c, &out->loop_length) || !rd_i32c(d, n, &c, &out->duration_ms)) {
        xnb_sound_free(out);
        free(d);
        snprintf(err, errcap, "loop meta missing");
        return 0;
    }
    free(d);
    return 1;
}

void xnb_sound_free(xnb_sound_t *s) {
    if (!s) return;
    if (s->format) {
        free(s->format);
        s->format = 0;
    }
    if (s->waveform) {
        free(s->waveform);
        s->waveform = 0;
    }
    s->format_len = 0;
    s->waveform_len = 0;
}

static int nearest_rate(int r) {
    static const int tab[9] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    int best = tab[0], bd = abs(r - tab[0]);
    for (int i = 1; i < 9; i++) {
        int dd = abs(r - tab[i]);
        if (dd < bd) {
            best = tab[i];
            bd = dd;
        }
    }
    return best;
}

static int16_t dec_sample(const xnb_sound_t *s, long frame, int ch, int bps) {
    const unsigned char *b = s->waveform + frame * s->block_align + (long)ch * bps;
    switch (s->bits_per_sample) {
    case 8:
        return (int16_t)(((int)b[0] - 128) * 256);
    case 16:
        return (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    case 24: {
        int32_t v = (int32_t)b[0] | ((int32_t)b[1] << 8) | ((int32_t)b[2] << 16);
        if (v & 0x00800000) v |= 0xFF000000;
        return (int16_t)(v >> 8);
    }
    case 32: {
        int32_t v = (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
        return (int16_t)(v >> 16);
    }
    default:
        return 0;
    }
}

int xnb_normalize_pcm16(const xnb_sound_t *s, unsigned char **out, int *out_bytes, int *out_rate,
                        int *out_channels, char *err, int errcap) {
    *out = 0;
    *out_bytes = 0;
    if (err) err[0] = 0;
    if (s->format_tag != 1) {
        snprintf(err, errcap, "not WAVE_FORMAT_PCM");
        return 0;
    }
    if (s->channel_count != 1 && s->channel_count != 2) {
        snprintf(err, errcap, "not mono/stereo");
        return 0;
    }
    if (s->bits_per_sample != 8 && s->bits_per_sample != 16 && s->bits_per_sample != 24 && s->bits_per_sample != 32) {
        snprintf(err, errcap, "bits unsupported");
        return 0;
    }
    int bps = s->bits_per_sample / 8;
    long exp_align = (long)s->channel_count * bps;
    if ((long)s->block_align != exp_align || s->waveform_len % s->block_align != 0) {
        snprintf(err, errcap, "block align mismatch");
        return 0;
    }
    long in_frames = s->waveform_len / s->block_align;
    if (in_frames <= 0) {
        snprintf(err, errcap, "no frames");
        return 0;
    }
    if (s->sample_rate > 192000) {
        snprintf(err, errcap, "rate too high");
        return 0;
    }
    int r = (int)s->sample_rate;
    if (r < 8000) r = 8000;
    if (r > 48000) r = 48000;
    int orate = nearest_rate(r);
    double ratio = (double)orate / (double)s->sample_rate;
    long oframes = (long)((double)in_frames * ratio + 0.5);
    if (oframes < 1) oframes = 1;
    long obytes = oframes * (long)s->channel_count * 2;
    if (obytes > (long)(64 * 1024 * 1024)) {
        snprintf(err, errcap, "normalized too large");
        return 0;
    }
    unsigned char *pcm = (unsigned char *)malloc(obytes);
    if (!pcm) {
        snprintf(err, errcap, "oom");
        return 0;
    }
    for (long of = 0; of < oframes; of++) {
        double sp = (double)of * (double)s->sample_rate / (double)orate;
        long fa = (long)sp;
        if (fa > in_frames - 1) fa = in_frames - 1;
        long fb = fa + 1;
        if (fb > in_frames - 1) fb = in_frames - 1;
        double frac = sp - (double)fa;
        for (int ch = 0; ch < s->channel_count; ch++) {
            double a = dec_sample(s, fa, ch, bps);
            double b2 = dec_sample(s, fb, ch, bps);
            double m = a + (b2 - a) * frac;
            int32_t v = (int32_t)(m < 0 ? m - 0.5 : m + 0.5);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            long off = (of * s->channel_count + ch) * 2;
            pcm[off] = (unsigned char)(v & 0xFF);
            pcm[off + 1] = (unsigned char)(((uint16_t)v >> 8) & 0xFF);
        }
    }
    *out = pcm;
    *out_bytes = (int)obytes;
    *out_rate = orate;
    *out_channels = (int)s->channel_count;
    return 1;
}
