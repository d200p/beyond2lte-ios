/* Synthetic staging fixture only. No vendor firmware is included in tests. */
#include <stddef.h>
typedef void *DTEntry;
typedef struct { uint64_t physBase, topOfKernelData; } boot_args;
#define FIXTURE_FIRMWARE_BYTES 1297036u
/* S10 port: Wi-Fi mode 11's synthetic NVRAM follows the firmware. */
#define FIXTURE_NVRAM_MAX 32768u
/* S10 port: Wi-Fi mode 19's synthetic CLM blob follows the NVRAM (B2LFW003). */
#define FIXTURE_CLM_BYTES 29464u
static _Alignas(4096) uint8_t firmware_fixture[FIXTURE_FIRMWARE_BYTES + FIXTURE_NVRAM_MAX + 65536u];
static unsigned firmware_nvram_bytes, firmware_clm_bytes, firmware_join_bytes;
static boot_args firmware_args;
static struct { void *bootArgs; } PE_state = { &firmware_args };
static unsigned firmware_fault;
/* S10 port, Wi-Fi mode 16: /chosen mac-address-wifi0 (the loader's, from the
 * private attachment): 0 present, 1 absent, 2 the same as the chip's. */
static unsigned firmware_mac_mode;
static const uint8_t firmware_chosen_mac[6] = { 0x02, 0xb2, 0x1c, 0x5e, 0x10, 0x16 };
static const uint8_t firmware_chip_mac[6] = { 0x00, 0x90, 0x4c, 0xc5, 0x12, 0x38 };
#define kSuccess 0
static inline void firmware_fixture_setup(void) {
    firmware_fault=0;firmware_nvram_bytes=0;firmware_clm_bytes=0;firmware_join_bytes=0;
    firmware_args.physBase=(uintptr_t)firmware_fixture;
    firmware_args.topOfKernelData=firmware_args.physBase+FIXTURE_FIRMWARE_BYTES;
    for(unsigned i=0;i<FIXTURE_FIRMWARE_BYTES;i++)firmware_fixture[i]=(uint8_t)(i*13u+7u);
    memset(firmware_fixture+FIXTURE_FIRMWARE_BYTES,0,FIXTURE_NVRAM_MAX+65536u);
}
static inline uint8_t *firmware_clm_at(void) { return firmware_fixture+FIXTURE_FIRMWARE_BYTES+firmware_nvram_bytes; }
static inline void firmware_fixture_clm(void) {
    uint8_t *c=firmware_clm_at();
    for(unsigned i=0;i<FIXTURE_CLM_BYTES;i++)c[i]=(uint8_t)(i*7u+3u);
    memcpy(c,"BLOB",4);firmware_clm_bytes=FIXTURE_CLM_BYTES;
    firmware_args.topOfKernelData+=FIXTURE_CLM_BYTES;
}
/* S10 port, Wi-Fi mode 23: the join record (B2LFW004) after the CLM blob, a
 * model network's; the fake SHA-256 below gives its digest. */
static inline uint8_t *firmware_join_at(void) { return firmware_clm_at()+firmware_clm_bytes; }
static inline void firmware_join_digest(const uint8_t *record,uint8_t digest[32]) {
    for(unsigned i=0;i<32;i++)digest[i]=(uint8_t)(record[i]^record[44+i]^(i*29u+1u));
}
static inline void firmware_fixture_join(const char *ssid,const char *password) {
    uint8_t *j=firmware_join_at(),digest[32];
    memset(j,0,128);memcpy(j,"B2LJOIN1",8);j[8]=(uint8_t)strlen(ssid);j[9]=(uint8_t)strlen(password);
    memcpy(j+12,ssid,j[8]);memcpy(j+44,password,j[9]);
    firmware_join_digest(j,digest);memcpy(j+112,digest,16);
    firmware_join_bytes=128;firmware_args.topOfKernelData+=128;
}
static inline void firmware_fixture_nvram(const uint8_t *nvram,unsigned bytes) {
    assert(bytes<=FIXTURE_NVRAM_MAX);
    memcpy(firmware_fixture+FIXTURE_FIRMWARE_BYTES,nvram,bytes);firmware_nvram_bytes=bytes;
    firmware_args.topOfKernelData=firmware_args.physBase+FIXTURE_FIRMWARE_BYTES+bytes;
}
static int SecureDTLookupEntry(const void *start,const char *name,DTEntry *dt) {
    assert(!start&&!strcmp(name,"/chosen"));*dt=NULL;return firmware_fault==1 ? -1:0;
}
static int SecureDTGetProperty(DTEntry dt,const char *name,const void **prop,unsigned *n) {
    static uint64_t range[2];(void)dt;
    if(!strcmp(name,"mac-address-wifi0")){
        *prop=firmware_mac_mode==2 ? firmware_chip_mac:firmware_chosen_mac;*n=6;return firmware_mac_mode==1 ? -1:0;
    }
    assert(!strcmp(name,"b2l,wifi-firmware"));
    range[0]=(uintptr_t)firmware_fixture;
    range[1]=FIXTURE_FIRMWARE_BYTES+firmware_nvram_bytes+firmware_clm_bytes+firmware_join_bytes;
    if(firmware_fault==4)range[1]--;
    if(firmware_fault==5)range[0]++;
    if(firmware_fault==6)range[0]-=4096;
    if(firmware_fault==7)range[0]=UINT64_MAX&~4095ULL;
    if(firmware_fault==8)firmware_args.topOfKernelData--;
    *prop=range;*n=firmware_fault==3 ? 8:16;return firmware_fault==2 ? -1:0;
}
static uintptr_t ml_static_ptovirt(uint64_t base) {
    assert(base==(uintptr_t)firmware_fixture);return firmware_fault==9 ? 0:(uintptr_t)firmware_fixture;
}
void b2l_sha256(const void *p,size_t n,uint8_t digest[32]) {
    /* The real SHA implementation is independently exercised by cache tests.
     * Here control only authentication outcome, not hardware transactions. */
    static const uint8_t expected[32]={
        0x16,0x76,0xf4,0x6c,0xe5,0x6b,0x96,0xf5,0x8d,0xc7,0x0d,0xe0,0x8b,0xea,0xab,0x4a,
        0xb3,0x36,0x2e,0xe6,0xdd,0x75,0x14,0x65,0xa8,0xd6,0xa0,0x02,0x3c,0x3c,0x54,0xad};
    /* Mode 19: the end of the attachment, where the board's CLM blob would
     * be; its pinned digest only for the synthetic blob (fault 11: not it). */
    static const uint8_t clm[32]={
        0x5e,0x55,0x20,0xcc,0x65,0x59,0x91,0x92,0x3b,0x7c,0x88,0xc8,0x5c,0x2c,0x02,0x7d,
        0x22,0xb0,0x46,0x2d,0xc6,0xa3,0xbf,0xd8,0xfd,0x19,0x1c,0xbe,0xfa,0xe3,0x33,0x00};
    if(firmware_join_bytes&&p==firmware_join_at()){
        assert(n==112);firmware_join_digest(p,digest);if(firmware_fault==12)digest[3]^=1;return;
    }
    if(p!=firmware_fixture){
        assert(n==FIXTURE_CLM_BYTES);memset(digest,0,32);
        if(firmware_clm_bytes&&p==firmware_clm_at()&&firmware_fault!=11)memcpy(digest,clm,32);
        return;
    }
    assert(p==firmware_fixture&&n==FIXTURE_FIRMWARE_BYTES);memcpy(digest,expected,32);
    if(firmware_fault==10)digest[0]^=1;
}
