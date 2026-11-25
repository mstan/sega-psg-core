#define psg_advance oracle_genesis_advance
#define psg_cosim_dump oracle_genesis_cosim_dump
#define psg_cosim_hash oracle_genesis_cosim_hash
#define psg_init oracle_genesis_init
#define psg_load_state oracle_genesis_load_state
#define psg_peek_audition oracle_genesis_peek_audition
#define psg_rb_load oracle_genesis_rb_load
#define psg_rb_save oracle_genesis_rb_save
#define psg_render oracle_genesis_render
#define psg_render_pair oracle_genesis_render_pair
#define psg_reset_leftover oracle_genesis_reset_leftover
#define psg_sample_rate oracle_genesis_sample_rate
#define psg_samples_available oracle_genesis_samples_available
#define psg_save_state oracle_genesis_save_state
#define psg_set_audition_channels oracle_genesis_set_audition_channels
#define psg_volumes oracle_genesis_volumes
#define psg_write oracle_genesis_write
#include <stdint.h>
#include <stddef.h>
size_t oracle_genesis_render_pair(int16_t *,int16_t *,size_t);
#include "oracles/genesis/sn76489.c"
