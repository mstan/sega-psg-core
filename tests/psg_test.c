#include "sega_psg.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d failed %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
#define DECL(P) \
void oracle_##P##_init(void); \
void oracle_##P##_write(uint8_t); \
void oracle_##P##_advance(uint32_t); \
size_t oracle_##P##_render(int16_t *,size_t); \
int oracle_##P##_save_state(FILE *); \
int oracle_##P##_load_state(FILE *);
DECL(genesis)
DECL(sms)
void oracle_sms_write_stereo(uint8_t);
void oracle_genesis_set_audition_channels(unsigned);
size_t oracle_genesis_peek_audition(int16_t *,int16_t *,size_t);
static int cases;
static void init(int p){if(p)oracle_sms_init();else oracle_genesis_init();}
static void write_pair(sega_psg *s,int p,uint8_t b){sega_psg_write(s,b);if(p)oracle_sms_write(b);else oracle_genesis_write(b);}
static void advance_pair(sega_psg *s,int p,uint32_t n){sega_psg_advance(s,n);if(p)oracle_sms_advance(n);else oracle_genesis_advance(n);}
static void drain_pair(sega_psg *s,int p){
 int16_t a[16384],b[16384];size_t n=sega_psg_render(s,a,8192);
 size_t m=p?oracle_sms_render(b,8192):oracle_genesis_render(b,8192);
 CHECK(n==m);CHECK(!memcmp(a,b,n*sega_psg_channels(s)*sizeof(*a)));CHECK(sega_psg_available(s)==0);
}
static void state_pair(sega_psg *s,int p){
 FILE *a=tmpfile(),*b=tmpfile();CHECK(a&&b);
 CHECK(sega_psg_save(s,a));CHECK(p?oracle_sms_save_state(b):oracle_genesis_save_state(b));
 CHECK(ftell(a)==ftell(b));rewind(a);rewind(b);int x,y;
 do{x=fgetc(a);y=fgetc(b);CHECK(x==y);}while(x!=EOF);
 rewind(a);rewind(b);CHECK(sega_psg_load(s,a));CHECK(p?oracle_sms_load_state(b):oracle_genesis_load_state(b));
 fclose(a);fclose(b);
}
static void scenario(int p,unsigned noise,unsigned level){
 sega_psg *s=sega_psg_create((sega_psg_profile)p,p?3579545u:53693175u);CHECK(s);init(p);
 for(unsigned ch=0;ch<3;ch++){
  unsigned period=(noise*137u+level*31u+ch*11u)&1023u;
  write_pair(s,p,(uint8_t)(0x80u|(ch<<5)|(period&15u)));write_pair(s,p,(uint8_t)(period>>4));
  write_pair(s,p,(uint8_t)(0x90u|(ch<<5)|level));
 }
 write_pair(s,p,(uint8_t)(0xe0u|noise));write_pair(s,p,(uint8_t)(0xf0u|level));
 const unsigned divider=p?16u:240u;
 for(unsigned n=0;n<31;n++){
  if(p){uint8_t mask=(uint8_t)(n*37u);CHECK(sega_psg_write_stereo(s,mask));oracle_sms_write_stereo(mask);}
  advance_pair(s,p,divider*(17u+n*7u)+(n%divider));drain_pair(s,p);
 }
 /* All latch/data values, stale latch writes, mixed sub-frame clock remainders. */
 for(unsigned n=0;n<256;n++){
  write_pair(s,p,(uint8_t)n);advance_pair(s,p,divider*(n%29u)+n%divider);drain_pair(s,p);
 }
 state_pair(s,p);
 /* Scratch saturation retains donor behavior; no silent capacity policy change. */
 advance_pair(s,p,divider*9000u);drain_pair(s,p);state_pair(s,p);
 if(!p){
  CHECK(!sega_psg_write_stereo(s,0));
  for(unsigned mask=0;mask<16;mask++){
   CHECK(sega_psg_audition_channels(s,mask));oracle_genesis_set_audition_channels(mask);
   advance_pair(s,p,divider*77u);int16_t a[200],b[200],rawa[200],rawb[200];
   size_t na=sega_psg_peek_audition(s,a,rawa,200),nb=oracle_genesis_peek_audition(b,rawb,200);
   CHECK(na==nb);CHECK(!memcmp(a,b,na*sizeof(*a)));CHECK(!memcmp(rawa,rawb,na*sizeof(*a)));drain_pair(s,p);
  }
 }
 sega_psg_reset(s);init(p);advance_pair(s,p,divider*100u);drain_pair(s,p);state_pair(s,p);
 /* Full scratch keeps the donor drop policy and frame remainder. */
 advance_pair(s,p,(p?16u:240u)*9000u+7u);CHECK(sega_psg_available(s)==8192);
 int16_t actual[16384],expected[16384];size_t count=sega_psg_render(s,actual,8192);
 size_t reference=p?oracle_sms_render(expected,8192):oracle_genesis_render(expected,8192);
 CHECK(count==reference&&!memcmp(actual,expected,count*(p?2u:1u)*sizeof(*actual)));
 sega_psg_destroy(s);cases++;
}
static void isolation(int p){
 sega_psg *a=sega_psg_create((sega_psg_profile)p,1234567u),*b=sega_psg_create((sega_psg_profile)p,1234567u);
 sega_psg *aa=sega_psg_create((sega_psg_profile)p,1234567u),*bb=sega_psg_create((sega_psg_profile)p,1234567u);
 CHECK(a&&b&&aa&&bb);unsigned d=p?16u:240u;
 for(unsigned n=0;n<101;n++){
  uint8_t va=(uint8_t)(n*43u),vb=(uint8_t)(n*71u);
  sega_psg_write(a,va);sega_psg_advance(a,d*13u+n);sega_psg_write(b,vb);sega_psg_advance(b,d*17u+n);
  sega_psg_write(aa,va);sega_psg_advance(aa,d*13u+n);sega_psg_write(bb,vb);sega_psg_advance(bb,d*17u+n);
 }
 int16_t x[16384],y[16384];size_t n=sega_psg_render(a,x,8192),m=sega_psg_render(aa,y,8192);
 CHECK(n==m&&!memcmp(x,y,n*sega_psg_channels(a)*sizeof(*x)));
 n=sega_psg_render(b,x,8192);m=sega_psg_render(bb,y,8192);CHECK(n==m&&!memcmp(x,y,n*sega_psg_channels(b)*sizeof(*x)));
 CHECK(sega_psg_sample_rate(a)==1234567u/d);
 /* Bad snapshots do not partially replace state or pending PCM. */
 FILE *bad=tmpfile();CHECK(bad);fputc(0,bad);rewind(bad);CHECK(!sega_psg_load(a,bad));fclose(bad);
 /* Valid-length but invalid register range must also leave the instance intact. */
 bad=tmpfile();CHECK(bad);CHECK(sega_psg_save(a,bad));rewind(bad);
 CHECK(fputc(255,bad)!=EOF&&fputc(255,bad)!=EOF);rewind(bad);
 CHECK(!sega_psg_load(a,bad));fclose(bad);
 sega_psg_advance(a,d*119u);sega_psg_advance(aa,d*119u);
 n=sega_psg_render(a,x,8192);m=sega_psg_render(aa,y,8192);CHECK(n==m&&!memcmp(x,y,n*sega_psg_channels(a)*sizeof(*x)));
 sega_psg_destroy(a);sega_psg_destroy(b);sega_psg_destroy(aa);sega_psg_destroy(bb);
}
int main(void){
 CHECK(!sega_psg_create((sega_psg_profile)99,1));CHECK(!sega_psg_create(SEGA_PSG_GENESIS,0));
 for(int p=0;p<2;p++)for(unsigned n=0;n<8;n++)for(unsigned v=0;v<16;v++)scenario(p,n,v);
 isolation(0);isolation(1);
 printf("%d donor differential PSG scenarios plus independent-instance and transactional-load checks passed\n",cases);return 0;
}
