/* SPDX-License-Identifier: BSD-2-Clause
 * PACGA mask result ABI: the persisted AND-immediate/NOP rewrite must compute
 * exactly what the trap emulator computes for every register alias. The
 * native model below decodes the logical immediate independently (DecodeBitMasks)
 * and shares no code with the converter. */
#include "b2l_cache_core.h"
#include "b2l_compat.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int executable(uint64_t pc,void *unused){(void)unused;return pc==0x1004?0:-1;}
static int mask_pacga(uint64_t a,uint64_t b,uint64_t *out,void *unused){(void)b;(void)unused;*out=a&UINT64_C(0xffffffff00000000);return 0;}
static uint64_t ror64(uint64_t v,unsigned r,unsigned size){
 uint64_t m=size==64?UINT64_MAX:((UINT64_C(1)<<size)-1);v&=m;r%=size;
 return r?(((v>>r)|(v<<(size-r)))&m):v;
}
/* ARM DecodeBitMasks for 64-bit logical immediates. */
static uint64_t logical_immediate(unsigned n,unsigned immr,unsigned imms){
 unsigned len=0;unsigned pattern=(n<<6)|((~imms)&63u);
 assert(pattern);while((pattern>>(len+1))&&len<6)len++;
 assert(len>=1);unsigned size=1u<<len,s=imms&(size-1),r=immr&(size-1);
 assert(s!=size-1);uint64_t welem=s==63u?UINT64_MAX:((UINT64_C(1)<<(s+1))-1);
 uint64_t elem=ror64(welem,r,size),value=0;
 for(unsigned i=0;i<64;i+=size)value|=elem<<i;
 return value;
}
static int native_step(uint32_t word,struct b2l_compat_ctx *c){
 if(word==0xd503201fu){c->pc+=4;return 0;}
 /* AND (immediate), 64-bit: sf=1 opc=00 100100 N immr imms Rn Rd */
 assert((word&0xff800000u)==0x92000000u);
 unsigned n=(word>>22)&1u,immr=(word>>16)&63u,imms=(word>>10)&63u,rn=(word>>5)&31u,rd=word&31u;
 uint64_t imm=logical_immediate(n,immr,imms);
 uint64_t source=rn==31u?0:c->x[rn];
 if(rd==31u)c->sp=source&imm;else c->x[rd]=source&imm;
 c->pc+=4;return 0;
}
int main(void){
 struct b2l_compat_ops ops={.mode=B2L_COMPAT_MODE_STRIPPED,.validate_pc=executable,.pacga=mask_pacga};
 uint64_t converted=0,nops=0;uint32_t replacement;
 assert(b2l_nc_pacga_mask==0);
 for(unsigned rm=0;rm<32;rm++)for(unsigned rn=0;rn<32;rn++)for(unsigned rd=0;rd<32;rd++){
  uint32_t original=0x9ac03000u|(rm<<16)|(rn<<5)|rd;
  b2l_nc_pacga_mask=0;assert(!b2l_nc_native(original,&replacement));
  b2l_nc_pacga_mask=1;assert(b2l_nc_native(original,&replacement));
  if(rd==31u){assert(replacement==0xd503201fu);nops++;}
  else{assert(replacement==(0x92607c00u|(rn<<5)|rd));assert(logical_immediate(1,32,31)==UINT64_C(0xffffffff00000000));converted++;}
  struct b2l_compat_ctx a={0},b;
  for(unsigned i=0;i<31;i++)a.x[i]=UINT64_C(0x1122334455667788)^(UINT64_C(0x0101010101010101)*i)^((uint64_t)rm<<40);
  a.pc=0x1000;a.sp=0x9000;a.cpsr=0xa0000000;b=a;
  assert(b2l_compat_emulate_reference(original,&a,&ops)==B2L_COMPAT_OK);
  assert(!native_step(replacement,&b));assert(!memcmp(&a,&b,sizeof(a)));
  /* The rewritten site never touches SP or flags and discards only XZR. */
  assert(b.sp==0x9000&&b.cpsr==0xa0000000&&b.pc==0x1004);
 }
 assert(converted==31*32*32&&nops==32*32);
 /* A record carrying PACGA rewrites validates only under the mask gate and
  * is counted so the kernel can adopt the ABI the root already requires. */
 unsigned char*r=calloc(1,B2L_NC_COPY);assert(r);
 memcpy(r,"B2LNCREC",8);b2l_nc_p64(r+8,1);b2l_nc_p64(r+16,42);b2l_nc_p64(r+24,8192);r[40]=7;b2l_nc_p32(r+56,1);b2l_nc_p32(r+60,4096);
 b2l_nc_p32(r+B2L_NC_DATA,0x9aca3108u);b2l_nc_p32(r+B2L_NC_DATA+4096,0x92607d08u);        /* pacga x8,x8,x10 */
 b2l_nc_p32(r+B2L_NC_DATA+4,0x9ac83008u);b2l_nc_p32(r+B2L_NC_DATA+4096+4,0x92607c08u);    /* pacga x8,x0,x8 */
 b2l_nc_p32(r+B2L_NC_DATA+8,0xd65f0fffu);b2l_nc_p32(r+B2L_NC_DATA+4096+8,0xd65f03c0u);    /* retab */
 b2l_nc_p32(r+B2L_NC_DATA+12,0x9ac0301fu);b2l_nc_p32(r+B2L_NC_DATA+4096+12,0x9ac0301fu);  /* unchanged pacga xzr */
 b2l_nc_seal(r);
 b2l_nc_pacga_mask=1;assert(!b2l_nc_record(r)&&b2l_nc_record_pacga(r)==2);
 b2l_nc_pacga_mask=0;assert(b2l_nc_record(r)==-1);
 free(r);
 printf("PACGA mask ABI: %llu AND-immediate and %llu NOP rewrites match the reference emulator; gate, SP/flags and record counting verified\n",
  (unsigned long long)converted,(unsigned long long)nops);
 return 0;
}
