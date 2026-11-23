/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../uefi/XnuLoader/BootMetadata.h"
int main(void){
 unsigned char field[32]={0},mac[6];memset(mac,255,6);
 assert(b2l_boot_wifi_decode(field,mac)==0 && !memcmp(mac,(unsigned char[6]){0},6));
 memcpy(field,"B2LWIFI1",8);field[8]=2;field[13]=1;
 assert(b2l_boot_wifi_decode(field,mac)==1 && mac[0]==2 && mac[5]==1);
 for(unsigned i=14;i<32;i++){field[i]=1;assert(b2l_boot_wifi_decode(field,mac)==-1);assert(!memcmp(mac,(unsigned char[6]){0},6));field[i]=0;}
 field[0]^=1;assert(b2l_boot_wifi_decode(field,mac)==-1);field[0]^=1;
 memcpy(field+14,"B2LFW001",8);
 unsigned fw=B2L_WIFI_FIRMWARE_BYTES;
 for(unsigned i=0;i<4;i++)field[22+i]=(unsigned char)(fw>>(8*i));
 assert(b2l_boot_wifi_decode(field,mac)==1&&b2l_boot_wifi_firmware(field)==(int)fw);
 for(unsigned i=14;i<32;i++){
  field[i]^=1;assert(b2l_boot_wifi_decode(field,mac)==-1);field[i]^=1;
 }
 /* S10 port: B2LFW002 adds the NVRAM length and its complement. */
 memcpy(field+14,"B2LFW002",8);field[26]=0x40;field[27]=0x2f;field[28]=0xbf;field[29]=0xd0;
 assert(b2l_boot_wifi_decode(field,mac)==1&&b2l_boot_wifi_firmware(field)==(int)fw+0x2f40);
 for(unsigned i=14;i<32;i++)for(unsigned bit=0;bit<8;bit++){
  field[i]^=(unsigned char)(1u<<bit);assert(b2l_boot_wifi_firmware(field)==-1);field[i]^=(unsigned char)(1u<<bit);
 }
 field[26]=0x42;field[28]=0xbd;assert(b2l_boot_wifi_firmware(field)==-1); /* not a word multiple */
 field[26]=0;field[27]=0;field[28]=0xff;field[29]=0xff;assert(b2l_boot_wifi_firmware(field)==-1); /* empty */
 field[26]=4;field[27]=0x80;field[28]=0xfb;field[29]=0x7f;assert(b2l_boot_wifi_firmware(field)==-1); /* > 32 KiB */
 field[26]=0;field[27]=0x80;field[28]=0xff;field[29]=0x7f;assert(b2l_boot_wifi_firmware(field)==(int)fw+32768);
 /* S10 port: B2LFW003 adds the CLM blob's size in words (here 7366, 29464 bytes). */
 memcpy(field+14,"B2LFW003",8);field[26]=0x40;field[27]=0x2f;field[28]=0xbf;field[29]=0xd0;field[30]=0xc6;field[31]=0x1c;
 assert(b2l_boot_wifi_decode(field,mac)==1&&b2l_boot_wifi_firmware(field)==(int)fw+0x2f40+29464);
 field[30]=0;field[31]=0;assert(b2l_boot_wifi_firmware(field)==-1);              /* no CLM */
 field[30]=1;field[31]=0x40;assert(b2l_boot_wifi_firmware(field)==-1);           /* > 64 KiB */
 field[30]=0;field[31]=0x40;assert(b2l_boot_wifi_firmware(field)==(int)fw+0x2f40+65536);
 field[26]=0x42;field[28]=0xbd;assert(b2l_boot_wifi_firmware(field)==-1);        /* NVRAM not a word multiple */
 field[26]=4;field[27]=0x80;field[28]=0xfb;field[29]=0x7f;assert(b2l_boot_wifi_firmware(field)==-1); /* NVRAM > 32 KiB */
 field[21]='2';field[26]=0x40;field[27]=0x2f;field[28]=0xbf;field[29]=0xd0;field[30]=0xc6;field[31]=0x1c;
 assert(b2l_boot_wifi_firmware(field)==-1);                                     /* B2LFW002 with a CLM size */
 assert(b2l_wifi_attachment_valid(fw)&&b2l_wifi_attachment_valid(fw+4)&&!b2l_wifi_attachment_valid(fw-4)&&
  !b2l_wifi_attachment_valid(fw+2)&&b2l_wifi_attachment_valid(fw+32768ULL+65536ULL)&&
  !b2l_wifi_attachment_valid(fw+32768ULL+65540ULL)&&!b2l_wifi_attachment_valid(0));
 memset(field+14,0,18);
 field[8]=1;assert(b2l_boot_wifi_decode(field,mac)==-1);
 field[8]=0;field[13]=0;assert(b2l_boot_wifi_decode(field,mac)==-1);
 puts("Optional BOOT WiFi metadata: legacy, valid, corrupt and invalid address checks passed");
 return 0;
}
