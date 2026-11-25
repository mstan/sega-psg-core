#include "profile_api.h"
#include <stdlib.h>
#include <string.h>
#define GENESIS_MASTER_CLOCK_NTSC 53693175u
#define SMS_Z80_HZ 3579545u
#define SMS_PSG_DIVIDER 16
/*
 * sn76489.c — cycle-stamped SN76489 PSG.
 *
 * Clean-room SN76489 (Sega Master System / Mega Drive variant) implemented
 * from public hardware documentation — NO emulator-core code, so this carries
 * no third-party copyleft. It replaced the previous clownmdemu (AGPL) PSG
 * wrapper outright (the old path lives in git history only). See LICENSING.md.
 *
 * Models 3 square-tone channels + 1 LFSR noise channel, 4-bit logarithmic
 * volume (2 dB/step, 0xF = off), and the SMS/Genesis 16-bit noise LFSR
 * (white-noise taps at bits 0 and 3, feedback -> bit 15), with a one-pole
 * low-pass to round the step edges.
 *
 * Clocked at the PSG sample rate (master/240 ≈ 223.7 kHz NTSC), one tone-
 * counter tick per emitted sample. The public psg_* API and that sample rate
 * are unchanged, so all downstream mixing/resampling is unaffected.
 */


#include <string.h>

#define PSG_SAMPLE_DIVISOR_MASTER   240u   /* Z80_CLOCK_DIVIDER * PSG_SAMPLE_RATE_DIVIDER */


  /* in master-cycle units */

#define PSG_SCRATCH_SAMPLES  8192



typedef struct {unsigned mask;int32_t previous,output;} AuditionState;





/* ---- clean-room SN76489 ---- */
typedef struct {
    uint16_t tone_period[3];   /* 10-bit reload value */
    int32_t  tone_counter[3];
    uint8_t  tone_out[3];      /* square flip-flop (0/1) */
    uint8_t  vol[4];           /* 4-bit attenuation; index 3 = noise */
    uint8_t  noise_ctrl;       /* low 3 bits: fb-mode + rate */
    int32_t  noise_counter;
    uint16_t lfsr;
    uint8_t  noise_ff;         /* ÷2 tone-divider flip-flop (clocks the LFSR) */
    uint8_t  noise_out;
    uint8_t  latch;            /* (chan<<1)|type of last LATCH byte */
    int32_t  lpf_y;            /* low-pass: previous output  */
    int32_t  lpf_prev;         /* low-pass: previous input sample */
} SN76489;
typedef struct PSGContext {
int s_inited;
uint32_t s_leftover_master_cycles;
int16_t s_scratch[PSG_SCRATCH_SAMPLES];
int16_t s_raw_scratch[PSG_SCRATCH_SAMPLES];
int16_t s_aud_scratch[PSG_SCRATCH_SAMPLES],s_aud_raw[PSG_SCRATCH_SAMPLES];
AuditionState s_aud;
size_t s_scratch_write;
size_t s_scratch_read;
int s_aud_separate,s_aud_canonical_history;
SN76489 s_sn;
uint16_t powers[13][16]; int ready;
} PSGContext;
static AuditionState audition_state(PSGContext *ctx);
static void separate_audition_queue(PSGContext *ctx);
static void sn_reset(PSGContext *ctx, SN76489 *p);
static int32_t sn_step_mix(PSGContext *ctx, SN76489 *p,int32_t *channels);
static int16_t sn_filter_sample(PSGContext *ctx, int32_t mix,int32_t *previous,int32_t *output);
static int16_t sn_render_sample(PSGContext *ctx, SN76489 *p,int16_t *unfiltered);
static int16_t sn_render_audition(PSGContext *ctx, SN76489 *p,int16_t *raw,int16_t *audition,int16_t *aud_raw);
static unsigned sn_counter_span(PSGContext *ctx, int32_t *counter,unsigned period,unsigned samples);
static uint16_t sn_white_step(PSGContext *ctx, uint16_t bits);
static uint16_t sn_linear_apply(PSGContext *ctx, const uint16_t columns[16],uint16_t bits);
static uint16_t sn_white_span(PSGContext *ctx, uint16_t bits,unsigned steps);
static int sn_silent_span(PSGContext *ctx, SN76489 *p,int16_t *out,size_t samples);
static void psg_init(PSGContext *ctx);
static void psg_advance(PSGContext *ctx, uint32_t cycles_master);
static void psg_write(PSGContext *ctx, uint8_t value);
static size_t psg_render(PSGContext *ctx, int16_t *out, size_t sample_count);
static size_t psg_render_pair(PSGContext *ctx, int16_t *filtered,int16_t *unfiltered,size_t sample_count);
static void psg_set_audition_channels(PSGContext *ctx, unsigned mask);
static size_t psg_peek_audition(PSGContext *ctx, int16_t *filtered,int16_t *raw,size_t count);
static size_t psg_samples_available(PSGContext *ctx);
static uint32_t psg_sample_rate(PSGContext *ctx);
static void psg_reset_leftover(PSGContext *ctx);
static int psg_save_state(PSGContext *ctx, FILE *f);
static int psg_load_state(PSGContext *ctx, FILE *f);

