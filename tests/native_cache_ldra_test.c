/* SPDX-License-Identifier: BSD-2-Clause */
#include "b2l_cache_core.h"
#include "b2l_compat.h"
#include <assert.h>
#include <stdio.h>
static int read_failure;
static int read_data(uint64_t address,unsigned width,uint64_t *out,void *unused){
 (void)unused;
 if(read_failure||width!=8||address<4096||address>0xffff-7)return -1;
 *out=address^UINT64_C(0x123456789abcdef0);return 0;
}
static int executable(uint64_t pc,void *unused){(void)unused;return pc==0x1004?0:-1;}
/* Ordinary ARMv8 unsigned-offset LDR, signed LDUR and pre-index LDR.
 * This independent native-side model shares no offset/register decoder with
 * the production LDRA converter or its reference emulator. */
static int native_step(uint32_t word,struct b2l_compat_ctx *c){
 unsigned base=(word>>5)&31u,dest=word&31u;int64_t displacement;int wb=0;
 if((word&0xffc00000u)==0xf9400000u)displacement=(int64_t)((word>>10)&4095u)*8;
 else{
  assert((word&0xffe00c00u)==0xf8400000u||(word&0xffe00c00u)==0xf8400c00u);
  unsigned bits=(word>>12)&511u;displacement=bits&256u?(int64_t)bits-512:(int64_t)bits;
  wb=(word&0xc00u)==0xc00u;
 }
 assert(base!=31u&&(!wb||base!=dest));
 uint64_t address=c->x[base]+(uint64_t)displacement,value;
 if(read_data(address,8,&value,NULL))return -1;
 if(dest!=31u)c->x[dest]=value;
 if(wb)c->x[base]=address;
 c->pc+=4;return 0;
}
int main(void){
 struct b2l_compat_ops ops={.mode=B2L_COMPAT_MODE_STRIPPED,.read=read_data,.validate_pc=executable};
 uint64_t converted=0;
 for(unsigned key=0;key<2;key++)for(unsigned wb=0;wb<2;wb++)
 for(unsigned imm=0;imm<1024;imm++)for(unsigned rn=0;rn<32;rn++)for(unsigned rd=0;rd<32;rd++){
  uint32_t original=0xf8200400u|(key<<23)|((imm&511u)<<12)|((imm&512u)<<13)|(wb<<11)|(rn<<5)|rd,replacement;
  int off=(imm>=512u?(int)imm-1024:(int)imm)*8;
  int supported=rn!=31u&&(!wb||rn!=rd)&&((!wb&&off>=0)||(off>=-256&&off<=255));
  assert(b2l_nc_native(original,&replacement)==supported);
  if(!supported)continue;
  converted++;
  struct b2l_compat_ctx a={0},b;
  for(unsigned i=0;i<31;i++)a.x[i]=0x8000+i*16;
  a.pc=0x1000;a.sp=0x9000;a.cpsr=0xa0000000;b=a;
  assert(b2l_compat_emulate_reference(original,&a,&ops)==B2L_COMPAT_OK);
  assert(!native_step(replacement,&b));assert(!memcmp(&a,&b,sizeof(a)));
 }
 /* A failed data access never publishes destination, base writeback or PC. */
 for(unsigned wb=0;wb<2;wb++){
  uint32_t word=0xf8200420u|(wb<<11),native;assert(b2l_nc_native(word,&native));
  struct b2l_compat_ctx a={0},b,old;a.x[1]=0x8000;a.pc=0x1000;b=old=a;read_failure=1;
  assert(b2l_compat_emulate_reference(word,&a,&ops)==B2L_COMPAT_FAULT);
  assert(native_step(native,&b)==-1&&!memcmp(&a,&old,sizeof(a))&&!memcmp(&b,&old,sizeof(b)));
 }
 assert(converted>1000000);printf("LDRA native loads: %llu encodings match the reference; SP, overlap, unrepresentable offsets and faults retained\n",(unsigned long long)converted);
}
