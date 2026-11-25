#include "profile_api.h"
#include <stdlib.h>
#include <string.h>
#define GENESIS_MASTER_CLOCK_NTSC 53693175u
#define SMS_Z80_HZ 3579545u
#define SMS_PSG_DIVIDER 16
/*
 * sn76489.c — cycle-stamped SN76489 PSG (Sega Master System / Game Gear).
 *
 * Clean-room SN76489 (SMS/GG/Mega Drive variant) implemented from public
 * hardware documentation — NO emulator-core code, so this carries no third-
 * party copyleft. See LICENSING.md.
 *
 * Models 3 square-tone channels + 1 LFSR noise channel, 4-bit logarithmic
 * volume (2 dB/step, 0xF = off), and the SMS/GG 16-bit noise LFSR (white-noise
 * taps at bits 0 and 3, feedback -> bit 15), with a one-pole low-pass to round
 * the step edges.
 *
 * Clocking: the PSG runs at the Z80 clock (3.579545 MHz NTSC) with an internal
 * /16 divider, so it emits one frame per 16 Z80 T-states (~223.7 kHz). The
 * runner accumulates Z80 T-states and calls psg_advance(ctx) with the delta, so
 * the PSG advances in lockstep with the VDP through the same timing path.
 *
 * Output is STEREO so the Game Gear stereo register ($06) can route each
 * channel to L and/or R independently. On the SMS the mask stays all-on, so
 * both channels carry an identical signal.
 */


#include <string.h>

#define PSG_FRAME_DIVISOR_Z80   ((uint32_t)SMS_PSG_DIVIDER)   /* 16 T-states / frame */


     /* sub-frame Z80-cycle remainder */

#define PSG_SCRATCH_FRAMES  8192
   /* interleaved L,R */
                 /* in frames */
                 /* in frames */

/* ---- clean-room SN76489 ---- */
typedef struct {
    uint16_t tone_period[3];   /* 10-bit reload value */
    int32_t  tone_counter[3];
    uint8_t  tone_out[3];      /* square flip-flop (0/1) */
    uint8_t  vol[4];           /* 4-bit attenuation; index 3 = noise */
    uint8_t  noise_ctrl;       /* low 3 bits: fb-mode + rate */
    int32_t  noise_counter;
    uint16_t lfsr;
    uint8_t  noise_out;
    uint8_t  latch;            /* (chan<<1)|type of last LATCH byte */
    uint8_t  gg_stereo;        /* GG $06: hi nibble L-en, lo nibble R-en, per ch0..3 */
    int32_t  lpf_yL, lpf_prevL;   /* left low-pass state  */
    int32_t  lpf_yR, lpf_prevR;   /* right low-pass state */
} SN76489;
typedef struct PSGContext {
int s_inited;
uint32_t s_leftover_z80_cycles;
int16_t s_scratch[PSG_SCRATCH_FRAMES * 2];
size_t s_scratch_write;
size_t s_scratch_read;
SN76489 s_sn;
int32_t s_mixL[16], s_mixR[16];
unsigned s_audible;
int s_mono_mix;

} PSGContext;
static void psg_dump_state_text(PSGContext *ctx, FILE *f);
static void sn_reset(PSGContext *ctx, SN76489 *p);
static inline void sn_tick(PSGContext *ctx, SN76489 *p, int32_t amp[4]);
static inline int16_t sn_lpf(PSGContext *ctx, int32_t mix, int32_t *y, int32_t *prev);
static void sn_render_frame(PSGContext *ctx, SN76489 *p, int16_t *l, int16_t *r);
static void sn_prepare_mix(PSGContext *ctx, const SN76489 *p);
static void sn_clock(PSGContext *ctx, SN76489 *p);
static void sn_skip_tone(PSGContext *ctx, SN76489 *p,unsigned i,unsigned frames);
static void sn_skip_noise(PSGContext *ctx, SN76489 *p,unsigned frames);
static void sn_filter_span(PSGContext *ctx, SN76489 *p,int16_t *out,size_t frames,int32_t l,int32_t r);
static void sn_render_block(PSGContext *ctx, SN76489 *p,int16_t *out,size_t frames);
static void psg_init(PSGContext *ctx);
static void psg_advance(PSGContext *ctx, uint32_t cycles_z80);
static void psg_write(PSGContext *ctx, uint8_t value);
static void psg_write_stereo(PSGContext *ctx, uint8_t mask);
static size_t psg_render(PSGContext *ctx, int16_t *out, size_t frame_count);
static size_t psg_frames_available(PSGContext *ctx);
static void psg_drain(PSGContext *ctx, void (*sink)(const int16_t *, size_t));
static uint32_t psg_sample_rate(PSGContext *ctx);
static void psg_reset_leftover(PSGContext *ctx);
static int psg_save_state(PSGContext *ctx, FILE *f);
static int psg_load_state(PSGContext *ctx, FILE *f);

