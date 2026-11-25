#ifndef PSG_PROFILE_API_H
#define PSG_PROFILE_API_H
#include "sega_psg.h"
typedef struct {
    void *(*create)(void);
    void (*destroy)(void *);
    void (*reset)(void *);
    void (*write)(void *,uint8_t);
    void (*advance)(void *,uint32_t);
    size_t (*available)(const void *);
    size_t (*render)(void *,int16_t *,size_t);
    int (*stereo)(void *,uint8_t);
    int (*save)(void *,FILE *);
    int (*load)(void *,FILE *);
    int (*audition)(void *,unsigned);
    size_t (*peek)(void *,int16_t *,int16_t *,size_t);
} ProfileOps;
extern const ProfileOps sega_psg_genesis_ops, sega_psg_sms_ops;
#endif