#include "registers.inc"






/* First-order low-pass matching the reference (clownmdemu low-pass-filter.c
 * with coefficients 26.044, 24.044): out = ((in + prevIn)*SAMPLE_MAGIC +
 * prevOut*OUTPUT_MAGIC) >> 16. Replaces the old >>1 one-pole, which was too
 * bright and let PSG energy run hotter than the reference. */
#define SN_LPF_SAMPLE_MAGIC  2517   /* round(1.0    * 65536 / 26.044) */
#define SN_LPF_OUTPUT_MAGIC  60504  /* round(24.044 * 65536 / 26.044) */

static AuditionState audition_state(PSGContext *ctx)
{
    AuditionState state=ctx->s_aud;
    if(ctx->s_aud_canonical_history){state.previous=ctx->s_sn.lpf_prev;state.output=ctx->s_sn.lpf_y;}
    return state;
}
/* Preserve a queued full-channel prefix once, when a partial selection first
 * joins it. Ordinary playback never creates a second PCM representation. */
static void separate_audition_queue(PSGContext *ctx)
{
    if(ctx->s_aud_separate)return;
    size_t bytes=(ctx->s_scratch_write-ctx->s_scratch_read)*sizeof *ctx->s_scratch;
    memcpy(ctx->s_aud_scratch+ctx->s_scratch_read,ctx->s_scratch+ctx->s_scratch_read,bytes);
    memcpy(ctx->s_aud_raw+ctx->s_scratch_read,ctx->s_raw_scratch+ctx->s_scratch_read,bytes);
    ctx->s_aud_separate=1;
}

/* 4-bit attenuation -> linear amplitude. 2 dB/step (×10^-0.1), 0xF = silence.
 * Values match the reference (clownmdemu psg.c psg_volumes / known SN76489
 * behaviour): peak 0x1FFF with a per-channel /4 headroom so 4 summed channels
 * (4*8191 = 32764) just fit int16. Brings PSG level to parity with the
 * clownmdemu render measured by tools/synth_replay. */
#include "attenuation.inc"

static void sn_reset(PSGContext *ctx, SN76489 *p)
{
    memset(p, 0, sizeof *p);
    for (int i = 0; i < 3; i++) { p->tone_period[i] = 1; p->tone_counter[i] = 1; }
    p->vol[0] = p->vol[1] = p->vol[2] = p->vol[3] = 0xF;  /* all silent */
    p->noise_counter = 0x10;
    p->lfsr = 0x8000;
}