#include "registers.inc"






/* First-order low-pass matching the reference (clownmdemu low-pass-filter.c
 * with coefficients 26.044, 24.044): out = ((in + prevIn)*SAMPLE_MAGIC +
 * prevOut*OUTPUT_MAGIC) >> 16. */
#define SN_LPF_SAMPLE_MAGIC  2517   /* round(1.0    * 65536 / 26.044) */
#define SN_LPF_OUTPUT_MAGIC  60504  /* round(24.044 * 65536 / 26.044) */


/* 4-bit attenuation -> linear amplitude. 2 dB/step (×10^-0.1), 0xF = silence.
 * Peak 0x1FFF with per-channel /4 headroom so 4 summed channels (4*8191 =
 * 32764) just fit int16 on either stereo side. */
#include "attenuation.inc"

/* Measurement-only: dump the LATCHED register state (the MMIO-written values)
 * as labeled text, for the recomp-vs-Mesen PSG state diff (Axis 5, no waveform).
 * lfsr/counters are sub-cycle state and may jitter; the periods/vols are the
 * values the CPU actually wrote and should match the oracle exactly. */
static void psg_dump_state_text(PSGContext *ctx, FILE *f)
{
    const SN76489 *p = &ctx->s_sn;
    fprintf(f, "tone0_period=%u\n", p->tone_period[0]);
    fprintf(f, "tone1_period=%u\n", p->tone_period[1]);
    fprintf(f, "tone2_period=%u\n", p->tone_period[2]);
    fprintf(f, "tone0_vol=%u\n", p->vol[0]);
    fprintf(f, "tone1_vol=%u\n", p->vol[1]);
    fprintf(f, "tone2_vol=%u\n", p->vol[2]);
    fprintf(f, "noise_vol=%u\n", p->vol[3]);
    fprintf(f, "noise_ctrl=%u\n", p->noise_ctrl);
    fprintf(f, "lfsr=%u\n", p->lfsr);
    fprintf(f, "gg_panning=%u\n", p->gg_stereo);
}

static void sn_reset(PSGContext *ctx, SN76489 *p)
{
    memset(p, 0, sizeof *p);
    for (int i = 0; i < 3; i++) { p->tone_period[i] = 1; p->tone_counter[i] = 1; }
    p->vol[0] = p->vol[1] = p->vol[2] = p->vol[3] = 0xF;  /* all silent */
    p->noise_counter = 0x10;
    p->lfsr = 0x8000;
    p->gg_stereo = 0xFF;       /* every channel on both sides (SMS / GG default) */
}





/* Advance all four channels by one frame, returning each channel's signed
 * amplitude in amp[0..3] (tone0, tone1, tone2, noise). The per-frame state
 * mutation (counters, flip-flops, LFSR) happens here exactly once. */
#ifdef SMS_PSG_REFERENCE
static inline void sn_tick(PSGContext *ctx, SN76489 *p, int32_t amp[4])
{
    for (int i = 0; i < 3; i++) {
        if (--p->tone_counter[i] <= 0) {
            p->tone_counter[i] = p->tone_period[i] ? p->tone_period[i] : 1;
            p->tone_out[i] ^= 1;
        }
        amp[i] = p->tone_out[i] ? SN_VOL[p->vol[i]] : -SN_VOL[p->vol[i]];
    }

    if (--p->noise_counter <= 0) {
        p->noise_counter = sn_noise_reload(ctx, p);
        uint16_t fb = (p->noise_ctrl & 0x04)            /* 1 = white */
                    ? ((p->lfsr ^ (p->lfsr >> 3)) & 1)  /* taps bit0 ^ bit3 */
                    : (p->lfsr & 1);                    /* periodic */
        p->lfsr = (uint16_t)((p->lfsr >> 1) | (fb << 15));
        p->noise_out = (uint8_t)(p->lfsr & 1);
    }
    amp[3] = p->noise_out ? SN_VOL[p->vol[3]] : -SN_VOL[p->vol[3]];
}
#endif

