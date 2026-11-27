#include "sega_psg.h"
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"qualified API line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
void oracle_genesis_init(void);
void oracle_genesis_write(uint8_t);
void oracle_genesis_advance(uint32_t);
void oracle_genesis_set_audition_channels(unsigned);
void oracle_genesis_reset_leftover(void);
size_t oracle_genesis_render_pair(int16_t *,int16_t *,size_t);
size_t oracle_genesis_rb_save(void *,size_t);
int oracle_genesis_rb_load(const void *,size_t);
uint64_t oracle_genesis_cosim_hash(void);
int oracle_genesis_cosim_dump(char *,int);
void oracle_sms_init(void);
void oracle_sms_write(uint8_t);
void oracle_sms_write_stereo(uint8_t);
void oracle_sms_advance(uint32_t);
void oracle_sms_reset_leftover(void);
void oracle_sms_dump_state_text(FILE *);
size_t oracle_sms_render(int16_t *,size_t);
static void queued_state(sega_psg *s) {
 unsigned char a[66000],b[66000];
 size_t n=sega_psg_rb_save(s,a,sizeof a),m=oracle_genesis_rb_save(b,sizeof b);
 CHECK(n&&n==m&&!memcmp(a,b,n));
 CHECK(sega_psg_state_hash(s)==oracle_genesis_cosim_hash());
 char x[1024],y[1024];
 CHECK(sega_psg_state_dump(s,x,sizeof x)==oracle_genesis_cosim_dump(y,sizeof y));
 CHECK(!strcmp(x,y));
}
static void genesis(void) {
 sega_psg *s=sega_psg_create(SEGA_PSG_GENESIS,53693175);CHECK(s);
 for(unsigned mask=0;mask<16;mask++) {
  sega_psg_reset(s);oracle_genesis_init();
  CHECK(sega_psg_audition_channels(s,mask));oracle_genesis_set_audition_channels(mask);
  for(unsigned c=0;c<4;c++) { uint8_t v=(uint8_t)(0x90|(c<<5)|c);sega_psg_write(s,v);oracle_genesis_write(v); }
  sega_psg_write(s,0xe4);oracle_genesis_write(0xe4);
  sega_psg_advance(s,240*8192+137);oracle_genesis_advance(240*8192+137);queued_state(s);
  int16_t a[8192],b[8192],raw_a[8192],raw_b[8192];
  CHECK(sega_psg_render_pair(s,a,raw_a,37)==oracle_genesis_render_pair(b,raw_b,37));
  CHECK(!memcmp(a,b,37*sizeof *a)&&!memcmp(raw_a,raw_b,37*sizeof *a));queued_state(s);
  unsigned char saved[66000],after[66000],bad[66000];size_t n=sega_psg_rb_save(s,saved,sizeof saved);
  CHECK(n&&sega_psg_rb_save(s,NULL,0)==n);
  CHECK(!sega_psg_rb_save(s,bad,n-1));
  memcpy(bad,saved,n);bad[0]=bad[1]=255;
  CHECK(!sega_psg_rb_load(s,bad,n));CHECK(!sega_psg_rb_load(s,saved,n-1));
  CHECK(sega_psg_rb_save(s,after,sizeof after)==n&&!memcmp(saved,after,n));
  sega_psg_advance(s,UINT32_MAX);oracle_genesis_advance(UINT32_MAX);
  CHECK(sega_psg_rb_load(s,saved,n)&&oracle_genesis_rb_load(saved,n));queued_state(s);
  sega_psg_reset_leftover(s);oracle_genesis_reset_leftover();queued_state(s);
  CHECK(sega_psg_render_pair(s,a,raw_a,8192)==oracle_genesis_render_pair(b,raw_b,8192));
  CHECK(!memcmp(a,b,8155*sizeof *a)&&!memcmp(raw_a,raw_b,8155*sizeof *a));queued_state(s);
 }
 sega_psg_destroy(s);
}
static void sms(void) {
 sega_psg *s=sega_psg_create(SEGA_PSG_SMS_GG,3579545);CHECK(s);oracle_sms_init();
 sega_psg_write(s,0x91);oracle_sms_write(0x91);
 CHECK(sega_psg_write_stereo(s,0x96));oracle_sms_write_stereo(0x96);
 sega_psg_advance(s,16*123+15);oracle_sms_advance(16*123+15);
 sega_psg_reset_leftover(s);oracle_sms_reset_leftover();
 sega_psg_advance(s,1);oracle_sms_advance(1);
 int16_t a[512],b[512];CHECK(sega_psg_available(s)==123);
 CHECK(!sega_psg_render_pair(s,a,b,1)&&!sega_psg_rb_save(s,a,sizeof a));
 CHECK(!sega_psg_rb_load(s,a,sizeof a)&&!sega_psg_state_hash(s));
 CHECK(sega_psg_available(s)==123);
 CHECK(sega_psg_render(s,a,256)==oracle_sms_render(b,256));CHECK(!memcmp(a,b,246*sizeof *a));
 char x[1024],y[1024]={0};FILE *f=tmpfile();CHECK(f);oracle_sms_dump_state_text(f);
 long n=ftell(f);CHECK(n>0&&n<(long)sizeof y);rewind(f);CHECK(fread(y,1,(size_t)n,f)==(size_t)n);fclose(f);
 CHECK(sega_psg_state_dump(s,x,sizeof x)==n&&!strcmp(x,y));
 CHECK(!sega_psg_state_dump(s,x,1)&&!sega_psg_state_dump(s,NULL,0));
 sega_psg_destroy(s);
}
int main(void) {
 genesis();sms();
 puts("Qualified PSG API: queued paired rollback, transactional rejection, hash/dump, both clock resets and profile support PASS");
 return 0;
}
