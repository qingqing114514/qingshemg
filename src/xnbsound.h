#ifndef XNBSOUND_H
#define XNBSOUND_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    unsigned char* format;
    int format_len;
    unsigned char* waveform;
    int waveform_len;
    uint16_t format_tag;
    uint16_t channel_count;
    uint32_t sample_rate;
    uint32_t avg_bytes_per_sec;
    uint16_t block_align;
    uint16_t bits_per_sample;
    int32_t loop_start;
    int32_t loop_length;
    int32_t duration_ms;
} xnb_sound_t;

int xnb_parse_sound(const char* path, xnb_sound_t* out, char* err, int errcap);
void xnb_sound_free(xnb_sound_t* s);
int xnb_normalize_pcm16(const xnb_sound_t* s, unsigned char** out, int* out_bytes, int* out_rate, int* out_channels, char* err, int errcap);
#endif