static inline int16_t sn_lpf(PSGContext *ctx, int32_t mix, int32_t *y, int32_t *prev)
{
    int64_t lp = ((int64_t)(mix + *prev) * SN_LPF_SAMPLE_MAGIC
                + (int64_t)(*y) * SN_LPF_OUTPUT_MAGIC) / 65536;
    *prev = mix;
    int32_t out = (int32_t)lp;
    if (out >  32767) out =  32767;
    if (out < -32768) out = -32768;
    *y = out;
    return (int16_t)out;
}

/* Render one stereo frame, applying the GG stereo routing mask. */
#ifdef SMS_PSG_REFERENCE
static void sn_render_frame(PSGContext *ctx, SN76489 *p, int16_t *l, int16_t *r)
{
    int32_t amp[4];
    sn_tick(ctx, p, amp);

    int32_t mixL = 0, mixR = 0;
    uint8_t m = p->gg_stereo;
    for (int i = 0; i < 4; i++) {
        if (m & (uint8_t)(0x10 << i)) mixL += amp[i];   /* left  enables: bits 4-7 */
        if (m & (uint8_t)(0x01 << i)) mixR += amp[i];   /* right enables: bits 0-3 */
    }

    *l = sn_lpf(ctx, mixL, &p->lpf_yL, &p->lpf_prevL);
    *r = sn_lpf(ctx, mixR, &p->lpf_yR, &p->lpf_prevR);
}
#endif

#ifndef SMS_PSG_REFERENCE
/* Derived host data, never serialized: output bits select one of sixteen
 * exact signed stereo mixes. Only volume or routing changes invalidate it. */



static void sn_prepare_mix(PSGContext *ctx, const SN76489 *p)
{
    ctx->s_audible=0;ctx->s_mono_mix=1;
    for(unsigned bits=0;bits<16;bits++){
        int32_t l=0,r=0;
        for(unsigned i=0;i<4;i++){
            int32_t amp=(bits&(1u<<i))?SN_VOL[p->vol[i]]:-SN_VOL[p->vol[i]];
            if(p->gg_stereo&(0x10u<<i))l+=amp;
            if(p->gg_stereo&(1u<<i))r+=amp;
            if(SN_VOL[p->vol[i]] && (p->gg_stereo&((0x10u<<i)|(1u<<i))))ctx->s_audible|=1u<<i;
        }
        ctx->s_mixL[bits]=l;ctx->s_mixR[bits]=r;
        if(l!=r)ctx->s_mono_mix=0;
    }
}
static void sn_clock(PSGContext *ctx, SN76489 *p)
{
    for(unsigned i=0;i<3;i++)if(--p->tone_counter[i]<=0){
        p->tone_counter[i]=p->tone_period[i]?p->tone_period[i]:1;
        p->tone_out[i]^=1;
    }
    if(--p->noise_counter<=0){
        p->noise_counter=sn_noise_reload(ctx, p);
        uint16_t fb=(p->noise_ctrl&4)?((p->lfsr^(p->lfsr>>3))&1):(p->lfsr&1);
        p->lfsr=(uint16_t)((p->lfsr>>1)|(fb<<15));
        p->noise_out=(uint8_t)(p->lfsr&1);
    }
}
/* Advance unobserved clocks analytically. An inaudible oscillator still has
 * exactly the same counter/flip-flop state when a later write makes it audible. */