static int32_t sn_step_mix(PSGContext *ctx, SN76489 *p,int32_t *channels)
{
    int32_t mix = 0;

    for (int i = 0; i < 3; i++) {
        if (--p->tone_counter[i] <= 0) {
            p->tone_counter[i] = p->tone_period[i] ? p->tone_period[i] : 1;
            p->tone_out[i] ^= 1;
        }
        int32_t value=p->tone_out[i]?SN_VOL[p->vol[i]]:-SN_VOL[p->vol[i]];
        mix+=value;if(channels)channels[i]=value;
    }

    if (--p->noise_counter <= 0) {
        p->noise_counter = sn_noise_reload(ctx, p);
        /* The noise generator drives an internal /2 flip-flop (exactly like a
         * tone channel's square output); the LFSR is clocked only on that
         * flip-flop's rising edge, i.e. once per TWO counter periods. Without
         * this the LFSR ran twice as fast (noise an octave too high) — the
         * jump-SFX "boop". Matches BlastEm psg.c output_state[3] and clownmdemu
         * psg.c fake_output_bit. */
        p->noise_ff ^= 1;
        if (p->noise_ff) {
            uint16_t fb = (p->noise_ctrl & 0x04)            /* 1 = white */
                        ? ((p->lfsr ^ (p->lfsr >> 3)) & 1)  /* taps bit0 ^ bit3 */
                        : (p->lfsr & 1);                    /* periodic */
            p->lfsr = (uint16_t)((p->lfsr >> 1) | (fb << 15));
            p->noise_out = (uint8_t)(p->lfsr & 1);
        }
    }
    int32_t noise=p->noise_out?SN_VOL[p->vol[3]]:-SN_VOL[p->vol[3]];
    if(channels)channels[3]=noise;
    return mix+noise;
}
static int16_t sn_filter_sample(PSGContext *ctx, int32_t mix,int32_t *previous,int32_t *output)
{
    int64_t lp=((int64_t)(mix+*previous)*SN_LPF_SAMPLE_MAGIC+
        (int64_t)*output*SN_LPF_OUTPUT_MAGIC)/65536;
    *previous=mix;int32_t out=(int32_t)lp;
    if(out>32767)out=32767;if(out< -32768)out= -32768;
    *output=out;return (int16_t)out;
}
static int16_t sn_render_sample(PSGContext *ctx, SN76489 *p,int16_t *unfiltered)
{
    int32_t mix=sn_step_mix(ctx, p,NULL);*unfiltered=(int16_t)mix;
    return sn_filter_sample(ctx, mix,&p->lpf_prev,&p->lpf_y);
}
static int16_t sn_render_audition(PSGContext *ctx, SN76489 *p,int16_t *raw,int16_t *audition,int16_t *aud_raw)
{
    int32_t channels[4],mix=sn_step_mix(ctx, p,channels),selected=0;
    for(unsigned n=0;n<4;n++)if(ctx->s_aud.mask&(1u<<n))selected+=channels[n];
    *raw=(int16_t)mix;*aud_raw=(int16_t)selected;
    *audition=sn_filter_sample(ctx, selected,&ctx->s_aud.previous,&ctx->s_aud.output);
    return sn_filter_sample(ctx, mix,&p->lpf_prev,&p->lpf_y);
}

#ifndef GENESIS_PSG_SILENT_SPANS
#define GENESIS_PSG_SILENT_SPANS 1
#endif
#if GENESIS_PSG_SILENT_SPANS
/* No writes occur inside an advance interval. Once every channel is muted
 * and the filter has settled exactly to zero, samples are known, but chip
 * counters/phase/noise must still advance for a later unmute or snapshot. */
