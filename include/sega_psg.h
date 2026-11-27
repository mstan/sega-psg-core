#ifndef SEGA_PSG_H
#define SEGA_PSG_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct sega_psg sega_psg;
typedef enum { SEGA_PSG_GENESIS = 0, SEGA_PSG_SMS_GG = 1 } sega_psg_profile;
/* Genesis advance uses master clocks (/240); SMS/GG uses Z80 clocks (/16).
 * Input clock Hz is explicit. Profiles remain fixed for each independent
 * instance. All calls on one instance require caller-provided exclusion. */
sega_psg *sega_psg_create(sega_psg_profile profile, uint32_t input_clock_hz);
void sega_psg_destroy(sega_psg *psg);
void sega_psg_reset(sega_psg *psg);
void sega_psg_write(sega_psg *psg, uint8_t value);
/* Returns 0 for Genesis, which has no GG routing register. */
int sega_psg_write_stereo(sega_psg *psg, uint8_t mask);
void sega_psg_advance(sega_psg *psg, uint32_t input_cycles);
size_t sega_psg_available(const sega_psg *psg);
/* Genesis output is mono; SMS/GG output is interleaved stereo. */
unsigned sega_psg_channels(const sega_psg *psg);
uint32_t sega_psg_sample_rate(const sega_psg *psg);
size_t sega_psg_render(sega_psg *psg, int16_t *samples, size_t frames);
/* Current-build donor-compatible FILE snapshot. Load discards queued PCM;
 * take snapshots at a drained output boundary. Invalid loads are transactional. */
int sega_psg_save(sega_psg *psg, FILE *file);
int sega_psg_load(sega_psg *psg, FILE *file);
/* Genesis-only diagnostic audition leaves canonical synthesis unchanged. */
int sega_psg_audition_channels(sega_psg *psg, unsigned mask);
size_t sega_psg_peek_audition(sega_psg *psg, int16_t *filtered, int16_t *raw, size_t frames);

/* Genesis supports paired capture, full queued-output rollback and state hash.
 * Both profiles support remainder reset and their existing diagnostic dump.
 * Unsupported SMS/GG paired/rollback/hash operations return zero. */
size_t sega_psg_render_pair(sega_psg *,int16_t *filtered,int16_t *raw,size_t frames);
void sega_psg_reset_leftover(sega_psg *);
size_t sega_psg_rb_save(sega_psg *,void *output,size_t capacity);
int sega_psg_rb_load(sega_psg *,const void *input,size_t size);
uint64_t sega_psg_state_hash(sega_psg *);
int sega_psg_state_dump(sega_psg *,char *output,int capacity);
#ifdef __cplusplus
}
#endif
#endif