static void sn_skip_tone(PSGContext *ctx, SN76489 *p,unsigned i,unsigned frames)
{
    unsigned wait=p->tone_counter[i]>0?(unsigned)p->tone_counter[i]:1;
    if(frames<wait){p->tone_counter[i]-=(int32_t)frames;return;}
    unsigned reload=p->tone_period[i]?p->tone_period[i]:1,after=frames-wait;
    p->tone_counter[i]=(int32_t)(reload-after%reload);
    p->tone_out[i]^=(uint8_t)((1+after/reload)&1);
}
static void sn_skip_noise(PSGContext *ctx, SN76489 *p,unsigned frames)
{
    unsigned wait=p->noise_counter>0?(unsigned)p->noise_counter:1;
    if(frames<wait){p->noise_counter-=(int32_t)frames;return;}
    unsigned reload=(unsigned)sn_noise_reload(ctx, p),after=frames-wait,edges=1+after/reload;
    p->noise_counter=(int32_t)(reload-after%reload);
    while(edges--){
        uint16_t fb=(p->noise_ctrl&4)?((p->lfsr^(p->lfsr>>3))&1):(p->lfsr&1);
        p->lfsr=(uint16_t)((p->lfsr>>1)|(fb<<15));
    }
    p->noise_out=(uint8_t)(p->lfsr&1);
}
static void sn_filter_span(PSGContext *ctx, SN76489 *p,int16_t *out,size_t frames,int32_t l,int32_t r)
{
    if(!l && !r && !p->lpf_yL && !p->lpf_yR && !p->lpf_prevL && !p->lpf_prevR){
        memset(out,0,frames*2*sizeof *out);return;
    }
    if(l==r && p->lpf_yL==p->lpf_yR && p->lpf_prevL==p->lpf_prevR){
        int32_t y=p->lpf_yL,prev=p->lpf_prevL;
        for(size_t i=0;i<frames;i++){
            int16_t sample=sn_lpf(ctx, l,&y,&prev);out[i*2]=out[i*2+1]=sample;
        }
        p->lpf_yL=p->lpf_yR=y;p->lpf_prevL=p->lpf_prevR=prev;
    }else{
        for(size_t i=0;i<frames;i++){
            out[i*2]=sn_lpf(ctx, l,&p->lpf_yL,&p->lpf_prevL);
            out[i*2+1]=sn_lpf(ctx, r,&p->lpf_yR,&p->lpf_prevR);
        }
    }
}
static void sn_render_block(PSGContext *ctx, SN76489 *p,int16_t *out,size_t frames)
{
    /* An audible reload-1 oscillator makes every sample an event. Use the
     * same cached native mixer directly rather than searching for empty spans. */
    int every_sample=0;
    for(unsigned i=0;i<3;i++)if((ctx->s_audible&(1u<<i)) && p->tone_period[i]<=1 && p->tone_counter[i]<=1)every_sample=1;
    if(every_sample){
        if(ctx->s_mono_mix && p->lpf_yL==p->lpf_yR && p->lpf_prevL==p->lpf_prevR){
            int32_t y=p->lpf_yL,prev=p->lpf_prevL;
            for(size_t i=0;i<frames;i++){
                sn_clock(ctx, p);
                unsigned bits=p->tone_out[0]|(p->tone_out[1]<<1)|(p->tone_out[2]<<2)|(p->noise_out<<3);
                int16_t sample=sn_lpf(ctx, ctx->s_mixL[bits],&y,&prev);out[i*2]=out[i*2+1]=sample;
            }
            p->lpf_yL=p->lpf_yR=y;p->lpf_prevL=p->lpf_prevR=prev;
        }else{
            for(size_t i=0;i<frames;i++){
                sn_clock(ctx, p);
                unsigned bits=p->tone_out[0]|(p->tone_out[1]<<1)|(p->tone_out[2]<<2)|(p->noise_out<<3);
                out[i*2]=sn_lpf(ctx, ctx->s_mixL[bits],&p->lpf_yL,&p->lpf_prevL);
                out[i*2+1]=sn_lpf(ctx, ctx->s_mixR[bits],&p->lpf_yR,&p->lpf_prevR);
            }
        }
        return;
    }
    while(frames){
        sn_clock(ctx, p);
        unsigned bits=p->tone_out[0]|(p->tone_out[1]<<1)|(p->tone_out[2]<<2)|(p->noise_out<<3);
        size_t span=frames;
        /* This first sample has already clocked. Outputs remain constant for
         * counter-1 more samples. Muted channels do not split the audio job. */
        for(unsigned i=0;i<3;i++)if((ctx->s_audible&(1u<<i)) && (size_t)p->tone_counter[i]<span)span=(size_t)p->tone_counter[i];
        if((ctx->s_audible&8) && (size_t)p->noise_counter<span)span=(size_t)p->noise_counter;
        if(span>1){
            unsigned skipped=(unsigned)(span-1);
            for(unsigned i=0;i<3;i++)sn_skip_tone(ctx, p,i,skipped);
            sn_skip_noise(ctx, p,skipped);
        }
        sn_filter_span(ctx, p,out,span,ctx->s_mixL[bits],ctx->s_mixR[bits]);
        out+=span*2;frames-=span;
    }
}
#endif

static void psg_init(PSGContext *ctx)
{
    sn_reset(ctx, &ctx->s_sn);
#ifndef SMS_PSG_REFERENCE
    sn_prepare_mix(ctx, &ctx->s_sn);
#endif
    ctx->s_scratch_write = ctx->s_scratch_read = 0;
    ctx->s_leftover_z80_cycles = 0;
    ctx->s_inited = 1;
}