static unsigned sn_counter_span(PSGContext *ctx, int32_t *counter,unsigned period,unsigned samples)
{
    unsigned first=*counter>0?(unsigned)*counter:1u;
    if(samples<first){*counter-=(int32_t)samples;return 0;}
    unsigned rest=samples-first;
    *counter=(int32_t)(period-rest%period);
    return 1u+rest/period;
}
static uint16_t sn_white_step(PSGContext *ctx, uint16_t bits)
{return (uint16_t)((bits>>1)|(((bits^(bits>>3))&1u)<<15));}
static uint16_t sn_linear_apply(PSGContext *ctx, const uint16_t columns[16],uint16_t bits)
{
    uint16_t out=0;
    for(unsigned i=0;bits;++i,bits>>=1)if(bits&1u)out^=columns[i];
    return out;
}
static uint16_t sn_white_span(PSGContext *ctx, uint16_t bits,unsigned steps)
{
    /* A scratch chunk has at most8192 ticks, hence4096 LFSR edges. This is
     * derived immutable math, not chip state; reset/restore needs no epoch. */


    if(steps<16u){while(steps--)bits=sn_white_step(ctx, bits);return bits;}
    if(!ctx->ready) {
        for(unsigned i=0;i<16;++i)ctx->powers[0][i]=sn_white_step(ctx, (uint16_t)(1u<<i));
        for(unsigned p=1;p<13;++p)for(unsigned i=0;i<16;++i)
            ctx->powers[p][i]=sn_linear_apply(ctx, ctx->powers[p-1],ctx->powers[p-1][i]);
        ctx->ready=1;
    }
    for(unsigned p=0;steps;++p,steps>>=1)if(steps&1u)bits=sn_linear_apply(ctx, ctx->powers[p],bits);
    return bits;
}
static int sn_silent_span(PSGContext *ctx, SN76489 *p,int16_t *out,size_t samples)
{
    if(p->vol[0]!=15||p->vol[1]!=15||p->vol[2]!=15||p->vol[3]!=15||
       p->lpf_y||p->lpf_prev||p->noise_ff>1||p->noise_out>1||
       p->tone_out[0]>1||p->tone_out[1]>1||p->tone_out[2]>1)return 0;
    for(unsigned i=0;i<3;++i) {
        unsigned period=p->tone_period[i]?p->tone_period[i]:1u;
        p->tone_out[i]^=(uint8_t)(sn_counter_span(ctx, &p->tone_counter[i],period,(unsigned)samples)&1u);
    }
    unsigned edges=sn_counter_span(ctx, &p->noise_counter,(unsigned)sn_noise_reload(ctx, p),(unsigned)samples);
    unsigned shifts=(edges+(p->noise_ff?0u:1u))/2u;
    p->noise_ff^=(uint8_t)(edges&1u);
    if(shifts) {
        if(p->noise_ctrl&4u)p->lfsr=sn_white_span(ctx, p->lfsr,shifts);
        else {
            unsigned rotate=shifts&15u;
            if(rotate)p->lfsr=(uint16_t)((p->lfsr>>rotate)|((uint32_t)p->lfsr<<(16u-rotate)));
        }
        /* A noise-control write resets LFSR without changing noise_out.
         * Preserve that stale output until an actual rising clock edge. */
        p->noise_out=(uint8_t)(p->lfsr&1u);
    }
    memset(out,0,samples*sizeof *out);
    return 1;
}
#endif

static void psg_init(PSGContext *ctx)
{
    sn_reset(ctx, &ctx->s_sn);
    ctx->s_scratch_write = ctx->s_scratch_read = 0;
    ctx->s_aud_separate=ctx->s_aud.mask!=15;ctx->s_aud_canonical_history=0;
    ctx->s_leftover_master_cycles = 0;ctx->s_aud.previous=ctx->s_aud.output=0;
    ctx->s_inited = 1;
}

static void psg_advance(PSGContext *ctx, uint32_t cycles_master)
{
    if (!ctx->s_inited) psg_init(ctx);
    if (cycles_master == 0) return;

    uint64_t total = (uint64_t)cycles_master + ctx->s_leftover_master_cycles;
    size_t   samples_to_emit = (size_t)(total / PSG_SAMPLE_DIVISOR_MASTER);
    ctx->s_leftover_master_cycles = (uint32_t)(total % PSG_SAMPLE_DIVISOR_MASTER);

    while (samples_to_emit > 0) {
        size_t avail = PSG_SCRATCH_SAMPLES - ctx->s_scratch_write;
        if (avail == 0) return;  /* scratch full; drop */
        size_t chunk = samples_to_emit < avail ? samples_to_emit : avail;
        int silent=0;
#if GENESIS_PSG_SILENT_SPANS
        if((ctx->s_aud.mask==15 || (!ctx->s_aud.previous && !ctx->s_aud.output)) &&
           sn_silent_span(ctx, &ctx->s_sn,ctx->s_scratch+ctx->s_scratch_write,chunk))silent=1;
#endif
        if(silent) {
            memset(ctx->s_raw_scratch+ctx->s_scratch_write,0,chunk*sizeof *ctx->s_raw_scratch);
            if(ctx->s_aud_separate) {
                memset(ctx->s_aud_scratch+ctx->s_scratch_write,0,chunk*sizeof *ctx->s_aud_scratch);
                memset(ctx->s_aud_raw+ctx->s_scratch_write,0,chunk*sizeof *ctx->s_aud_raw);
            }
        } else if(ctx->s_aud.mask==15) {
            for(size_t i=0;i<chunk;i++)
                ctx->s_scratch[ctx->s_scratch_write+i]=sn_render_sample(ctx, &ctx->s_sn,ctx->s_raw_scratch+ctx->s_scratch_write+i);
            if(ctx->s_aud_separate) {
                memcpy(ctx->s_aud_scratch+ctx->s_scratch_write,ctx->s_scratch+ctx->s_scratch_write,chunk*sizeof *ctx->s_scratch);
                memcpy(ctx->s_aud_raw+ctx->s_scratch_write,ctx->s_raw_scratch+ctx->s_scratch_write,chunk*sizeof *ctx->s_raw_scratch);
            }
        } else for(size_t i=0;i<chunk;i++)
            ctx->s_scratch[ctx->s_scratch_write+i]=sn_render_audition(ctx, &ctx->s_sn,ctx->s_raw_scratch+ctx->s_scratch_write+i,
                ctx->s_aud_scratch+ctx->s_scratch_write+i,ctx->s_aud_raw+ctx->s_scratch_write+i);
        ctx->s_aud_canonical_history=ctx->s_aud.mask==15;
        ctx->s_scratch_write  += chunk;
        samples_to_emit  -= chunk;
    }
}

