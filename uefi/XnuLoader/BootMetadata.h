/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef B2L_BOOT_METADATA_H
#define B2L_BOOT_METADATA_H
/* Optional BOOT-v2 reserved field. Device-specific bytes belong in local
 * packaging input, never in this source or a printed manifest. */
#define B2L_WIFI_MAC_BYTES 6U
#define B2L_BOOT_METADATA_BYTES 32U
#define B2L_WIFI_FIRMWARE_BYTES 1297036U
/* S10 port: Wi-Fi mode 11 boots that firmware, which needs the board NVRAM,
 * DHD-processed, right after it in the same attachment (B2LFW002). */
#define B2L_WIFI_NVRAM_MAX_BYTES 32768U
/* S10 port: Wi-Fi mode 19 loads the board's CLM (regulatory) blob the way DHD
 * does ("clmload"); it follows the NVRAM in the same attachment (B2LFW003). */
#define B2L_WIFI_CLM_MAX_BYTES 65536U
/* S10 port: Wi-Fi mode 23 joins the network a fixed 128-byte record after
 * the CLM blob names (B2LFW004): its name and passphrase, which only the
 * kernel reads; /chosen still carries only the attachment's range. */
#define B2L_WIFI_JOIN_BYTES 128U
/* The attachment is the firmware alone or followed by 4-byte-multiple NVRAM,
 * and then perhaps a 4-byte-multiple CLM blob and the join record. */
static inline int b2l_wifi_attachment_valid(unsigned long long size)
{
    if(size<B2L_WIFI_FIRMWARE_BYTES)return 0;
    size-=B2L_WIFI_FIRMWARE_BYTES;
    return size<=B2L_WIFI_NVRAM_MAX_BYTES+B2L_WIFI_CLM_MAX_BYTES+B2L_WIFI_JOIN_BYTES&&!(size&3U);
}
/* Optional private firmware follows the Mach-O exactly. Explicit metadata
 * prevents stale bytes beyond a shorter BOOT image from becoming an input.
 * B2LFW001: field[22..25] firmware size, field[26..31] zero.
 * B2LFW002: the same, then field[26..27] NVRAM size, field[28..29] its
 * complement, field[30..31] zero, so any single flipped bit is rejected.
 * B2LFW003: B2LFW002's fields, then field[30..31] the CLM blob's size in
 * words (nonzero); the kernel checks the blob against its pinned SHA-256.
 * B2LFW004: B2LFW003's fields; the join record (B2L_WIFI_JOIN_BYTES) follows
 * the CLM blob, and the kernel checks its own digest.
 * Returns the attachment's total size, 0 when absent, -1 when malformed. */
static inline int b2l_boot_wifi_firmware(const unsigned char field[32])
{
    static const unsigned char tag[7]={'B','2','L','F','W','0','0'};
    unsigned any=0;
    for(unsigned i=14;i<32;i++)any|=field[i];
    if(!any)return 0;
    for(unsigned i=0;i<7;i++)if(field[14+i]!=tag[i])return -1;
    unsigned size=(unsigned)field[22]|(unsigned)field[23]<<8|
        (unsigned)field[24]<<16|(unsigned)field[25]<<24;
    if(size!=B2L_WIFI_FIRMWARE_BYTES)return -1;
    if(field[21]=='1'){
        for(unsigned i=26;i<32;i++)if(field[i])return -1;
        return (int)size;
    }
    if(field[21]!='2'&&field[21]!='3'&&field[21]!='4')return -1;
    unsigned clm=((unsigned)field[30]|(unsigned)field[31]<<8)*4U;
    if(field[21]=='2'?clm!=0:(clm==0||clm>B2L_WIFI_CLM_MAX_BYTES))return -1;
    unsigned nvram=(unsigned)field[26]|(unsigned)field[27]<<8;
    unsigned check=(unsigned)field[28]|(unsigned)field[29]<<8;
    if(!nvram||(nvram^check)!=0xffffU||nvram>B2L_WIFI_NVRAM_MAX_BYTES||(nvram&3U))return -1;
    return (int)(size+nvram+clm+(field[21]=='4'?B2L_WIFI_JOIN_BYTES:0U));
}
static inline int b2l_wifi_mac_valid(const unsigned char mac[6])
{
    unsigned any=0;
    for(unsigned i=0;i<6;i++)any|=mac[i];
    return any && !(mac[0]&1U);
}
/* 0: legacy absent field; 1: valid WiFi metadata; -1: malformed field.
 * All six output bytes are cleared even on absence or failure. */
static inline int b2l_boot_wifi_decode(const unsigned char field[32],unsigned char mac[6])
{
    static const unsigned char tag[8]={'B','2','L','W','I','F','I','1'};
    unsigned any=0;
    for(unsigned i=0;i<6;i++)mac[i]=0;
    for(unsigned i=0;i<32;i++)any|=field[i];
    if(!any)return 0;
    for(unsigned i=0;i<8;i++)if(field[i]!=tag[i])return -1;
    if(b2l_boot_wifi_firmware(field)<0)return -1;
    if(!b2l_wifi_mac_valid(field+8))return -1;
    for(unsigned i=0;i<6;i++)mac[i]=field[8+i];
    return 1;
}
#endif
