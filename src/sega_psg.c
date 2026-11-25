#include "profile_api.h"
#include <stdlib.h>
extern const ProfileOps sega_psg_genesis_ops, sega_psg_sms_ops;
struct sega_psg { const ProfileOps *ops; void *context; unsigned channels,divider; uint32_t clock; };
sega_psg *sega_psg_create(sega_psg_profile profile,uint32_t input_clock_hz) {
    if ((profile!=SEGA_PSG_GENESIS && profile!=SEGA_PSG_SMS_GG) || !input_clock_hz) return NULL;
    sega_psg *p=(sega_psg *)calloc(1,sizeof(*p));if(!p)return NULL;
    p->ops=profile==SEGA_PSG_GENESIS?&sega_psg_genesis_ops:&sega_psg_sms_ops;
    p->channels=profile==SEGA_PSG_GENESIS?1u:2u;p->divider=profile==SEGA_PSG_GENESIS?240u:16u;
    p->clock=input_clock_hz;p->context=p->ops->create();
    if(!p->context){free(p);return NULL;}return p;
}
void sega_psg_destroy(sega_psg *p){if(p){p->ops->destroy(p->context);free(p);}}
void sega_psg_reset(sega_psg *p){if(p)p->ops->reset(p->context);}
void sega_psg_write(sega_psg *p,uint8_t v){if(p)p->ops->write(p->context,v);}
int sega_psg_write_stereo(sega_psg *p,uint8_t m){return p?p->ops->stereo(p->context,m):0;}
void sega_psg_advance(sega_psg *p,uint32_t n){if(p)p->ops->advance(p->context,n);}
size_t sega_psg_available(const sega_psg *p){return p?p->ops->available(p->context):0;}
unsigned sega_psg_channels(const sega_psg *p){return p?p->channels:0;}
uint32_t sega_psg_sample_rate(const sega_psg *p){return p?p->clock/p->divider:0;}
size_t sega_psg_render(sega_psg *p,int16_t *s,size_t n){return p&&s?p->ops->render(p->context,s,n):0;}
int sega_psg_save(sega_psg *p,FILE *f){return p&&f?p->ops->save(p->context,f):0;}
int sega_psg_load(sega_psg *p,FILE *f){return p&&f?p->ops->load(p->context,f):0;}
int sega_psg_audition_channels(sega_psg *p,unsigned m){return p?p->ops->audition(p->context,m):0;}
size_t sega_psg_peek_audition(sega_psg *p,int16_t *f,int16_t *r,size_t n){return p?p->ops->peek(p->context,f,r,n):0;}