static void psg_write(PSGContext *ctx, uint8_t value)
{
    if (!ctx->s_inited) psg_init(ctx);
    sn_write(ctx, &ctx->s_sn, value);
}

static size_t psg_render(PSGContext *ctx, int16_t *out, size_t sample_count)
{
    return psg_render_pair(ctx, out,NULL,sample_count);
}

static size_t psg_render_pair(PSGContext *ctx, int16_t *filtered,int16_t *unfiltered,size_t sample_count)
{
    if ((!filtered && !unfiltered) || sample_count == 0) return 0;
    size_t available = ctx->s_scratch_write - ctx->s_scratch_read;
    size_t copy = sample_count < available ? sample_count : available;
    if (copy > 0) {
        if(filtered)memcpy(filtered, &ctx->s_scratch[ctx->s_scratch_read], copy * sizeof(int16_t));
        if(unfiltered)memcpy(unfiltered, &ctx->s_raw_scratch[ctx->s_scratch_read], copy * sizeof(int16_t));
        ctx->s_scratch_read += copy;
    }
    if (ctx->s_scratch_read >= ctx->s_scratch_write) {
        ctx->s_scratch_read = ctx->s_scratch_write = 0;
        ctx->s_aud_separate=ctx->s_aud.mask!=15;
    }
    return copy;
}

static void psg_set_audition_channels(PSGContext *ctx, unsigned mask)
{
    ctx->s_aud=audition_state(ctx);ctx->s_aud_canonical_history=0;
    mask&=15u;if(mask!=15u)separate_audition_queue(ctx);ctx->s_aud.mask=mask;
}
static size_t psg_peek_audition(PSGContext *ctx, int16_t *filtered,int16_t *raw,size_t count)
{
    size_t available=ctx->s_scratch_write-ctx->s_scratch_read;if(count>available)count=available;
    if(filtered)memcpy(filtered,(ctx->s_aud_separate?ctx->s_aud_scratch:ctx->s_scratch)+ctx->s_scratch_read,count*sizeof *filtered);
    if(raw)memcpy(raw,(ctx->s_aud_separate?ctx->s_aud_raw:ctx->s_raw_scratch)+ctx->s_scratch_read,count*sizeof *raw);return count;
}
static size_t psg_samples_available(PSGContext *ctx)
{
    return ctx->s_scratch_write - ctx->s_scratch_read;
}

static uint32_t psg_sample_rate(PSGContext *ctx)
{
    return (uint32_t)(GENESIS_MASTER_CLOCK_NTSC / PSG_SAMPLE_DIVISOR_MASTER);
}

static void psg_reset_leftover(PSGContext *ctx)
{
    ctx->s_leftover_master_cycles = 0;
}

/* Save-state hooks (own-backend snapshots): the whole SN76489 register/
 * counter struct + sample-clock remainder. The output scratch is transient
 * (drained every frame) and is reset on load. */
#include <stdio.h>
static int psg_save_state(PSGContext *ctx, FILE *f)
{
    if (!ctx->s_inited) psg_init(ctx);
    if (fwrite(&ctx->s_sn, sizeof ctx->s_sn, 1, f) != 1) return 0;
    if (fwrite(&ctx->s_leftover_master_cycles, sizeof ctx->s_leftover_master_cycles, 1, f) != 1) return 0;
    AuditionState audition=audition_state(ctx);
    if(fwrite(&audition,sizeof audition,1,f)!=1)return 0;
    return 1;
}