static void psg_advance(PSGContext *ctx, uint32_t cycles_z80)
{
    if (!ctx->s_inited) psg_init(ctx);
    if (cycles_z80 == 0) return;

    uint64_t total = (uint64_t)cycles_z80 + ctx->s_leftover_z80_cycles;
    size_t   frames_to_emit = (size_t)(total / PSG_FRAME_DIVISOR_Z80);
    ctx->s_leftover_z80_cycles = (uint32_t)(total % PSG_FRAME_DIVISOR_Z80);

    while (frames_to_emit > 0) {
        size_t avail = PSG_SCRATCH_FRAMES - ctx->s_scratch_write;
        if (avail == 0) return;  /* scratch full; drop (no sink draining) */
        size_t chunk = frames_to_emit < avail ? frames_to_emit : avail;
#ifdef SMS_PSG_REFERENCE
        for (size_t i = 0; i < chunk; i++)
            sn_render_frame(ctx, &ctx->s_sn,
                            &ctx->s_scratch[(ctx->s_scratch_write + i) * 2],
                            &ctx->s_scratch[(ctx->s_scratch_write + i) * 2 + 1]);
#else
        sn_render_block(ctx, &ctx->s_sn,&ctx->s_scratch[ctx->s_scratch_write*2],chunk);
#endif
        ctx->s_scratch_write += chunk;
        frames_to_emit  -= chunk;
    }
}

static void psg_write(PSGContext *ctx, uint8_t value)
{
    if (!ctx->s_inited) psg_init(ctx);
#ifndef SMS_PSG_REFERENCE
    unsigned latched=(value&0x80)?(value>>4)&7:ctx->s_sn.latch;
    int remix=(latched&1) && ctx->s_sn.vol[latched>>1]!=(value&15);
#endif
    sn_write(ctx, &ctx->s_sn, value);
#ifndef SMS_PSG_REFERENCE
    if(remix)sn_prepare_mix(ctx, &ctx->s_sn);
#endif
}

static void psg_write_stereo(PSGContext *ctx, uint8_t mask)
{
    if (!ctx->s_inited) psg_init(ctx);
    if(ctx->s_sn.gg_stereo==mask)return;
    ctx->s_sn.gg_stereo = mask;
#ifndef SMS_PSG_REFERENCE
    sn_prepare_mix(ctx, &ctx->s_sn);
#endif
}

static size_t psg_render(PSGContext *ctx, int16_t *out, size_t frame_count)
{
    if (!out || frame_count == 0) return 0;
    size_t available = ctx->s_scratch_write - ctx->s_scratch_read;
    size_t copy = frame_count < available ? frame_count : available;
    if (copy > 0) {
        memcpy(out, &ctx->s_scratch[ctx->s_scratch_read * 2], copy * 2 * sizeof(int16_t));
        ctx->s_scratch_read += copy;
    }
    if (ctx->s_scratch_read >= ctx->s_scratch_write) {
        ctx->s_scratch_read = ctx->s_scratch_write = 0;
    }
    return copy;
}

static size_t psg_frames_available(PSGContext *ctx)
{
    return ctx->s_scratch_write - ctx->s_scratch_read;
}

static void psg_drain(PSGContext *ctx, void (*sink)(const int16_t *, size_t))
{
    size_t available=ctx->s_scratch_write-ctx->s_scratch_read;
    if(sink && available){
        sink(&ctx->s_scratch[ctx->s_scratch_read*2],available);
        ctx->s_scratch_write=ctx->s_scratch_read=0;
    }
}

static uint32_t psg_sample_rate(PSGContext *ctx)
{
    return (uint32_t)(SMS_Z80_HZ / PSG_FRAME_DIVISOR_Z80);
}

static void psg_reset_leftover(PSGContext *ctx)
{
    ctx->s_leftover_z80_cycles = 0;
}

/* Save-state hooks (own-backend snapshots): the whole SN76489 register/counter
 * struct + sub-frame cycle remainder. The output scratch is transient (drained
 * every frame) and is reset on load. */
#include <stdio.h>
static int psg_save_state(PSGContext *ctx, FILE *f)
{
    if (!ctx->s_inited) psg_init(ctx);
    if (fwrite(&ctx->s_sn, sizeof ctx->s_sn, 1, f) != 1) return 0;
    if (fwrite(&ctx->s_leftover_z80_cycles, sizeof ctx->s_leftover_z80_cycles, 1, f) != 1) return 0;
    return 1;
}

