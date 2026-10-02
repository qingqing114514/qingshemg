#ifndef OPENSL_H
#define OPENSL_H

/* registered key -> normalized 16bit PCM, played via OpenSL ES */
int  sl_init(void);
int  sl_register(const char* key, const char* path, int rate, int channels);
int  sl_has(const char* key);
int  sl_play(const char* key, float pitch_offset);
int  sl_play_vol(const char* key, float pitch_offset, float volume);
/* 一次加锁完成: 判断存在+取PCM+播放; 命中返回1, 未注册/失败返回0. 用于高频 hook 路径减锁 */
int  sl_try_play(const char* key, float pitch_offset, float volume);
int  sl_try_play2(const char* k1, const char* k2, float pitch_offset, float volume);
int  sl_preload(const char* key);
int  sl_preload_all(void);
void sl_shutdown(void);
void sl_housekeep(void);
int  sl_is_init(void);
#endif