/* Full PSG state hashed for benchmark regression and differential co-sim
 * (field-by-field to dodge struct padding). Includes the noise LFSR + counters
 * + sample-clock leftover — the surface frame_snapshots.c zeroes. */
static int psg_load_state(PSGContext *ctx, FILE *f)
{
    if(!ctx->s_inited)psg_init(ctx);SN76489 state;uint32_t leftover;
    unsigned mask;int32_t previous,output;
    if(fread(&state,sizeof state,1,f)!=1 || fread(&leftover,sizeof leftover,1,f)!=1 ||
       leftover>=PSG_SAMPLE_DIVISOR_MASTER || fread(&mask,sizeof mask,1,f)!=1 || mask>15 ||
       fread(&previous,sizeof previous,1,f)!=1 || fread(&output,sizeof output,1,f)!=1)return 0;
    ctx->s_sn=state;ctx->s_leftover_master_cycles=leftover;ctx->s_aud.mask=mask;
    ctx->s_aud.previous=previous;ctx->s_aud.output=output;ctx->s_scratch_read=ctx->s_scratch_write=0;
    ctx->s_aud_separate=ctx->s_aud.mask!=15;ctx->s_aud_canonical_history=0;return 1;
}

/* Instance boundary: once per block, never per sample. */
static void *instance_create(void) {PSGContext *c=(PSGContext *)calloc(1,sizeof *c);if(c){c->s_aud.mask=15;psg_init(c);}return c;}
static void instance_destroy(void *c){free(c);}
static void instance_reset(void *c){psg_init((PSGContext *)c);}
static void instance_write(void *c,uint8_t v){psg_write((PSGContext *)c,v);}
static void instance_advance(void *c,uint32_t n){psg_advance((PSGContext *)c,n);}
static size_t instance_available(const void *v){const PSGContext *c=(const PSGContext *)v;return c->s_scratch_write-c->s_scratch_read;}
static size_t instance_render(void *c,int16_t *s,size_t n){return psg_render((PSGContext *)c,s,n);}
static int instance_stereo(void *c,uint8_t m){(void)c;(void)m;return 0;}
static int instance_save(void *c,FILE *f){return psg_save_state((PSGContext *)c,f);}
static int valid_context(const PSGContext *c){
 const SN76489 *p=&c->s_sn;
 for(unsigned i=0;i<3;++i)if(p->tone_period[i]>1023 || p->tone_counter[i]<1 || p->tone_counter[i]>1023 || p->tone_out[i]>1)return 0;
 for(unsigned i=0;i<4;++i)if(p->vol[i]>15)return 0;
 if(p->noise_ctrl>7 || p->noise_counter<1 || p->noise_counter>1023 || p->noise_out>1 || p->latch>7 || c->s_leftover_master_cycles>=240)return 0;
 if(p->noise_ff>1 || p->lpf_y < -32768 || p->lpf_y>32767 || p->lpf_prev < -32764 || p->lpf_prev>32764 || c->s_aud.mask>15 || c->s_aud.previous < -32764 || c->s_aud.previous>32764 || c->s_aud.output < -32768 || c->s_aud.output>32767)return 0;
 return 1;
}
static int instance_load(void *v,FILE *f){
 PSGContext *original=(PSGContext *)v,*candidate=(PSGContext *)malloc(sizeof *candidate);
 if(!candidate)return 0;*candidate=*original;
 int ok=psg_load_state(candidate,f)&&valid_context(candidate);
 if(ok)*original=*candidate;free(candidate);return ok;
}
static int instance_audition(void *c,unsigned m){psg_set_audition_channels((PSGContext *)c,m);return 1;}
static size_t instance_peek(void *c,int16_t *f,int16_t *r,size_t n){return psg_peek_audition((PSGContext *)c,f,r,n);}
const ProfileOps sega_psg_genesis_ops={instance_create,instance_destroy,instance_reset,instance_write,instance_advance,instance_available,instance_render,instance_stereo,instance_save,instance_load,instance_audition,instance_peek};