static int psg_load_state(PSGContext *ctx, FILE *f)
{
    if (!ctx->s_inited) psg_init(ctx);
    SN76489 loaded;uint32_t leftover;
    if(fread(&loaded,sizeof loaded,1,f)!=1 || fread(&leftover,sizeof leftover,1,f)!=1)return 0;
    /* Derived table indices and filter arithmetic must never trust malformed
     * input. Valid current-build states use these ranges, including period 0. */
    for(unsigned i=0;i<3;i++)if(loaded.tone_period[i]>1023 || loaded.tone_counter[i]<1 ||
        loaded.tone_counter[i]>1023 || loaded.tone_out[i]>1)return 0;
    for(unsigned i=0;i<4;i++)if(loaded.vol[i]>15)return 0;
    if(loaded.noise_ctrl>7 || loaded.noise_counter<1 || loaded.noise_counter>1023 ||
       loaded.noise_out>1 || loaded.latch>7 || leftover>=PSG_FRAME_DIVISOR_Z80 ||
       loaded.lpf_yL< -32768 || loaded.lpf_yL>32767 || loaded.lpf_yR< -32768 || loaded.lpf_yR>32767 ||
       loaded.lpf_prevL< -32764 || loaded.lpf_prevL>32764 || loaded.lpf_prevR< -32764 || loaded.lpf_prevR>32764)return 0;
    memcpy(&ctx->s_sn,&loaded,sizeof ctx->s_sn);ctx->s_leftover_z80_cycles=leftover;
#ifndef SMS_PSG_REFERENCE
    sn_prepare_mix(ctx, &ctx->s_sn);
#endif
    ctx->s_scratch_read = ctx->s_scratch_write = 0;
    return 1;
}

/* Instance boundary: once per block, never per sample. */
static void *instance_create(void) {PSGContext *c=(PSGContext *)calloc(1,sizeof *c);if(c){psg_init(c);}return c;}
static void instance_destroy(void *c){free(c);}
static void instance_reset(void *c){psg_init((PSGContext *)c);}
static void instance_write(void *c,uint8_t v){psg_write((PSGContext *)c,v);}
static void instance_advance(void *c,uint32_t n){psg_advance((PSGContext *)c,n);}
static size_t instance_available(const void *v){const PSGContext *c=(const PSGContext *)v;return c->s_scratch_write-c->s_scratch_read;}
static size_t instance_render(void *c,int16_t *s,size_t n){return psg_render((PSGContext *)c,s,n);}
static int instance_stereo(void *c,uint8_t m){psg_write_stereo((PSGContext *)c,m);return 1;}
static int instance_save(void *c,FILE *f){return psg_save_state((PSGContext *)c,f);}
static int valid_context(const PSGContext *c){
 const SN76489 *p=&c->s_sn;
 for(unsigned i=0;i<3;++i)if(p->tone_period[i]>1023 || p->tone_counter[i]<1 || p->tone_counter[i]>1023 || p->tone_out[i]>1)return 0;
 for(unsigned i=0;i<4;++i)if(p->vol[i]>15)return 0;
 if(p->noise_ctrl>7 || p->noise_counter<1 || p->noise_counter>1023 || p->noise_out>1 || p->latch>7 || c->s_leftover_z80_cycles>=16)return 0;
 if(p->lpf_yL < -32768 || p->lpf_yL>32767 || p->lpf_yR < -32768 || p->lpf_yR>32767 || p->lpf_prevL < -32764 || p->lpf_prevL>32764 || p->lpf_prevR < -32764 || p->lpf_prevR>32764)return 0;
 return 1;
}
static int instance_load(void *v,FILE *f){
 PSGContext *original=(PSGContext *)v,*candidate=(PSGContext *)malloc(sizeof *candidate);
 if(!candidate)return 0;*candidate=*original;
 int ok=psg_load_state(candidate,f)&&valid_context(candidate);
 if(ok)*original=*candidate;free(candidate);return ok;
}
static int instance_audition(void *c,unsigned m){(void)c;(void)m;return 0;}
static size_t instance_peek(void *c,int16_t *f,int16_t *r,size_t n){(void)c;(void)f;(void)r;(void)n;return 0;}
const ProfileOps sega_psg_sms_ops={instance_create,instance_destroy,instance_reset,instance_write,instance_advance,instance_available,instance_render,instance_stereo,instance_save,instance_load,instance_audition,instance_peek};
