/* SPDX-License-Identifier: BSD-2-Clause
 * First Wi-Fi bring-up checkpoint for beyond2lte / Exynos 9820.
 *
 * b2l_wifi_diag=1 samples shared PMU, GPIO and CMU registers once, read only.
 * b2l_wifi_diag=2 additionally pulses WLAN power with PERST held low, tests
 * PHY isolation bypass readback, then restores the owned bits.
 * b2l_wifi_diag=3 additionally reads six controller ELBI status/reset words
 * during that pulse. It writes no controller register, trains no link and
 * accesses no PHY/PCS/DBI/config/BAR, firmware or network registration.
 * b2l_wifi_diag=4 initializes the channel-zero PHY/root port for one bounded
 * Gen1 x1 link attempt, then asserts reset and powers the PHY down. No ATU,
 * endpoint configuration/BAR, DMA mapping, IRQ enabling or firmware loading.
 * b2l_wifi_diag=5 additionally reads four endpoint configuration words through
 * a temporary outbound CFG0 region, then disables it. No endpoint writes.
 * b2l_wifi_diag=6 additionally sets a temporary BAR0/window and memory-only
 * endpoint command for three chipcommon reads. Never enables bus mastering,
 * maps dongle RAM, writes chip registers or loads firmware.
 * b2l_wifi_diag=7 also captures the measured AI EROM page, without following
 * its address descriptors or reading its control registers.
 * b2l_wifi_diag=8 checks measured EROM witnesses, reads fixed R4/PCIe wrapper
 * state, and reads R4 capabilities only when the vendor core-up test passes.
 * b2l_wifi_diag=9 also inventories TCM banks on the measured halted R4,
 * temporarily changing only BANKIDX and restoring its original value.
 * b2l_wifi_diag=10 writes the pinned private firmware to measured TCM and
 * reads every word back while R4 stays halted. No firmware execution.
 * b2l_wifi_diag=11 (S10 port) also boots that firmware the way Linux DHD
 * does: NVRAM, its length token and a random seed at the top of RAM, the
 * reset vector, then the R4 released for about 3 s while only reads follow
 * (the shared area, console text, trap/assert records), then halted again.
 * Bus mastering, DMA rings, mailboxes and IRQs stay off; nothing is sent to
 * the firmware. It runs in a kernel thread 360 s after start, after the
 * Metal test window, so a wedge here cannot cost that boot's other results.
 * b2l_wifi_diag=12 (S10 port) also reads, while the firmware runs, the whole
 * shared area, its ring_info, the five common rings and their indices and
 * the mailbox words, lets it run 2 s more and reads its console again.
 * b2l_wifi_diag=13 (S10 port) also does the host half of DHD's start
 * (dhd_prot_init, dhd_bus_hostready): the five common rings in phone memory
 * below 4 GiB, the scratch word and host_cap in the shared area, bus
 * mastering on the root port and the endpoint, the hostready doorbell, then
 * two ioctl-response and four event buffers on the control submission ring
 * and its doorbell, and 2 s of polling what the firmware did with them. No
 * ioctl is sent and no interrupt is enabled. The memory is never freed, so a
 * late DMA can only land in it; bus mastering is off again once the R4 is
 * halted, before the power-off.
 * b2l_wifi_diag=14 (S10 port) also sends one ioctl, WLC_GET_MAGIC, once the
 * buffers are taken, and polls 2 s for its completions and its answer.
 * b2l_wifi_diag=15 (S10 port) also asks, one at a time, for the "ver" and
 * "cur_etheraddr" iovars (the MAC is compared, never logged).
 * b2l_wifi_diag=16 (S10 port) also gives the chip the attachment's MAC
 * (/chosen mac-address-wifi0) when its own differs, and reads it back.
 * b2l_wifi_diag=17 (S10 port) also brings the radio up (WLC_UP), reads
 * "isup" and logs the first firmware events (types only); no scan or join.
 * b2l_wifi_diag=18 (S10 port) also runs one passive scan (escan, listening
 * only: no probe request is sent) and logs how many networks it heard, on
 * which channels and how strongly; never a name or an address.
 * b2l_wifi_diag=19 (S10 port) first loads the board's CLM (regulatory) blob
 * from the private attachment, as DHD does before the radio comes up.
 * A powered PHY alone does not establish that DBI/ELBI accesses are safe.
 * All addresses/offsets are sourced in docs/wifi-bringup.md.
 */
#include <arm64/b2l_early.h>
#include <arm64/b2l_wifi_phy_data.h>
#include <arm/caches_internal.h>
#include <kern/clock.h>
#include <kern/misc_protos.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/boot.h>
#include <pexpert/arm64/b2l_ram.h>
#include <libsa/string.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>

#define wlog(...) b2l_early_trace("B2L WIFI: " __VA_ARGS__)
#define WIFI_FIRMWARE_BYTES 1297036u
/* S10 port, mode 11: the DHD-processed NVRAM follows the firmware in the same
 * private attachment (uefi/XnuLoader/BootMetadata.h, B2LFW002). */
#define WIFI_NVRAM_MAX_BYTES 32768u
#define WIFI_RAM_BASE 0x170000u
#define WIFI_RAM_END 0x300000u
#define WIFI_R4_WRAPPER 0x18102000u
#define WIFI_DEFER_S 360u
/* S10 port, mode 13: the PCIe2 core (EROM: core 0x83c rev 64, slave port 0),
 * its doorbells (pcie_core.h PCIH2D_MailBox, PCIH2D_DB1; the firmware does
 * not set PCIE_SHARED_DAR, so not the DAR copies) and PCIMailBoxInt (rev 64). */
#define WIFI_PCIE_CORE 0x18003000u
#define WIFI_H2D_DB0 0x140u
#define WIFI_H2D_DB1 0x144u
#define WIFI_MAILBOX_INT 0xc30u
#define WIFI_HOST_BYTES 0x40000u        /* mode 19: 256 KiB (one clmload chunk's input above 128 KiB) */
#define WIFI_HOST_SCRATCH 0x11000u      /* DMA_D2H_SCRATCH_BUF_LEN 8 */
#define WIFI_HOST_IOCTL_IN 0x11800u     /* mode 14: the ioctl's input */
#define WIFI_HOST_IOCTL 0x12000u        /* two 8 KiB ioctl-response buffers */
#define WIFI_HOST_EVENT 0x16000u        /* four 2 KiB event buffers */
#define WIFI_HOST_IOCTL2 0x18000u       /* mode 15: two more ioctl-response buffers */
#define WIFI_HOST_IOVAR_IN 0x1c000u     /* modes 15 and 16: the iovars' inputs, 512 bytes each */
#define WIFI_HOST_IOCTL3 0x1e000u       /* mode 16: two 4 KiB ioctl-response buffers */
#define WIFI_HOST_IOCTL4 0x1d000u       /* mode 17: two 2 KiB ioctl-response buffers */
#define WIFI_HOST_BLOB_IN 0x20000u      /* mode 19: one clmload chunk's input, 2 KiB */
/* Mode 19: the board's CLM blob (bcmdhd_clm.blob) after the NVRAM in the
 * attachment (B2LFW003), pinned like the firmware; dhd_download_blob's chunk. */
#define WIFI_CLM_BYTES 29464u
#define WIFI_CLM_MAX_BYTES 65536u
#define WIFI_CLM_CHUNK 1400u
extern ppnum_t pmap_find_phys(pmap_t, addr64_t);
/* bcmmsgbuf.h sizes for IPC revision 7 (the default, not DHD_HTPUT_TUNABLES). */
static const struct { uint32_t offset; uint16_t items, size; } wifi_rings[5] = {
    { 0x00000, 64, 40 },        /* H2D control submission */
    { 0x01000, 512, 32 },       /* H2D rx post */
    { 0x05000, 64, 24 },        /* D2H control completion */
    { 0x06000, 1024, 24 },      /* D2H tx completion */
    { 0x0c000, 512, 40 },       /* D2H rx completion */
};
static uint8_t *wifi_host;      /* KMA_PERMANENT: never freed, never reused */
static uint64_t wifi_host_pa;
/* 4 while bus mastering is on: the guards then expect it on both sides. */
static uint32_t wifi_master;
static const uint8_t *wifi_firmware;
static const uint8_t *wifi_nvram;
static unsigned wifi_nvram_bytes;
static const uint8_t *wifi_clm;
static unsigned wifi_clm_bytes;
/* S10 port, mode 19: the control rings wrap (clmload is 22 ioctls). Every
 * completion is counted from the start (wifi_d2h_count, at the last look;
 * wifi_d2h_ring, the ring index then; wifi_d2h_parsed, how many were read),
 * an item's slot is its count modulo the ring. The buffers the firmware
 * holds are known (bit n: request id n), so none is posted twice. While
 * wifi_quick is set an ioctl is waited for only until its completion. */
static uint32_t wifi_d2h_count, wifi_d2h_ring, wifi_d2h_parsed, wifi_events_held, wifi_ioctls_held;
static int wifi_quick;
extern void b2l_sha256(const void *, size_t, uint8_t[32]);
extern void read_frandom(void *, unsigned int);

/* The layout DHD's process_nvram_vars and download_nvram produce: printable
 * name=value entries, each ended by one NUL, then NUL padding whose last four
 * bytes are always zero. scripts/package-boot.py applies the same rule. */
static int
wifi_nvram_check(const uint8_t *p, unsigned n)
{
    unsigned entries = 0, start = 0, equals = 0, ended = 0;
    char type[16] = "?", rev[16] = "?";
    if (n < 8 || n > WIFI_NVRAM_MAX_BYTES || (n & 3) || p[n - 4] || p[n - 3] || p[n - 2] || p[n - 1])
        goto bad;
    for (unsigned i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (ended) {
            if (c) goto bad;
            continue;
        }
        if (!c) {
            if (i == start) { ended = 1; continue; }
            if (equals <= start + 1) goto bad;
            entries++; start = i + 1; equals = 0;
            continue;
        }
        if (c < 0x20 || c > 0x7e) goto bad;
        if (c == '=' && !equals) equals = i + 1;
    }
    if (!entries) goto bad;
    for (unsigned i = 0; i < n && p[i]; ) {
        unsigned length = 0;
        while (p[i + length]) length++;
        const char *entry = (const char *)p + i;
        char *out = !strncmp(entry, "boardtype=", 10) ? type : !strncmp(entry, "boardrev=", 9) ? rev : NULL;
        if (out) {
            unsigned skip = out == type ? 10 : 9, copy = length - skip;
            if (copy > 15) copy = 15;
            memcpy(out, entry + skip, copy);
            out[copy] = 0;
        }
        i += length + 1;
    }
    wlog("private NVRAM input verified: bytes=%u entries=%u boardtype=%s boardrev=%s\n", n, entries, type, rev);
    return 0;
bad:
    wlog("private NVRAM input rejected: bytes=%u; not the DHD-processed layout\n", n);
    return -1;
}

static int
wifi_firmware_input(uint32_t mode)
{
    static const uint8_t expected[32] = {
        0x16,0x76,0xf4,0x6c,0xe5,0x6b,0x96,0xf5,0x8d,0xc7,0x0d,0xe0,0x8b,0xea,0xab,0x4a,
        0xb3,0x36,0x2e,0xe6,0xdd,0x75,0x14,0x65,0xa8,0xd6,0xa0,0x02,0x3c,0x3c,0x54,0xad
    };
    DTEntry dt;
    const void *prop;
    unsigned n;
    uint64_t range[2];
    uint8_t digest[32];
    const boot_args *args = (const boot_args *)PE_state.bootArgs;
    static const uint8_t expected_clm[32] = {
        0x5e,0x55,0x20,0xcc,0x65,0x59,0x91,0x92,0x3b,0x7c,0x88,0xc8,0x5c,0x2c,0x02,0x7d,
        0x22,0xb0,0x46,0x2d,0xc6,0xa3,0xbf,0xd8,0xfd,0x19,0x1c,0xbe,0xfa,0xe3,0x33,0x00
    };
    wifi_firmware = NULL;
    wifi_nvram = NULL;
    wifi_nvram_bytes = 0;
    wifi_clm = NULL;
    wifi_clm_bytes = 0;
    if (!args || SecureDTLookupEntry(NULL, "/chosen", &dt) != kSuccess ||
        SecureDTGetProperty(dt, "b2l,wifi-firmware", &prop, &n) != kSuccess || n != sizeof(range)) return -1;
    memcpy(range, prop, sizeof(range));
    /* The firmware alone (B2LFW001), followed by the NVRAM (B2LFW002), and
     * then perhaps the board's CLM blob (B2LFW003), which is known by its size
     * and its pinned SHA-256 at the end. */
    if (range[1] < WIFI_FIRMWARE_BYTES ||
        range[1] - WIFI_FIRMWARE_BYTES > WIFI_NVRAM_MAX_BYTES + WIFI_CLM_MAX_BYTES ||
        ((range[1] - WIFI_FIRMWARE_BYTES) & 3) || (range[0] & 4095) ||
        range[0] < args->physBase || range[0] > args->topOfKernelData ||
        range[1] > args->topOfKernelData - range[0]) return -1;
    const uint8_t *data = (const uint8_t *)ml_static_ptovirt(range[0]);
    if (!data) return -1;
    b2l_sha256(data, WIFI_FIRMWARE_BYTES, digest);
    if (memcmp(digest, expected, sizeof(expected))) return -1;
    unsigned nvram = (unsigned)(range[1] - WIFI_FIRMWARE_BYTES), clm = 0;
    if (nvram >= WIFI_CLM_BYTES + 8) {
        b2l_sha256(data + range[1] - WIFI_CLM_BYTES, WIFI_CLM_BYTES, digest);
        if (!memcmp(digest, expected_clm, sizeof(expected_clm))) clm = WIFI_CLM_BYTES;
        nvram -= clm;
    }
    if (nvram > WIFI_NVRAM_MAX_BYTES) return -1;
    if (mode >= 11 && wifi_nvram_check(data + WIFI_FIRMWARE_BYTES, nvram)) return -1;
    wifi_firmware = data;
    wlog("private firmware input verified: bytes=%u SHA-256 pinned; no filesystem access\n", WIFI_FIRMWARE_BYTES);
    if (mode >= 11) {
        wifi_nvram = data + WIFI_FIRMWARE_BYTES;
        wifi_nvram_bytes = nvram;
    }
    if (clm) {
        wifi_clm = data + WIFI_FIRMWARE_BYTES + nvram;
        wifi_clm_bytes = clm;
        wlog("private CLM blob verified: bytes=%u SHA-256 pinned\n", clm);
    }
    return 0;
}

struct wifi_region {
    vm_offset_t physical, mapped;
    vm_size_t size;
};

static uint32_t
wifi_read(const struct wifi_region *region, unsigned offset)
{
    return *(volatile const uint32_t *)(region->mapped + offset);
}

static void
wifi_write(const struct wifi_region *region, unsigned offset, uint32_t value)
{
    *(volatile uint32_t *)(region->mapped + offset) = value;
    __asm__ volatile ("dsb sy" ::: "memory");
}

/* Change and restore only owned bits, preserving every other pin and PMU bit.
 * An all-ones register is not used as the input to a read-modify-write. */
static int
wifi_bits(const struct wifi_region *region, unsigned offset, uint32_t mask, uint32_t value)
{
    uint32_t before = wifi_read(region, offset);
    if (before == UINT32_MAX) {
        wlog("bits invalid read pa=%#lx off=%#x mask=%#x\n",
            (unsigned long)region->physical, offset, mask);
        return -1;
    }
    uint32_t after = (before & ~mask) | (value & mask);
    if (after != before) wifi_write(region, offset, after);
    uint32_t checked = wifi_read(region, offset);
    if (checked != UINT32_MAX && (checked & mask) == (value & mask)) return 0;
    wlog("bits mismatch pa=%#lx off=%#x mask=%#x before=%#x got=%#x want=%#x\n",
        (unsigned long)region->physical, offset, mask, before, checked, value & mask);
    return -1;
}

static int
wifi_restore_bits(const struct wifi_region *region, unsigned offset, uint32_t mask, uint32_t value)
{
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        if (!wifi_bits(region, offset, mask, value)) return 0;
        delay_for_interval(1, NSEC_PER_MSEC);
    }
    return -1;
}

/* Samsung writes this channel-zero profile without checking its readback.
 * Epoch 53 measured 0x1d02 -> write 0x1d0c -> read 0x1d04 on this board.
 * Preserve that exact vendor write; accept only the exact observed exception,
 * not a general waiver for bit 3 or a claim that the bit is read-only. */
static int
wifi_phy_sysreg_profile(const struct wifi_region *sysreg)
{
    uint32_t before = wifi_read(sysreg, 0x1050);
    if (before == UINT32_MAX) {
        wlog("PHY SYSREG profile invalid initial read\n");
        return -1;
    }
    uint32_t next = (before & ~0xfeu) | 0x0cu;
    if (next != before) wifi_write(sysreg, 0x1050, next);
    uint32_t checked = wifi_read(sysreg, 0x1050);
    if (checked != UINT32_MAX && (checked & 0xfeu) == 0x0c) return 0;
    if (before == 0x1d02 && next == 0x1d0c && checked == 0x1d04) {
        wlog("PHY SYSREG profile: observed board readback before=%#x wrote=%#x got=%#x\n",
            before, next, checked);
        return 0;
    }
    wlog("PHY SYSREG profile mismatch before=%#x wrote=%#x got=%#x mask=0xfe want=0xc\n",
        before, next, checked);
    return -1;
}

/* PCI configuration control is in the low half-word. Write zero to status
 * bits in the high half, and to PME_Status (W1C) in the PMCSR low half. */
static int
wifi_control16(const struct wifi_region *dbi, unsigned off, uint32_t mask, uint32_t value)
{
    uint32_t before = wifi_read(dbi, off);
    if (before == UINT32_MAX) return -1;
    uint32_t next = ((before & 0xffffu) & ~mask) | (value & mask);
    if (off == 0x44) next &= ~0x8000u;
    wifi_write(dbi, off, next);
    uint32_t checked = wifi_read(dbi, off);
    if (checked != UINT32_MAX && (checked & mask) == (value & mask)) return 0;
    wlog("PCI control mismatch off=%#x mask=%#x before=%#x got=%#x want=%#x\n",
        off, mask, before, checked, value & mask);
    return -1;
}

static int
wifi_reset_pulse(const struct wifi_region *elbi, unsigned off, unsigned final_us)
{
    wifi_write(elbi, off, 1); delay(10);
    wifi_write(elbi, off, 0); delay(10);
    wifi_write(elbi, off, 1); delay(final_us);
    uint32_t value = wifi_read(elbi, off);
    if (value == 1) return 0;
    wlog("reset pulse mismatch off=%#x got=%#x want=1\n", off, value);
    return -1;
}

static int
wifi_atu_enable(const struct wifi_region *dbi, unsigned index)
{
    uint32_t control = 0;
    wifi_write(dbi, 0x908, 0x80000000);
    for (unsigned attempt = 0; attempt < 5; attempt++) {
        control = wifi_read(dbi, 0x908);
        if (control == 0x80000000) return 0;
        if (control == UINT32_MAX) break;
        delay_for_interval(9, NSEC_PER_MSEC);
    }
    wlog("ATU%u enable failed: readback=%#x\n", index, control);
    return -1;
}

static int
wifi_cfg_ready(struct wifi_region *regions, uint64_t deadline)
{
    const struct wifi_region *elbi = &regions[4], *dbi = &regions[8];
    uint32_t link = wifi_read(elbi, 0x74), status = wifi_read(dbi, 0x80);
    uint32_t command = wifi_read(dbi, 0x04), viewport = wifi_read(dbi, 0x900);
    if (mach_absolute_time() >= deadline || link == UINT32_MAX ||
        (link & 31u) < 0x0d || (link & 31u) > 0x14 || status == UINT32_MAX ||
        ((status >> 16) & 15u) != 1 || ((status >> 20) & 63u) != 1 ||
        command == UINT32_MAX || (command & 0x147u) != (2u | wifi_master) || viewport != 1) return 0;
    return wifi_read(dbi, 0x908) == 0x80000000;
}

static int
wifi_cfg_write_checked(struct wifi_region *regions, unsigned offset, uint32_t value,
    uint64_t deadline)
{
    if (!wifi_cfg_ready(regions, deadline)) return -1;
    const struct wifi_region *cfg = &regions[9];
    wifi_write(cfg, 0x2000 + offset, value);
    if (!wifi_cfg_ready(regions, deadline)) return -1;
    uint32_t checked = wifi_read(cfg, 0x2000 + offset);
    if (checked == value) return 0;
    wlog("BAR0 config mismatch off=%#x got=%#x want=%#x\n", offset, checked, value);
    return -1;
}

static int
wifi_ep_memory_command(struct wifi_region *regions, uint32_t value, uint64_t deadline)
{
    if (!wifi_cfg_ready(regions, deadline)) return -1;
    const struct wifi_region *cfg = &regions[9];
    uint32_t before = wifi_read(cfg, 0x2004);
    if (before == UINT32_MAX || (value != 0 && value != 2)) return -1;
    value |= wifi_master;   /* S10 port, mode 13: bus mastering stays as it was set */
    wifi_write(cfg, 0x2004, ((before & 0xffffu) & ~7u) | value);
    if (!wifi_cfg_ready(regions, deadline)) return -1;
    uint32_t checked = wifi_read(cfg, 0x2004);
    if (checked != UINT32_MAX && (checked & 7u) == value) return 0;
    wlog("BAR0 memory command mismatch got=%#x want=%#x; %s\n", checked, value,
        wifi_master ? "bus mastering was on (mode 13)" : "BME never requested");
    return -1;
}

static int
wifi_cfg_restore(struct wifi_region *regions, unsigned offset, uint32_t value)
{
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        if (!wifi_cfg_write_checked(regions, offset, value, UINT64_MAX)) return 0;
        delay_for_interval(1, NSEC_PER_MSEC);
    }
    return -1;
}

static int
wifi_bar_ready(struct wifi_region *regions, uint32_t window, uint64_t deadline)
{
    const struct wifi_region *dbi = &regions[8], *cfg = &regions[9];
    if (wifi_bits(dbi, 0x900, UINT32_MAX, 0)) return 0;
    uint32_t control = wifi_read(dbi, 0x908);
    if (wifi_bits(dbi, 0x900, UINT32_MAX, 1) || control != 0x80000000 ||
        !wifi_cfg_ready(regions, deadline) || (wifi_read(cfg, 0x2004) & 7u) != (2u | wifi_master) ||
        wifi_read(cfg, 0x2010) != 0x14400004 || wifi_read(cfg, 0x2014) != 0 ||
        wifi_read(cfg, 0x2080) != window) return 0;
    return 1;
}

static int
wifi_erom_capture(struct wifi_region *regions, unsigned *captured_words, unsigned *target_signature)
{
    /* aidmp.h puts ER_REMAPCONTROL at 0xe00. Never read it or later registers.
     * An exact ER_END|ER_VALID (0xf) terminates only at a tagged-entry boundary;
     * 64-bit address/size extensions are untagged data and may also equal 0xf. */
    unsigned state = 0, size_follows = 0;
    /* Host-validated epoch58 witnesses for just the two fixed probe targets.
     * This is an additional device guard, not a general EROM decoder. */
    static const struct { unsigned off; uint32_t value; } witnesses[] = {
        { 0x44, 0x4bf83e01 }, { 0x48, 0x0f084411 }, { 0x50, 0x18002005 },
        { 0x68, 0x181020c5 }, { 0x70, 0x4bf83c01 }, { 0x74, 0x40084411 },
        { 0x7c, 0x18003005 }, { 0xa0, 0x181030c5 },
    };
    *captured_words = *target_signature = 0;
    uint64_t deadline;
    clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
    wlog("EROM capture begin: measured page=0x18111000, max=896 words, 100 ms; descriptors not followed\n");
    for (unsigned i = 0; i < 896; i++) {
        if (!wifi_bar_ready(regions, 0x18111000, deadline)) {
            wlog("EROM capture stopped: guard/deadline before word=%u state=%u\n", i, state);
            return -1;
        }
        uint32_t word = wifi_read(&regions[10], i * 4);
        *captured_words = i + 1;
        wlog("EROM +%#x raw=%#x\n", i * 4, word);
        if (word == UINT32_MAX) {
            wlog("EROM capture stopped: invalid all-ones word=%u\n", i);
            return -1;
        }
        for (unsigned n = 0; n < sizeof(witnesses) / sizeof(witnesses[0]); n++) {
            if (i * 4 == witnesses[n].off && word == witnesses[n].value)
                *target_signature |= 1u << n;
        }
        if (state == 0) {
            if (word == 0x0f) {
                wlog("EROM capture complete: words=%u; raw table only, no core access\n", i + 1);
                return 0;
            }
            if ((word & 1u) && (word & 6u) == 4) {
                size_follows = (word & 0x30u) == 0x30u;
                if (word & 8u) state = 1; /* 64-bit address high word. */
                else if (size_follows) state = 2;
            }
        } else if (state == 1) state = size_follows ? 2 : 0;
        else if (state == 2) state = (word & 8u) ? 3 : 0; /* Optional high size. */
        else state = 0;
    }
    wlog("EROM capture stopped: 896-word bound reached without terminator\n");
    return -1;
}

static int
wifi_backplane_window(struct wifi_region *regions, uint32_t window, uint64_t deadline)
{
    return wifi_ep_memory_command(regions, 0, deadline) ||
        wifi_cfg_write_checked(regions, 0x80, window, deadline) ||
        wifi_ep_memory_command(regions, 2, deadline) ? -1 : 0;
}

static int
wifi_core_snapshot(struct wifi_region *regions, uint32_t *r4_cap, unsigned *r4_halted)
{
    const struct wifi_region *bar = &regions[10];
    static const uint32_t wrappers[] = { 0x18103000, 0x18102000 };
    static const unsigned offsets[] = { 0x408, 0x500, 0x800 };
    uint32_t values[2][3] = { { 0 } };
    *r4_cap = *r4_halted = 0;
    uint64_t deadline;
    clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
    wlog("core snapshot begin: measured PCIe2/R4 wrappers; no core clock/reset writes\n");
    for (unsigned core = 0; core < 2; core++) {
        if (wifi_backplane_window(regions, wrappers[core], deadline)) return -1;
        for (unsigned i = 0; i < 3; i++) {
            if (!wifi_bar_ready(regions, wrappers[core], deadline)) return -1;
            values[core][i] = wifi_read(bar, offsets[i]);
            wlog("core wrapper=%#x +%#x raw=%#x\n", wrappers[core], offsets[i], values[core][i]);
            if (values[core][i] == UINT32_MAX) return -1;
        }
    }
    /* Broadcom ai_iscoreup: CLOCK_EN set, FGC clear, RESET clear. The R4
     * CPUHALT flag is reported only, never modified in this checkpoint. */
    unsigned r4_up = (values[1][0] & 3u) == 1 && !(values[1][2] & 1u);
    wlog("R4 state: up=%u CPUHALT=%u; no clock/reset changes\n", r4_up, (values[1][0] >> 5) & 1u);
    if (r4_up) {
        if (wifi_backplane_window(regions, 0x18002000, deadline) ||
            !wifi_bar_ready(regions, 0x18002000, deadline)) return -1;
        uint32_t cap = wifi_read(bar, 4); /* sbhndarm.h: corecapabilities. */
        wlog("R4 core capabilities=%#x; bank indices and RAM untouched\n", cap);
        if (cap == UINT32_MAX) return -1;
        *r4_cap = cap;
        *r4_halted = !!(values[1][0] & 0x20u);
    } else wlog("R4 capabilities skipped: core-up prerequisites absent\n");
    wlog("core snapshot complete: wrappers=2 R4_capabilities=%s; no chip writes or firmware\n",
        r4_up ? "read" : "skipped");
    return 0;
}

static int
wifi_tcm_index(struct wifi_region *regions, uint32_t index, uint64_t deadline)
{
    if (!wifi_bar_ready(regions, 0x18002000, deadline)) return -1;
    wifi_write(&regions[10], 0x40, index);
    if (!wifi_bar_ready(regions, 0x18002000, deadline)) return -1;
    return wifi_read(&regions[10], 0x40) == index ? 0 : -1;
}

static int
wifi_tcm_inventory(struct wifi_region *regions, int *restore_failed, unsigned *measured_profile)
{
    const struct wifi_region *bar = &regions[10];
    uint32_t original = 0, bytes = 0;
    unsigned read_banks = 0;
    int changed = 0, restored = 1, result = -1;
    unsigned matches = 1;
    *measured_profile = 0;
    uint64_t deadline;
    /* Only the measured capability word is admitted by the caller: 5 TCMA
     * and 8 TCMB banks. si_tcm_size selects BANKIDX then reads BANKINFO.
     * We never reset/clock the core, write BANKPDA, or access TCM contents. */
    clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
    if (!wifi_bar_ready(regions, 0x18002000, deadline)) goto out;
    original = wifi_read(bar, 0x40);
    if (original >= 13) {
        wlog("TCM inventory skipped: original BANKIDX=%#x outside measured 13 banks\n", original);
        goto out;
    }
    wlog("TCM inventory begin: TCMA=5 TCMB=8 original_BANKIDX=%u; selector writes only\n", original);
    for (unsigned bank = 0; bank < 13; bank++) {
        /* Mark owned before attempting a posted write, including lost writes. */
        changed = 1;
        if (wifi_tcm_index(regions, bank, deadline) ||
            !wifi_bar_ready(regions, 0x18002000, deadline)) goto out;
        uint32_t info = wifi_read(bar, 0x44);
        if (info == UINT32_MAX) goto out;
        if (info != (bank == 4 ? 0x88c07u : 0x108c0fu)) matches = 0;
        uint32_t size = ((info & 0x7fu) + 1u) * ((info & 0x200u) ? 1024u : 8192u);
        bytes += size;
        read_banks++;
        wlog("TCM bank=%u info=%#x bytes=%u; no RAM access\n", bank, info, size);
    }
    result = 0;
out:
    if (changed) {
        restored = 0;
        /* A separate bounded cleanup budget still checks live link/BAR state.
         * Loss of access forbids further chip writes; parent powers down. */
        clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
        for (unsigned attempt = 0; attempt < 3; attempt++) {
            if (!wifi_tcm_index(regions, original, deadline)) {
                restored = 1; break;
            }
            delay_for_interval(1, NSEC_PER_MSEC);
        }
    }
    if (!restored) *restore_failed = -1;
    if (!result && restored && matches && bytes == 0x190000) *measured_profile = 1;
    wlog("TCM inventory end: stage=%s banks=%u bytes=%u selector_restore=%s; no RAM/firmware\n",
        result ? "incomplete" : "complete", read_banks, bytes,
        changed ? (restored ? "verified" : "FAILED") : "unchanged");
    return result || !restored ? -1 : 0;
}

static int
wifi_firmware_halted(struct wifi_region *regions, uint64_t deadline)
{
    if (wifi_backplane_window(regions, 0x18102000, deadline) ||
        !wifi_bar_ready(regions, 0x18102000, deadline)) return 0;
    uint32_t io = wifi_read(&regions[10], 0x408);
    if (!wifi_bar_ready(regions, 0x18102000, deadline)) return 0;
    uint32_t reset = wifi_read(&regions[10], 0x800);
    return io != UINT32_MAX && reset != UINT32_MAX && (io & 0x23u) == 0x21u && !(reset & 1u);
}

static uint32_t
wifi_firmware_word(unsigned at)
{
    return (uint32_t)wifi_firmware[at] | (uint32_t)wifi_firmware[at+1] << 8 |
        (uint32_t)wifi_firmware[at+2] << 16 | (uint32_t)wifi_firmware[at+3] << 24;
}

static int
wifi_firmware_download(struct wifi_region *regions)
{
    uint64_t deadline;
    unsigned verified = 0;
    /* DHD uses base0x170000 and TCM0x190000 on this exact measured profile.
     * Only this pinned image is written. No reset vector at zero, NVRAM,
     * RAM-size token, DMA, IRQ or CPUHALT release is programmed. */
    clock_interval_to_deadline(5000, NSEC_PER_MSEC, &deadline);
    wlog("firmware download begin: base=0x170000 bytes=%u; 5 s bound, R4 halted, DMA off\n", WIFI_FIRMWARE_BYTES);
    for (unsigned page = 0; page < WIFI_FIRMWARE_BYTES; page += 4096) {
        uint32_t window = 0x170000 + page;
        unsigned length = WIFI_FIRMWARE_BYTES - page;
        if (length > 4096) length = 4096;
        if (!wifi_firmware_halted(regions, deadline) ||
            wifi_backplane_window(regions, window, deadline)) goto stopped;
        for (unsigned chunk = 0; chunk < length; chunk += 64) {
            unsigned end = chunk + 64;
            if (end > length) end = length;
            if (!wifi_bar_ready(regions, window, deadline)) goto stopped;
            for (unsigned off = chunk; off < end; off += 4)
                wifi_write(&regions[10], off, wifi_firmware_word(page + off));
            if (!wifi_bar_ready(regions, window, deadline)) goto stopped;
            for (unsigned off = chunk; off < end; off += 4) {
                if (wifi_read(&regions[10], off) != wifi_firmware_word(page + off)) goto stopped;
                verified += 4;
            }
        }
    }
    if (!wifi_firmware_halted(regions, deadline)) goto stopped;
    wlog("firmware download complete: bytes=%u readback=verified R4=halted; execution/NVRAM/DMA/IRQs disabled\n", verified);
    return 0;
stopped:
    wlog("firmware download stopped: verified_bytes=%u; no further RAM writes or firmware execution\n", verified);
    return -1;
}

/* S10 port, mode 11. One backplane word through BAR0's 4 KiB window, moved
 * only when needed; every access is bracketed by the same link/BAR guard. */
static int
wifi_bp_window(struct wifi_region *regions, uint32_t address, uint64_t deadline)
{
    uint32_t window = address & ~4095u;
    if (wifi_bar_ready(regions, window, deadline)) return 0;
    return wifi_backplane_window(regions, window, deadline) || !wifi_bar_ready(regions, window, deadline) ? -1 : 0;
}

static int
wifi_bp_read(struct wifi_region *regions, uint32_t address, uint32_t *value, uint64_t deadline)
{
    if (wifi_bp_window(regions, address, deadline)) return -1;
    *value = wifi_read(&regions[10], address & 4095u);
    return wifi_bar_ready(regions, address & ~4095u, deadline) ? 0 : -1;
}

static int
wifi_bp_write(struct wifi_region *regions, uint32_t address, uint32_t value, uint64_t deadline)
{
    if (wifi_bp_window(regions, address, deadline)) return -1;
    wifi_write(&regions[10], address & 4095u, value);
    return wifi_bar_ready(regions, address & ~4095u, deadline) ? 0 : -1;
}

static uint32_t
wifi_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Word-aligned bytes into dongle RAM, each word read back. */
static int
wifi_bp_block(struct wifi_region *regions, uint32_t address, const uint8_t *data, unsigned bytes, uint64_t deadline)
{
    for (unsigned at = 0; at < bytes; at += 4) {
        uint32_t word = wifi_le32(data + at), got = 0;
        if (wifi_bp_write(regions, address + at, word, deadline) ||
            wifi_bp_read(regions, address + at, &got, deadline) || got != word) {
            wlog("RAM write stopped at %#x: got=%#x want=%#x\n", address + at, got, word);
            return -1;
        }
    }
    return 0;
}

/* aiutils.c _ai_core_reset on the R4's only wrapper (one master wrapper, so
 * no second one): each step waits up to 300 us for resetstatus to clear. */
static int
wifi_r4_idle(struct wifi_region *regions, uint64_t deadline)
{
    uint32_t status = 0;
    for (unsigned i = 0; i < 30; i++) {
        if (wifi_bp_read(regions, WIFI_R4_WRAPPER + 0x804, &status, deadline)) return -1;
        if (!status) return 0;
        delay(10);
    }
    wlog("R4 wrapper resetstatus stays %#x\n", status);
    return -1;
}

static int
wifi_r4_reset(struct wifi_region *regions, uint32_t bits, uint64_t deadline)
{
    uint32_t value = 0;
    if (wifi_r4_idle(regions, deadline) ||
        wifi_bp_write(regions, WIFI_R4_WRAPPER + 0x800, 1, deadline)) return -1;
    delay(10);
    if (wifi_r4_idle(regions, deadline) ||
        wifi_bp_write(regions, WIFI_R4_WRAPPER + 0x408, bits | 3u, deadline) ||
        wifi_bp_read(regions, WIFI_R4_WRAPPER + 0x408, &value, deadline) ||
        wifi_r4_idle(regions, deadline)) return -1;
    for (unsigned attempt = 0;; attempt++) {
        if (wifi_bp_read(regions, WIFI_R4_WRAPPER + 0x800, &value, deadline)) return -1;
        if (!value) break;
        if (attempt == 9 || wifi_r4_idle(regions, deadline) ||
            wifi_bp_write(regions, WIFI_R4_WRAPPER + 0x800, 0, deadline) ||
            wifi_r4_idle(regions, deadline)) return -1;
    }
    if (wifi_bp_write(regions, WIFI_R4_WRAPPER + 0x408, bits | 1u, deadline) ||
        wifi_bp_read(regions, WIFI_R4_WRAPPER + 0x408, &value, deadline)) return -1;
    delay(10);
    if ((value & 0x23u) == (bits | 1u)) return 0;
    wlog("R4 wrapper ioctrl=%#x after reset, want %#x\n", value, bits | 1u);
    return -1;
}

/* A NUL-terminated firmware string (assert file/expression), at most 95 bytes. */
static void
wifi_log_string(struct wifi_region *regions, const char *what, uint32_t address, uint64_t deadline)
{
    char text[96];
    unsigned length = 0;
    if (address < WIFI_RAM_BASE || address >= WIFI_RAM_END - 96) return;
    for (uint32_t at = address & ~3u; length < sizeof(text) - 1; at += 4) {
        uint32_t word;
        if (wifi_bp_read(regions, at, &word, deadline)) break;
        for (unsigned b = 0; b < 4 && length < sizeof(text) - 1; b++) {
            if (at + b < address) continue;
            char c = (char)(word >> (8 * b));
            if (!c) goto done;
            text[length++] = c >= 0x20 && c <= 0x7e ? c : '.';
        }
    }
done:
    text[length] = 0;
    wlog("firmware %s: %s\n", what, text);
}

static int
wifi_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* S10 port: an address the firmware prints (bcm_ether_ntoa, xx:xx:xx:xx:xx:xx)
 * never reaches the log; from mode 17 on the console can name networks. */
static void
wifi_mask_addresses(char *s, unsigned n)
{
    for (unsigned k = 0; k + 17 <= n; k++) {
        int match = 1;
        for (unsigned p = 0; match && p < 17; p++) match = p % 3 == 2 ? s[k + p] == ':' : wifi_hex_digit(s[k + p]);
        if (!match) continue;
        for (unsigned p = 0; p < 17; p++)
            if (p % 3 != 2) s[k + p] = 'x';
        k += 16;
    }
}

/* The text before the console's write index, one log line per firmware line;
 * from *last on (mode 12's second read), at most 4 KiB. *last becomes the index. */
static void
wifi_log_console(struct wifi_region *regions, uint32_t console, uint64_t deadline, uint32_t *last)
{
    static char text[4096 + 4];
    uint32_t buffer = 0, size = 0, index = 0;
    if (console < WIFI_RAM_BASE || console > WIFI_RAM_END - 32 ||
        wifi_bp_read(regions, console + 8, &buffer, deadline) ||
        wifi_bp_read(regions, console + 12, &size, deadline) ||
        wifi_bp_read(regions, console + 16, &index, deadline)) {
        wlog("console structure at %#x unreadable\n", console);
        return;
    }
    wlog("console at %#x: buffer=%#x size=%u index=%u\n", console, buffer, size, index);
    if (buffer < WIFI_RAM_BASE || size == 0 || size > 0x10000 || buffer > WIFI_RAM_END - size || index > size) {
        wlog("console buffer outside RAM or index beyond it; text not read\n");
        return;
    }
    unsigned count = index > 4096 ? 4096 : index, from = index - count, length = 0;
    if (*last <= index && *last > from) from = *last;
    if (*last == index && index != 0) {
        wlog("console: no new text since index %u\n", index);
        return;
    }
    *last = index;
    for (uint32_t at = (buffer + from) & ~3u; at < buffer + index; at += 4) {
        uint32_t word;
        if (wifi_bp_read(regions, at, &word, deadline)) {
            wlog("console read stopped at %#x\n", at);
            break;
        }
        for (unsigned b = 0; b < 4; b++) {
            if (at + b < buffer + from || at + b >= buffer + index) continue;
            text[length++] = (char)(word >> (8 * b));
        }
    }
    unsigned lines = 0, start = 0;
    for (unsigned i = 0; i <= length && lines < 120; i++) {
        if (i < length && text[i] != '\n' && i - start < 160) continue;
        char saved = text[i];
        text[i] = 0;
        for (unsigned j = start; j < i; j++) {
            if (text[j] == '\r') text[j] = ' ';
            else if (text[j] < 0x20 || text[j] > 0x7e) text[j] = '.';
        }
        wifi_mask_addresses(text + start, i - start);
        if (i > start) {
            wlog("console: %s\n", text + start);
            lines++;
        }
        text[i] = saved;
        start = i < length && text[i] == '\n' ? i + 1 : i;
    }
    wlog("console end: bytes=%u lines=%u\n", length, lines);
}

static int
wifi_in_ram(uint32_t address, uint32_t bytes)
{
    return !(address & 3u) && address >= WIFI_RAM_BASE && address <= WIFI_RAM_END - bytes;
}

/*
 * S10 port, mode 12, read only: what the running firmware set up for the
 * host (bcmpcie.h): the whole pciedev_shared_t (32 words, IPC revision 7),
 * its ring_info_t (19 words), the five common rings' ring_mem_t and the
 * read/write indices of those rings in dongle memory, and the two mailbox
 * data words. This is what Linux DHD reads in dhdpcie_readshared before it
 * sets up any ring; nothing is written.
 */
static void
wifi_log_rings(struct wifi_region *regions, uint32_t shared, uint64_t deadline)
{
    static const struct { uint32_t bit; const char *name; } names[] = {
        { 0x100, "assert_built" }, { 0x200, "assert" }, { 0x400, "trap" }, { 0x8000, "evt_seqnum" },
        { 0x10000, "dma_index" }, { 0x20000, "d2h_sync_seqnum" }, { 0x40000, "d2h_sync_xorcsum" },
        { 0x80000, "idle_flow_ring" }, { 0x100000, "2byte_indices" }, { 0x200000, "fatal_logbuf" },
        { 0x400000, "idma" }, { 0x800000, "msi_multi" }, { 0x1000000, "ifrm" }, { 0x2000000, "use_mailbox" },
        { 0x4000000, "mfgbuild" }, { 0x8000000, "timestamp_db0" }, { 0x10000000, "hostrdy" },
        { 0x20000000, "no_oob_dw" }, { 0x40000000, "inband_ds" }, { 0x80000000u, "dar" },
    };
    uint32_t w[32], info[19], mem[5][4], value;
    char text[320];
    unsigned length = 0;
    if (!wifi_in_ram(shared, sizeof(w))) {
        wlog("rings: shared area at %#x too close to the end of RAM\n", shared);
        return;
    }
    for (unsigned i = 0; i < 32; i++) {
        if (wifi_bp_read(regions, shared + 4 * i, &w[i], deadline)) {
            wlog("rings: shared word %u unreadable\n", i);
            return;
        }
    }
    text[0] = 0;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]) && length < sizeof(text) - 24; i++) {
        if (w[0] & names[i].bit) {
            int n = snprintf(text + length, sizeof(text) - length, " %s", names[i].name);
            if (n > 0) length += (unsigned)n;
        }
    }
    wlog("shared flags %#x (IPC revision %u):%s\n", w[0], w[0] & 0xffu, length ? text : " none");
    wlog("shared: lfrag_pkts=%u max_host_rxbufs=%u dma_rxoffset=%#x h2d_mb=%#x d2h_mb=%#x rings_info=%#x "
        "scratch_len=%u buzz/trap_len=%#x flags2=%#x host_cap=%#x fatal_logbuf=%#x flags3=%#x etd=%#x "
        "txpost_ext_tags=%#x\n", w[8] & 0xffffu, w[8] >> 16, w[9], w[10], w[11], w[12], w[13], w[19], w[20],
        w[21], w[24], w[27], w[30], w[31]);
    for (unsigned i = 0; i < 2; i++) {
        uint32_t box = w[10 + i];
        if (wifi_in_ram(box, 4) && !wifi_bp_read(regions, box, &value, deadline))
            wlog("%s mailbox data at %#x: %#x\n", i ? "d2h" : "h2d", box, value);
    }
    if (!wifi_in_ram(w[12], sizeof(info))) {
        wlog("rings: rings_info %#x outside RAM\n", w[12]);
        return;
    }
    for (unsigned i = 0; i < 19; i++) {
        if (wifi_bp_read(regions, w[12] + 4 * i, &info[i], deadline)) {
            wlog("rings: ring_info word %u unreadable\n", i);
            return;
        }
    }
    wlog("rings info at %#x: ringmem=%#x h2d_w=%#x h2d_r=%#x d2h_w=%#x d2h_r=%#x max_tx_flowrings=%u "
        "submission=%u completion=%u vdevs=%u hwa_db=%#x/%#x\n", w[12], info[0], info[1], info[2], info[3],
        info[4], info[13] & 0xffffu, info[13] >> 16, info[14] & 0xffffu, info[14] >> 16, info[17], info[18]);
    wlog("rings host addresses (unset by us): h2d_w %#x:%#x h2d_r %#x:%#x d2h_w %#x:%#x d2h_r %#x:%#x ifrm %#x:%#x\n",
        info[6], info[5], info[8], info[7], info[10], info[9], info[12], info[11], info[16], info[15]);
    if (!wifi_in_ram(info[0], sizeof(mem))) {
        wlog("rings: ringmem %#x outside RAM\n", info[0]);
        return;
    }
    for (unsigned r = 0; r < 5; r++) {
        for (unsigned i = 0; i < 4; i++) {
            if (wifi_bp_read(regions, info[0] + 16 * r + 4 * i, &mem[r][i], deadline)) {
                wlog("rings: ring_mem %u unreadable\n", r);
                return;
            }
        }
        wlog("ring %u: id %u type %u max_item %u item %u bytes base %#x:%#x\n", r, mem[r][0] & 0xffffu,
            (mem[r][0] >> 16) & 0xffu, mem[r][1] & 0xffffu, mem[r][1] >> 16, mem[r][3], mem[r][2]);
    }
    /* 4-byte indices unless the firmware says 2 (PCIE_SHARED_2BYTE_INDICES). */
    const unsigned size = (w[0] & 0x100000u) ? 2 : 4;
    static const char *const arrays[] = { "h2d write", "h2d read", "d2h write", "d2h read" };
    for (unsigned a = 0; a < 4; a++) {
        unsigned count = a < 2 ? 2 : 3;
        uint32_t base = info[1 + a], idx[3] = { 0 };
        if (!wifi_in_ram(base & ~3u, 12)) {
            wlog("rings: %s indices at %#x outside RAM\n", arrays[a], base);
            continue;
        }
        for (unsigned i = 0; i < count; i++) {
            uint32_t at = base + i * size, word = 0;
            if (wifi_bp_read(regions, at & ~3u, &word, deadline)) break;
            idx[i] = size == 4 ? word : (word >> (8 * (at & 2u))) & 0xffffu;
        }
        wlog("rings: %s indices at %#x: %u %u %u\n", arrays[a], base, idx[0], idx[1], count == 3 ? idx[2] : 0);
    }
}

/* ---- S10 port, mode 13: the host side of the message rings ---- */

/* One dongle RAM word, written and read back. */
static int
wifi_tcm_set(struct wifi_region *regions, uint32_t address, uint32_t value, uint64_t deadline)
{
    uint32_t got = 0;
    if (!wifi_in_ram(address, 4) || wifi_bp_write(regions, address, value, deadline) ||
        wifi_bp_read(regions, address, &got, deadline) || got != value) {
        wlog("mode 13: RAM word %#x got=%#x want=%#x\n", address, got, value);
        return -1;
    }
    return 0;
}

/* The rings, scratch word and buffers: one physically contiguous extent of
 * owned RAM below 4 GiB, zeroed, never freed (as b2l_ufs_transport.c). */
static int
wifi_host_memory(void)
{
    vm_offset_t va = 0;
    if (wifi_host) return 0;
    if (kmem_alloc_contig(kernel_map, &va, WIFI_HOST_BYTES, PAGE_MASK, (ppnum_t)(((1ull << 32) >> PAGE_SHIFT) - 1),
            0, KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS || !va) {
        wlog("mode 13: no contiguous host memory below 4 GiB; host side not started\n");
        return -1;
    }
    uint64_t pa = ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)va));
    int contiguous = pa != 0 && !(pa & 4095u) && pa + WIFI_HOST_BYTES <= (1ull << 32);
    for (uint32_t off = PAGE_SIZE; contiguous && off < WIFI_HOST_BYTES; off += PAGE_SIZE)
        contiguous = ptoa_64(pmap_find_phys(kernel_pmap, (addr64_t)(va + off))) == pa + off;
    if (!contiguous || !b2l_ram_contains_range(b2l_owned_ram, b2l_owned_ram_count, pa, WIFI_HOST_BYTES)) {
        wlog("mode 13: host memory at pa %#llx is not contiguous owned RAM below 4 GiB; kept, not used\n",
            (unsigned long long)pa);
        return -1;
    }
    wifi_host = (uint8_t *)va;
    wifi_host_pa = pa;
    wlog("mode 13: host memory %u bytes at pa %#llx: contiguous owned RAM, zeroed, never freed\n",
        WIFI_HOST_BYTES, (unsigned long long)pa);
    return 0;
}

/* Bus mastering on (bits 4: the root port forwards upstream, then the
 * endpoint masters) or off (0: the endpoint stops, then the root port),
 * memory decoding kept on both. Off is tried whatever the guards say. */
static int
wifi_master_set(struct wifi_region *regions, uint32_t bits, uint64_t deadline)
{
    const struct wifi_region *dbi = &regions[8], *cfg = &regions[9];
    uint32_t rc = wifi_read(dbi, 0x04), ep;
    if (rc == UINT32_MAX) return -1;
    rc &= 0xffffu;      /* the status half is write-1-to-clear: written as zero */
    if (bits) {
        if (wifi_master || !wifi_cfg_ready(regions, deadline)) return -1;
        ep = wifi_read(cfg, 0x2004);
        if (ep == UINT32_MAX || (ep & 7u) != 2u) return -1;
        wifi_write(dbi, 0x04, rc | 4u);
        wifi_master = 4;
        if (!wifi_cfg_ready(regions, deadline)) return -1;
        wifi_write(cfg, 0x2004, ((ep & 0xffffu) & ~7u) | 6u);
    } else {
        wifi_master = rc & 4u;          /* what the root port has now */
        if (!wifi_cfg_ready(regions, deadline)) {
            wifi_master = 0;
            return -1;
        }
        ep = wifi_read(cfg, 0x2004);
        if (ep != UINT32_MAX) wifi_write(cfg, 0x2004, ((ep & 0xffffu) & ~7u) | 2u);
        wifi_write(dbi, 0x04, rc & ~4u);
        wifi_master = 0;
    }
    if (!wifi_cfg_ready(regions, deadline) || (wifi_read(cfg, 0x2004) & 7u) != (2u | bits)) return -1;
    return 0;
}

static void
wifi_put(uint8_t *p, uint32_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++) p[i] = (uint8_t)(value >> (8 * i));
}

/* What the firmware did with the host side. */
struct wifi_host_view {
    uint32_t h2d_read, d2h_write[3], mailbox_int, d2h_data;
    uint32_t d2h_count;         /* control completions written so far, through ring wraps */
};

static int
wifi_host_view(struct wifi_region *regions, const uint32_t *info, uint32_t d2h_mb, struct wifi_host_view *v,
    uint64_t deadline)
{
    memset(v, 0, sizeof(*v));
    if (wifi_bp_read(regions, info[2], &v->h2d_read, deadline)) return -1;
    for (unsigned i = 0; i < 3; i++)
        if (wifi_bp_read(regions, info[3] + 4 * i, &v->d2h_write[i], deadline)) return -1;
    if (wifi_bp_read(regions, WIFI_PCIE_CORE + WIFI_MAILBOX_INT, &v->mailbox_int, deadline)) return -1;
    if (wifi_in_ram(d2h_mb, 4) && wifi_bp_read(regions, d2h_mb, &v->d2h_data, deadline)) return -1;
    /* Between two looks the firmware writes less than a ring: it stops at
     * the read index, which is given back as completions are read. */
    if (v->d2h_write[0] < wifi_rings[2].items) {
        wifi_d2h_count += (v->d2h_write[0] + wifi_rings[2].items - wifi_d2h_ring) % wifi_rings[2].items;
        wifi_d2h_ring = v->d2h_write[0];
    }
    v->d2h_count = wifi_d2h_count;
    return 0;
}

static void
wifi_log_view(const char *when, unsigned posted, const struct wifi_host_view *v)
{
    wlog("mode 13: %s: control submissions read %u of %u; completion writes %u %u %u; "
        "mailbox int %#x; d2h mailbox data %#x\n", when, v->h2d_read, posted % wifi_rings[0].items, v->d2h_write[0], v->d2h_write[1],
        v->d2h_write[2], v->mailbox_int, v->d2h_data);
}

/* Every 200 ms for 2 s, the state when it changed and at the end. */
static int
wifi_host_poll(struct wifi_region *regions, const uint32_t *info, uint32_t d2h_mb, unsigned posted, const char *after,
    struct wifi_host_view *last)
{
    struct wifi_host_view now;
    uint64_t deadline;
    for (unsigned i = 1; i <= 10; i++) {
        delay_for_interval(200, NSEC_PER_MSEC);
        clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
        if (wifi_host_view(regions, info, d2h_mb, &now, deadline)) {
            wlog("mode 13: poll %u lost the link/BAR guard\n", i);
            return -1;
        }
        if (memcmp(&now, last, sizeof(now)) || i == 10) {
            char when[48];
            snprintf(when, sizeof(when), "%u ms after the %s", 200 * i, after);
            wifi_log_view(when, posted, &now);
        }
        *last = now;
    }
    return 0;
}

/* Where the buffer with this request id is, 0 for none. */
static uint32_t
wifi_host_buffer(uint32_t request)
{
    if (request >= 1 && request <= 2) return WIFI_HOST_IOCTL + 0x2000u * (request - 1);
    if (request >= 3 && request <= 6) return WIFI_HOST_EVENT + 0x800u * (request - 3);
    if (request >= 7 && request <= 8) return WIFI_HOST_IOCTL2 + 0x2000u * (request - 7);
    if (request >= 9 && request <= 10) return WIFI_HOST_IOCTL3 + 0x1000u * (request - 9);
    if (request >= 11 && request <= 12) return WIFI_HOST_IOCTL4 + 0x800u * (request - 11);
    return 0;
}

/* The completions the firmware wrote, with dhd_prot_d2h_sync_xorcsum's
 * check (all words XOR to zero), and anything in the scratch and buffers. */
static void
wifi_log_host_writes(const struct wifi_host_view *v, int dump_buffers)
{
    FlushPoC_DcacheRegion((vm_offset_t)wifi_host, WIFI_HOST_BYTES);
    for (unsigned r = 2; r < 5; r++) {
        unsigned n = v->d2h_write[r - 2] < wifi_rings[r].items ? v->d2h_write[r - 2] : 0, first = 0;
        if (r == 2) {       /* the last 16 control completions, counted through ring wraps */
            n = v->d2h_count;
            first = n > 16 ? n - 16 : 0;
        }
        for (unsigned i = first; i < n && i < first + (r == 2 ? 16u : 2u); i++) {
            const uint8_t *c = wifi_host + wifi_rings[r].offset + wifi_rings[r].size * (i % wifi_rings[r].items);
            uint32_t x = 0;
            for (unsigned w = 0; w < wifi_rings[r].size; w += 4) x ^= wifi_le32(c + w);
            wlog("mode 13: ring %u item %u: type %#x if %u flags %#x epoch %u request %#x status %d ring %u; "
                "%#x %#x %#x %#x; xor %#x\n", r, i, c[0], c[1], c[2], c[3], wifi_le32(c + 4),
                (int)(int16_t)(c[8] | c[9] << 8), (unsigned)(c[10] | c[11] << 8), wifi_le32(c + 12),
                wifi_le32(c + 16), wifi_le32(c + 20), wifi_rings[r].size > 24 ? wifi_le32(c + 24) : 0, x);
        }
    }
    const uint8_t *s = wifi_host + WIFI_HOST_SCRATCH;
    wlog("mode 13: scratch %#x %#x\n", wifi_le32(s), wifi_le32(s + 4));
    /* Modes 13 and 14 only, the first six buffers: from mode 15 on, an
     * answer can hold the MAC address and an event packet starts with
     * addresses; those are reported as verdicts or event types instead. */
    for (unsigned i = 0; dump_buffers && i < 6; i++) {
        const uint8_t *b = wifi_host + wifi_host_buffer(1 + i);
        uint32_t w0 = wifi_le32(b), w1 = wifi_le32(b + 4), w2 = wifi_le32(b + 8), w3 = wifi_le32(b + 12);
        if (w0 | w1 | w2 | w3)
            wlog("mode 13: %s buffer %u: %#x %#x %#x %#x\n", i < 2 ? "ioctl-response" : "event", i, w0, w1, w2, w3);
    }
}

/*
 * S10 port, mode 14: one ioctl the way dhd_fillup_ioct_reqst sends it, once
 * the firmware has taken the six buffers: MSG_TYPE_IOCTLPTR_REQ for
 * WLC_GET_MAGIC (0), 4 bytes in and out, request id DHD_IOCTL_REQ_PKTID, its
 * input in phone memory; write index 7 and DB0. The firmware should answer
 * on the control completion ring (MSG_TYPE_IOCTLPTR_REQ_ACK, then
 * MSG_TYPE_IOCTL_CMPLT) and put 0x14e46c77 (WLC_IOCTL_MAGIC) in the first
 * ioctl-response buffer: the chip writing phone memory.
 */
static int
wifi_host_ioctl(struct wifi_region *regions, const uint32_t *info, uint32_t d2h_mb, struct wifi_host_view *last)
{
    uint64_t deadline, in = wifi_host_pa + WIFI_HOST_IOCTL_IN;
    if (last->h2d_read != 6) {
        wlog("mode 14: the firmware took %u of the 6 buffers; no ioctl sent\n", last->h2d_read);
        return 0;
    }
    uint8_t *item = wifi_host + wifi_rings[0].offset + wifi_rings[0].size * 6;
    memset(wifi_host + WIFI_HOST_IOCTL_IN, 0, 64);
    item[0] = 0x09;                             /* MSG_TYPE_IOCTLPTR_REQ */
    item[3] = (uint8_t)((254 + 6) % 253);       /* the seventh H2D item */
    wifi_put(item + 4, 0xfffe, 4);              /* DHD_IOCTL_REQ_PKTID */
    wifi_put(item + 8, 0, 4);                   /* WLC_GET_MAGIC */
    wifi_put(item + 12, 1, 2);                  /* trans_id */
    wifi_put(item + 14, 4, 2);                  /* input_buf_len */
    wifi_put(item + 16, 4, 2);                  /* output_buf_len */
    wifi_put(item + 24, (uint32_t)in, 4);
    wifi_put(item + 28, (uint32_t)(in >> 32), 4);
    CleanPoC_DcacheRegion((vm_offset_t)item, wifi_rings[0].size);
    CleanPoC_DcacheRegion((vm_offset_t)wifi_host + WIFI_HOST_IOCTL_IN, 64);
    clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
    if (wifi_tcm_set(regions, info[1], 7, deadline) ||
        wifi_bp_write(regions, WIFI_PCIE_CORE + WIFI_H2D_DB0, 0xff000007u, deadline)) {
        wlog("mode 14: ioctl write index or DB0 failed\n");
        return -1;
    }
    wlog("mode 14: ioctl WLC_GET_MAGIC sent (4 bytes in and out, input at pa %#llx), write index 7, DB0 %#x\n",
        (unsigned long long)in, 0xff000007u);
    return wifi_host_poll(regions, info, d2h_mb, 7, "ioctl", last);
}

/* The NVRAM's macaddr=xx:xx:xx:xx:xx:xx, 0 when there is none. */
static int
wifi_nvram_mac(uint8_t mac[6])
{
    for (unsigned i = 0; wifi_nvram && i < wifi_nvram_bytes && wifi_nvram[i]; ) {
        const char *e = (const char *)wifi_nvram + i;
        unsigned length = 0;
        while (i + length < wifi_nvram_bytes && e[length]) length++;
        if (length == 25 && !strncmp(e, "macaddr=", 8)) {
            for (unsigned b = 0; b < 6; b++) {
                unsigned v = 0;
                for (unsigned k = 0; k < 2; k++) {
                    char c = e[8 + 3 * b + k];
                    unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ?
                        (unsigned)(c - 'a' + 10) : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16;
                    if (d == 16) return 0;
                    v = v << 4 | d;
                }
                mac[b] = (uint8_t)v;
            }
            return 1;
        }
        i += length + 1;
    }
    return 0;
}

/* S10 port, mode 19: every 20 ms for up to 2 s, until a MSG_TYPE_IOCTL_CMPLT
 * comes after completion `done`; one line only if none came. */
static int
wifi_host_wait(struct wifi_region *regions, const uint32_t *info, uint32_t d2h_mb, uint32_t done,
    struct wifi_host_view *last)
{
    uint64_t deadline;
    for (unsigned i = 0; i < 100; i++) {
        delay_for_interval(20, NSEC_PER_MSEC);
        clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
        if (wifi_host_view(regions, info, d2h_mb, last, deadline)) {
            wlog("mode 19: the link/BAR guard was lost while waiting\n");
            return -1;
        }
        if (last->d2h_count == done) continue;
        FlushPoC_DcacheRegion((vm_offset_t)wifi_host + wifi_rings[2].offset,
            (unsigned long)wifi_rings[2].items * wifi_rings[2].size);
        for (uint32_t k = last->d2h_count - done >= wifi_rings[2].items ? last->d2h_count - 1 : done;
             k < last->d2h_count; k++)
            if (wifi_host[wifi_rings[2].offset + wifi_rings[2].size * (k % wifi_rings[2].items)] == 0x0c) return 0;
    }
    wifi_log_view("2000 ms of waiting", last->h2d_read, last);
    return 0;
}

/*
 * S10 port, modes 15 and 16: one WLC_GET_VAR (262) or WLC_SET_VAR (263) at
 * control submission item `item`, the way DHD sends iovars: the name (and,
 * to set, the value after it) as the input, `length` bytes in and out,
 * request id DHD_IOCTL_REQ_PKTID, transaction id item - 7, then the write
 * index and DB0, and 2 s of polling. 0: answered (*answer, *bytes, *status
 * from the newest MSG_TYPE_IOCTL_CMPLT and its buffer), 1: no answer, -1: a
 * failed access.
 */
static int
wifi_host_var(struct wifi_region *regions, const uint32_t *info, uint32_t d2h_mb, struct wifi_host_view *last,
    const char *tag, unsigned item, unsigned cmd, const char *name, const uint8_t *value, unsigned value_bytes,
    unsigned length, uint32_t in_off, const uint8_t **answer, unsigned *bytes, int *status)
{
    const uint64_t in = wifi_host_pa + in_off;
    const int iovar = cmd == 262 || cmd == 263;    /* WLC_GET_VAR, WLC_SET_VAR: the name first */
    const unsigned done = last->d2h_count, name_bytes = iovar ? (unsigned)strlen(name) + 1 : 0;
    const unsigned in_size = length > 0x200u ? (length + 63u) & ~63u : 0x200u;
    const unsigned windex = (item + 1) % wifi_rings[0].items;
    uint64_t deadline;
    memset(wifi_host + in_off, 0, in_size);
    if (iovar) memcpy(wifi_host + in_off, name, name_bytes);
    if (value) memcpy(wifi_host + in_off + name_bytes, value, value_bytes);
    uint8_t *e = wifi_host + wifi_rings[0].offset + wifi_rings[0].size * (item % wifi_rings[0].items);
    memset(e, 0, wifi_rings[0].size);
    e[0] = 0x09;                                /* MSG_TYPE_IOCTLPTR_REQ */
    e[3] = (uint8_t)((254 + item) % 253);
    wifi_put(e + 4, 0xfffe, 4);                 /* DHD_IOCTL_REQ_PKTID */
    wifi_put(e + 8, cmd, 4);
    wifi_put(e + 12, item - 7, 2);              /* trans_id */
    wifi_put(e + 14, length, 2);
    wifi_put(e + 16, length, 2);
    wifi_put(e + 24, (uint32_t)in, 4);
    wifi_put(e + 28, (uint32_t)(in >> 32), 4);
    CleanPoC_DcacheRegion((vm_offset_t)wifi_host + wifi_rings[0].offset,
        (unsigned long)wifi_rings[0].items * wifi_rings[0].size);
    CleanPoC_DcacheRegion((vm_offset_t)wifi_host + in_off, in_size);
    clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
    if (wifi_tcm_set(regions, info[1], windex, deadline) ||
        wifi_bp_write(regions, WIFI_PCIE_CORE + WIFI_H2D_DB0, 0xff000000u | windex, deadline)) {
        wlog("%s: %s: write index or DB0 failed\n", tag, name);
        return -1;
    }
    if (!wifi_quick && iovar)       /* mode 19's chunks have lines of their own */
        wlog("%s: WLC_%s_VAR %s sent (%u bytes in and out), write index %u, DB0 %#x\n", tag,
            cmd == 263 ? "SET" : "GET", name, length, windex, 0xff000000u | windex);
    else if (!wifi_quick)
        wlog("%s: ioctl %u (%s) sent (%u bytes in and out), write index %u, DB0 %#x\n", tag, cmd, name, length,
            windex, 0xff000000u | windex);
    if (wifi_quick ? wifi_host_wait(regions, info, d2h_mb, done, last) :
        wifi_host_poll(regions, info, d2h_mb, item + 1, name, last)) return -1;
    FlushPoC_DcacheRegion((vm_offset_t)wifi_host, WIFI_HOST_BYTES);
    const unsigned upto = last->d2h_count;
    int found = 0;
    if (upto - wifi_d2h_parsed >= wifi_rings[2].items) wifi_d2h_parsed = upto - (wifi_rings[2].items - 1);
    for (unsigned i = wifi_d2h_parsed; i < upto; i++) {
        const uint8_t *c = wifi_host + wifi_rings[2].offset + wifi_rings[2].size * (i % wifi_rings[2].items);
        const uint32_t id = wifi_le32(c + 4);
        if (c[0] == 0x0e && id >= 3 && id <= 6) wifi_events_held &= ~(1u << id);  /* an event used its buffer */
        if (c[0] == 0x0c && id >= 1 && id <= 12) wifi_ioctls_held &= ~(1u << id);
        if (c[0] != 0x0c || i < done) continue;
        *status = (int16_t)(c[8] | c[9] << 8);
        *bytes = c[12] | c[13] << 8;
        uint32_t at = wifi_host_buffer(wifi_le32(c + 4));
        if (!at || *bytes > 0x1000u) {
            wlog("%s: %s: completion %u status %d, %u bytes in buffer id %#x (not ours)\n", tag, name, i, *status,
                *bytes, wifi_le32(c + 4));
            continue;
        }
        *answer = wifi_host + at;
        found = 1;
    }
    wifi_d2h_parsed = upto;
    if (!found) wlog("%s: %s: no completion in 2 s\n", tag, name);
    return found ? 0 : 1;
}

/* Ioctl-response buffers with request ids `id` and `id` + 1, from item
 * `item` on, after giving the completion read index back
 * (dhd_prot_upd_read_idx); a buffer the firmware still holds is not posted
 * again. -1 on a failed access, else how many items were written. */
static int
wifi_host_repost(struct wifi_region *regions, const uint32_t *info, const struct wifi_host_view *last,
    unsigned item, unsigned id)
{
    uint64_t deadline;
    int written = 0;
    clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
    if (wifi_tcm_set(regions, info[4], last->d2h_write[0], deadline)) return -1;
    for (unsigned i = 0; i < 2; i++) {
        if (wifi_ioctls_held & (1u << (id + i))) continue;
        const unsigned at = item + (unsigned)written;
        uint8_t *e = wifi_host + wifi_rings[0].offset + wifi_rings[0].size * (at % wifi_rings[0].items);
        uint64_t pa = wifi_host_pa + wifi_host_buffer(id + i);
        memset(e, 0, wifi_rings[0].size);
        e[0] = 0x0b;                            /* MSG_TYPE_IOCTLRESP_BUF_POST */
        e[3] = (uint8_t)((254 + at) % 253);
        wifi_put(e + 4, id + i, 4);
        wifi_put(e + 8, id < 9 ? 0x2000u : id < 11 ? 0x1000u : 0x800u, 2);
        wifi_put(e + 16, (uint32_t)pa, 4);
        wifi_put(e + 20, (uint32_t)(pa >> 32), 4);
        wifi_ioctls_held |= 1u << (id + i);
        written++;
    }
    return written;
}

/* The MAC the loader puts in /chosen from the private attachment. */
static int
wifi_chosen_mac(uint8_t mac[6])
{
    DTEntry dt;
    const void *prop = NULL;
    unsigned int n = 0;
    if (SecureDTLookupEntry(NULL, "/chosen", &dt) != kSuccess ||
        SecureDTGetProperty(dt, "mac-address-wifi0", &prop, &n) != kSuccess || n != 6 || !prop) return 0;
    memcpy(mac, prop, 6);
    return 1;
}

/*
 * S10 port, mode 16: when the chip's address is not the attachment's, set
 * it and read it back (items *next on). 0: go on, 1: unanswered (stop, as
 * DHD would with an ioctl outstanding), -1: a failed access.
 */
static int
wifi_host_mac(struct wifi_region *regions, const uint32_t *info, uint32_t d2h_mb, struct wifi_host_view *last,
    const uint8_t chip[6], unsigned *next)
{
    const uint8_t *answer = NULL;
    unsigned bytes = 0;
    int status = 0, r;
    uint8_t chosen[6];
    if (!wifi_chosen_mac(chosen)) {
        wlog("mode 16: no mac-address-wifi0 in /chosen; nothing set\n");
        return 0;
    }
    if (!memcmp(chip, chosen, 6)) {
        wlog("mode 16: the chip's address already is the attachment's; nothing set\n");
        return 0;
    }
    r = wifi_host_repost(regions, info, last, *next, 9);
    if (r < 0) return -1;
    *next += (unsigned)r;
    wlog("mode 16: the chip's address is not the attachment's; completion read index %u given back, two more "
        "ioctl-response buffers posted\n", last->d2h_write[0]);
    r = wifi_host_var(regions, info, d2h_mb, last, "mode 16", (*next)++, 263, "cur_etheraddr", chosen, 6, 20,
        WIFI_HOST_IOVAR_IN + 0x400u, &answer, &bytes, &status);
    if (r) return r;
    wlog("mode 16: cur_etheraddr set: status %d\n", status);
    r = wifi_host_var(regions, info, d2h_mb, last, "mode 16", (*next)++, 262, "cur_etheraddr", NULL, 0, 32,
        WIFI_HOST_IOVAR_IN + 0x600u, &answer, &bytes, &status);
    if (r) return r;
    wlog("mode 16: cur_etheraddr read back (status %d, %u bytes): %s\n", status, bytes,
        bytes < 6 ? "too short" : memcmp(answer, chosen, 6) ? "still NOT the attachment's" : "now the attachment's");
    return 0;
}

/*
 * S10 port, mode 19: the board's CLM (regulatory) blob, as DHD's
 * dhd_apply_default_clm and dhd_download_blob load it before the radio comes
 * up. Boot 81: without it the firmware has "Data: Generic.Min" and the
 * country "#n", and refuses to scan (BCME_EPERM). WLC_SET_VAR "clmload" in
 * chunks of 1400 bytes, each a wl_dload_data_t: flag (DLOAD_HANDLER_VER << 12,
 * DL_BEGIN on the first, DL_END on the last), DL_TYPE_CLM, the chunk's
 * length, crc 0, the bytes, padded to 8; ioctl-response buffers 11 and 12
 * posted for every two. Then "clmload_status", "clmver" and "country"; when
 * the country is still "#n" it is set to "XZ" rev 11, the universal code
 * Samsung's DHD uses when it knows no other (dhd_custom_sec.c) -- a setting:
 * nothing is transmitted. 0: go on, 1: unanswered, -1: a failed access.
 */
static int
wifi_host_clm(struct wifi_region *regions, const uint32_t *info, uint32_t d2h_mb, struct wifi_host_view *last,
    unsigned *next)
{
    static uint8_t chunk[16 + WIFI_CLM_CHUNK];
    const uint8_t *answer = NULL;
    unsigned bytes = 0, chunks = 0;
    int status = 0, r;
    if (!wifi_clm) {
        wlog("mode 19: no CLM blob in the attachment; none loaded\n");
        return 0;
    }
    wifi_quick = 1;
    for (unsigned off = 0; off < wifi_clm_bytes; off += WIFI_CLM_CHUNK, chunks++) {
        const unsigned n = wifi_clm_bytes - off < WIFI_CLM_CHUNK ? wifi_clm_bytes - off : WIFI_CLM_CHUNK;
        const unsigned size = (12u + n + 7u) & ~7u;
        const unsigned flag = 0x1000u | (off == 0 ? 2u : 0u) | (off + n == wifi_clm_bytes ? 4u : 0u);
        if (chunks % 2 == 0) {
            r = wifi_host_repost(regions, info, last, *next, 11);
            if (r < 0) goto failed;
            *next += (unsigned)r;
        }
        memset(chunk, 0, sizeof(chunk));
        wifi_put(chunk, flag, 2);
        wifi_put(chunk + 2, 2, 2);              /* DL_TYPE_CLM */
        wifi_put(chunk + 4, n, 4);
        memcpy(chunk + 12, wifi_clm + off, n);
        r = wifi_host_var(regions, info, d2h_mb, last, "mode 19", (*next)++, 263, "clmload", chunk, size, 8 + size,
            WIFI_HOST_BLOB_IN, &answer, &bytes, &status);
        if (r) {
            wlog("mode 19: clmload chunk %u (%u bytes at %u, flag %#x): no answer\n", chunks + 1, n, off, flag);
            wifi_quick = 0;
            return r < 0 ? -1 : 1;
        }
        if (status) {
            wlog("mode 19: clmload chunk %u (%u bytes at %u, flag %#x): status %d\n", chunks + 1, n, off, flag,
                status);
            wifi_quick = 0;
            return 1;
        }
    }
    wlog("mode 19: CLM blob sent: %u bytes in %u clmload chunks, every one status 0\n", wifi_clm_bytes, chunks);
    r = wifi_host_repost(regions, info, last, *next, 11);
    if (r < 0) goto failed;
    *next += (unsigned)r;
    r = wifi_host_var(regions, info, d2h_mb, last, "mode 19", (*next)++, 262, "clmload_status", NULL, 0, 32,
        WIFI_HOST_IOVAR_IN + 0xa00u, &answer, &bytes, &status);
    if (r) goto unanswered;
    wlog("mode 19: clmload_status (status %d, %u bytes): %u\n", status, bytes, bytes >= 4 ? wifi_le32(answer) : 0);
    r = wifi_host_var(regions, info, d2h_mb, last, "mode 19", (*next)++, 262, "clmver", NULL, 0, 512,
        WIFI_HOST_IOVAR_IN + 0x400u, &answer, &bytes, &status);
    if (r) goto unanswered;
    char text[160];
    unsigned k = 0;
    for (unsigned i = 0; status == 0 && i < bytes && answer[i] && k + 4 < sizeof(text); i++) {
        if (answer[i] == '\n') {
            if (i + 1 < bytes && answer[i + 1]) { text[k++] = ' '; text[k++] = '|'; text[k++] = ' '; }
        } else {
            text[k++] = answer[i] >= 0x20 && answer[i] <= 0x7e ? (char)answer[i] : '.';
        }
    }
    text[k] = 0;
    wlog("mode 19: clmver now (status %d, %u bytes): %s\n", status, bytes, k ? text : "(none)");
    r = wifi_host_repost(regions, info, last, *next, 9);
    if (r < 0) goto failed;
    *next += (unsigned)r;
    r = wifi_host_var(regions, info, d2h_mb, last, "mode 19", (*next)++, 262, "country", NULL, 0, 32,
        WIFI_HOST_IOVAR_IN + 0x600u, &answer, &bytes, &status);
    if (r) goto unanswered;
    const int null_country = status == 0 && bytes >= 12 && answer[0] == '#' && answer[1] == 'n';
    wlog("mode 19: country now (status %d, %u bytes): %s\n", status, bytes,
        status || bytes < 12 ? "not given" : null_country ? "still #n" : "a country of the CLM's");
    if (null_country) {
        static const uint8_t xz[12] = { 'X', 'Z', 0, 0, 11, 0, 0, 0, 'X', 'Z', 0, 0 };   /* wl_country_t */
        r = wifi_host_var(regions, info, d2h_mb, last, "mode 19", (*next)++, 263, "country", xz, sizeof(xz), 20,
            WIFI_HOST_IOVAR_IN + 0x800u, &answer, &bytes, &status);
        if (r) goto unanswered;
        wlog("mode 19: country set to XZ rev 11: status %d\n", status);
    }
    wifi_quick = 0;
    return 0;
unanswered:
    wifi_quick = 0;
    return r < 0 ? -1 : 1;
failed:
    wifi_quick = 0;
    return -1;
}

/*
 * S10 port, mode 17: the radio up, as dhd_preinit_ioctls ends: two more
 * ioctl-response buffers (2 KiB, request ids 11 and 12), then WLC_UP (2, a
 * zero int) and WLC_GET_VAR "isup", then 2 s more for what the firmware
 * sends by itself. Its MSG_TYPE_WL_EVENT completions are logged with the
 * event type and status only (an event packet starts with addresses). No
 * scan or join is asked for, so nothing is asked to be transmitted.
 */
static int
wifi_host_up(struct wifi_region *regions, const uint32_t *info, uint32_t d2h_mb, struct wifi_host_view *last,
    unsigned *next)
{
    const uint8_t *answer = NULL;
    unsigned bytes = 0;
    int status = 0, r;
    static const uint8_t zero[4];
    const uint32_t from = last->d2h_count;
    r = wifi_host_repost(regions, info, last, *next, 11);
    if (r < 0) return -1;
    *next += (unsigned)r;
    wlog("mode 17: completion read index %u given back; two more ioctl-response buffers posted\n",
        last->d2h_write[0]);
    r = wifi_host_var(regions, info, d2h_mb, last, "mode 17", (*next)++, 2, "WLC_UP", zero, 4, 4,
        WIFI_HOST_IOVAR_IN + 0x800u, &answer, &bytes, &status);
    if (r) return r;
    wlog("mode 17: WLC_UP: status %d\n", status);
    const int up = status;
    r = wifi_host_var(regions, info, d2h_mb, last, "mode 17", (*next)++, 262, "isup", NULL, 0, 32,
        WIFI_HOST_IOVAR_IN + 0xa00u, &answer, &bytes, &status);
    if (r) return r;
    wlog("mode 17: isup (status %d, %u bytes): %u\n", status, bytes, bytes >= 4 ? wifi_le32(answer) : 0);
    if (wifi_host_poll(regions, info, d2h_mb, *next, "isup", last)) return -1;
    FlushPoC_DcacheRegion((vm_offset_t)wifi_host, WIFI_HOST_BYTES);
    unsigned events = 0;
    const uint32_t upto = last->d2h_count;
    for (uint32_t i = upto - from >= wifi_rings[2].items ? upto - (wifi_rings[2].items - 1) : from; i < upto; i++) {
        const uint8_t *c = wifi_host + wifi_rings[2].offset + wifi_rings[2].size * (i % wifi_rings[2].items);
        if (c[0] != 0x0e) continue;     /* MSG_TYPE_WL_EVENT */
        if (wifi_le32(c + 4) >= 3 && wifi_le32(c + 4) <= 6) wifi_events_held &= ~(1u << wifi_le32(c + 4));
        uint32_t at = wifi_host_buffer(wifi_le32(c + 4));
        unsigned length = c[12] | c[13] << 8;
        events++;
        if (!at || length < 40 || events > 8) continue;
        /* bcm_event_t: ether header (14), bcmeth header (10), wl_event_msg_t
         * (version, flags, then event_type and status, big-endian) */
        const uint8_t *m = wifi_host + at + 24;
        wlog("mode 17: event %u: %u bytes, type %u status %u\n", events, length,
            (unsigned)m[4] << 24 | (unsigned)m[5] << 16 | (unsigned)m[6] << 8 | m[7],
            (unsigned)m[8] << 24 | (unsigned)m[9] << 16 | (unsigned)m[10] << 8 | m[11]);
    }
    wlog("mode 17: %u firmware events so far\n", events);
    return up ? 2 : 0;
}

static uint32_t
wifi_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* One MSG_TYPE_EVENT_BUF_POST at control submission item `item`. */
static void
wifi_host_event_post(unsigned item, unsigned id)
{
    uint8_t *e = wifi_host + wifi_rings[0].offset + wifi_rings[0].size * (item % wifi_rings[0].items);
    const uint64_t pa = wifi_host_pa + wifi_host_buffer(id);
    wifi_events_held |= 1u << id;
    memset(e, 0, wifi_rings[0].size);
    e[0] = 0x0d;
    e[3] = (uint8_t)((254 + item) % 253);
    wifi_put(e + 4, id, 4);
    wifi_put(e + 8, 0x800u, 2);
    wifi_put(e + 16, (uint32_t)pa, 4);
    wifi_put(e + 20, (uint32_t)(pa >> 32), 4);
}

/* What mode 18 heard. Addresses are kept only to count networks once. */
struct wifi_scan {
    unsigned results, networks, networks_5g, others, malformed, given, logged, done;
    int rssi_min, rssi_max;
    uint32_t final_status, channels[8];
    uint8_t seen[48][6];
};

static void
wifi_scan_event(struct wifi_scan *s, const uint8_t *packet, unsigned length)
{
    const uint8_t *m = packet + 24;     /* wl_event_msg_t: big-endian */
    const uint32_t type = wifi_be32(m + 4), status = wifi_be32(m + 8), datalen = wifi_be32(m + 20);
    if (type != 69) {                   /* WLC_E_ESCAN_RESULT */
        if (++s->others <= 4) wlog("mode 18: event type %u status %u\n", type, status);
        return;
    }
    if (status != 8) {                  /* WLC_E_STATUS_PARTIAL: one more result; anything else ends it */
        s->done = 1;
        s->final_status = status;
        return;
    }
    /* wl_escan_result_t (buflen, version, sync_id, bss_count) then
     * wl_bss_info_t: BSSID at 8, chanspec at 72, RSSI at 78, little-endian */
    const uint8_t *e = m + 48, *bi = e + 12;
    if (datalen < 12 + 80 || 72u + datalen > length || (e[10] | e[11] << 8) < 1 || wifi_le32(bi + 4) < 80 ||
        12u + wifi_le32(bi + 4) > datalen) {
        s->malformed++;
        return;
    }
    const unsigned chanspec = bi[72] | bi[73] << 8, channel = chanspec & 0xffu;
    const int rssi = (int16_t)(bi[78] | bi[79] << 8);
    const char *band = (chanspec & 0xc000u) == 0 ? "2.4" : (chanspec & 0xc000u) == 0xc000u ? "5" : "6";
    unsigned k = 0;
    while (k < s->networks && k < 48 && memcmp(s->seen[k], bi + 8, 6)) k++;
    const int again = k < s->networks && k < 48;
    if (!again) {
        if (s->networks < 48) memcpy(s->seen[s->networks], bi + 8, 6);
        s->networks++;
        s->networks_5g += band[0] != '2';
    }
    s->results++;
    s->channels[channel / 32] |= 1u << (channel % 32);
    if (rssi < s->rssi_min) s->rssi_min = rssi;
    if (rssi > s->rssi_max) s->rssi_max = rssi;
    if (s->logged++ < 12)
        wlog("mode 18: result %u: channel %u (%s GHz), RSSI %d dBm%s\n", s->results, channel, band, rssi,
            again ? ", a network heard before" : "");
}

/*
 * S10 port, mode 18: one passive scan, the radio's first work on air, and
 * listening only: escan with the passive scan type sends no probe request,
 * so nothing is transmitted. First the ioctl-response buffers 7 to 12 are
 * posted again (consumed by now, or never posted), the event buffers the
 * firmware has used are given back, "scan_ver" is read (which wl_scan_params
 * layout the firmware wants), "clmver", "country" and "mpc" are read, and
 * "event_msgs" is read and set with WLC_E_ESCAN_RESULT (69). Then "escan": version 1,
 * WL_SCAN_ACTION_START, any SSID, the broadcast BSSID, any BSS type,
 * passive, the firmware's own channel list, 110 ms each. For up to 8 s every
 * MSG_TYPE_WL_EVENT is read and its buffer posted again, as dhd_prot refills
 * event buffers, until the escan's final status. Logged: counts, channels,
 * signal levels; never a network's name or address. The rings may wrap.
 */
static int
wifi_host_scan(struct wifi_region *regions, const uint32_t *info, uint32_t d2h_mb, struct wifi_host_view *last,
    unsigned *next)
{
    const uint8_t *answer = NULL;
    unsigned bytes = 0;
    int status = 0, r;
    uint64_t deadline;
    static const unsigned pairs[3] = { 9, 11, 7 };     /* 9 and 10, 11 and 12, then 7 and 8 */
    for (unsigned k = 0; k < 3; k++) {
        r = wifi_host_repost(regions, info, last, *next, pairs[k]);
        if (r < 0) return -1;
        *next += (unsigned)r;
    }
    uint32_t at = last->d2h_count;
    unsigned given = 0;
    for (unsigned id = 3; id <= 6; id++) {
        if (!(wifi_events_held & (1u << id))) {
            wifi_host_event_post((*next)++, id);
            given++;
        }
    }
    wlog("mode 18: completion read index %u given back; ioctl-response buffers 7 to 12 posted again, "
        "%u used event buffers given back\n", last->d2h_write[0], given);
    r = wifi_host_var(regions, info, d2h_mb, last, "mode 18", (*next)++, 262, "scan_ver", NULL, 0, 32,
        WIFI_HOST_IOVAR_IN + 0xc00u, &answer, &bytes, &status);
    if (r) return r < 0 ? -1 : 0;
    /* wl_scan_version_t: version, length, scan_ver_major. Boot 79: 1, 6, 2,
     * and escan version 1 was refused with BCME_VERSION (-37). */
    const unsigned major = status == 0 && bytes >= 6 ? (unsigned)(answer[4] | answer[5] << 8) : 0;
    const int v2 = major >= 2;
    if (status == 0 && bytes >= 6)
        wlog("mode 18: scan_ver (status 0, %u bytes): version %u, length %u, major %u; escan version %u sent\n", bytes,
            answer[0] | answer[1] << 8, answer[2] | answer[3] << 8, major, v2 ? 2u : 1u);
    else
        wlog("mode 18: scan_ver (status %d, %u bytes): not given; escan version 1 sent\n", status, bytes);
    /* Boot 80: the escan v2 was refused with BCME_EPERM (-26). What the
     * firmware holds that a scan depends on: its CLM (regulatory) data, its
     * country and its power-saving mode, read only. */
    r = wifi_host_var(regions, info, d2h_mb, last, "mode 18", (*next)++, 262, "clmver", NULL, 0, 512,
        WIFI_HOST_IOVAR_IN + 0x400u, &answer, &bytes, &status);
    if (r) return r < 0 ? -1 : 0;
    char clm[200];
    unsigned clm_n = 0;
    for (unsigned k = 0; status == 0 && k < bytes && answer[k] && clm_n + 4 < sizeof(clm); k++) {
        if (answer[k] == '\n') {
            if (k + 1 < bytes && answer[k + 1]) { clm[clm_n++] = ' '; clm[clm_n++] = '|'; clm[clm_n++] = ' '; }
        } else {
            clm[clm_n++] = answer[k] >= 0x20 && answer[k] <= 0x7e ? (char)answer[k] : '.';
        }
    }
    clm[clm_n] = 0;
    wlog("mode 18: clmver (status %d, %u bytes): %s\n", status, bytes, clm_n ? clm : "(none)");
    r = wifi_host_var(regions, info, d2h_mb, last, "mode 18", (*next)++, 262, "country", NULL, 0, 32,
        WIFI_HOST_IOVAR_IN + 0x600u, &answer, &bytes, &status);
    if (r) return r < 0 ? -1 : 0;
    if (status == 0 && bytes >= 12) {
        /* wl_country_t: country_abbrev[4], rev, ccode[4] */
        char abbrev[5], ccode[5];
        for (unsigned k = 0; k < 4; k++) {
            abbrev[k] = answer[k] >= 0x20 && answer[k] <= 0x7e ? (char)answer[k] : 0;
            ccode[k] = answer[8 + k] >= 0x20 && answer[8 + k] <= 0x7e ? (char)answer[8 + k] : 0;
        }
        abbrev[4] = ccode[4] = 0;
        wlog("mode 18: country (status 0, %u bytes): %s rev %d, ccode %s\n", bytes, abbrev, (int)wifi_le32(answer + 4),
            ccode);
    } else {
        wlog("mode 18: country (status %d, %u bytes): not given\n", status, bytes);
    }
    r = wifi_host_var(regions, info, d2h_mb, last, "mode 18", (*next)++, 262, "mpc", NULL, 0, 32,
        WIFI_HOST_IOVAR_IN + 0x800u, &answer, &bytes, &status);
    if (r) return r < 0 ? -1 : 0;
    if (status == 0 && bytes >= 4) wlog("mode 18: mpc (status 0, %u bytes): %u\n", bytes, wifi_le32(answer));
    else wlog("mode 18: mpc (status %d, %u bytes): not given\n", status, bytes);
    r = wifi_host_var(regions, info, d2h_mb, last, "mode 18", (*next)++, 262, "event_msgs", NULL, 0, 32,
        WIFI_HOST_IOVAR_IN + 0xe00u, &answer, &bytes, &status);
    if (r) return r < 0 ? -1 : 0;
    if (status == 0 && bytes >= 16) {
        uint8_t mask[16];
        memcpy(mask, answer, 16);
        wlog("mode 18: event_msgs (status 0, %u bytes): ESCAN_RESULT %s\n", bytes, mask[8] & 0x20u ? "on" : "off");
        mask[8] |= 0x20u;               /* setbit(mask, WLC_E_ESCAN_RESULT) */
        r = wifi_host_var(regions, info, d2h_mb, last, "mode 18", (*next)++, 263, "event_msgs", mask, 16, 27,
            WIFI_HOST_IOVAR_IN, &answer, &bytes, &status);
        if (r) return r < 0 ? -1 : 0;
        wlog("mode 18: event_msgs set with ESCAN_RESULT: status %d\n", status);
    } else {
        wlog("mode 18: event_msgs (status %d, %u bytes): mask not read, left as it is\n", status, bytes);
    }
    /* wl_escan_params_t (ESCAN_REQ_VERSION 1) or wl_escan_params_v2_t (2):
     * version, action, sync_id, then the scan parameters at 8. Version 1's
     * wl_scan_params_t: SSID at 0, BSSID at 36, bss_type at 42, scan_type
     * (a byte) at 43, nprobes, active, passive and home times at 44 to 56,
     * channel_num at 60. wl_scan_params_v2_t: its own version (2) and length
     * (sizeof, 76) first, SSID at 4, BSSID at 40, bss_type at 46, scan_type
     * (a word) at 48, the times at 52 to 64, channel_num at 68. */
    uint8_t params[80];
    const unsigned q = 8, ssid = v2 ? q + 4 : q, times = v2 ? q + 52 : q + 44, size = v2 ? 80u : 72u;
    memset(params, 0, sizeof(params));
    wifi_put(params, v2 ? 2u : 1u, 4);          /* ESCAN_REQ_VERSION(_V2) */
    wifi_put(params + 4, 1, 2);                 /* WL_SCAN_ACTION_START */
    wifi_put(params + 6, 0x5318, 2);            /* sync_id */
    if (v2) {
        wifi_put(params + q, 2, 2);             /* WL_SCAN_PARAMS_VERSION_V2 */
        wifi_put(params + q + 2, 76, 2);        /* sizeof(wl_scan_params_v2_t) */
    }
    memset(params + ssid + 36, 0xff, 6);        /* no SSID, the broadcast BSSID */
    params[ssid + 42] = 2;                      /* DOT11_BSSTYPE_ANY */
    if (v2) wifi_put(params + q + 48, 1, 4);    /* WL_SCANFLAGS_PASSIVE */
    else params[q + 43] = 1;
    wifi_put(params + times, 0xffffffffu, 4);   /* nprobes: the firmware's */
    wifi_put(params + times + 4, 0xffffffffu, 4);       /* active_time: the firmware's */
    wifi_put(params + times + 8, 110, 4);       /* passive_time, ms per channel */
    wifi_put(params + times + 12, 0xffffffffu, 4);      /* home_time: the firmware's */
    wifi_put(params + times + 16, 0, 4);        /* no channel list: every channel it has */
    r = wifi_host_var(regions, info, d2h_mb, last, "mode 18", (*next)++, 263, "escan", params, size, size + 6,
        WIFI_HOST_IOVAR_IN + 0x200u, &answer, &bytes, &status);
    if (r) return r < 0 ? -1 : 0;
    wlog("mode 18: escan v%u (passive, every channel, 110 ms each) set: status %d\n", v2 ? 2u : 1u, status);
    if (status == -26) {
        /* BCME_EPERM, which DHD takes as "not permitted at this time": once
         * more, 1 s later, with ioctl-response buffers 11 and 12 again. */
        delay_for_interval(1000, NSEC_PER_MSEC);
        clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
        if (wifi_host_view(regions, info, d2h_mb, last, deadline)) return -1;
        r = wifi_host_repost(regions, info, last, *next, 11);
        if (r < 0) return -1;
        *next += (unsigned)r;
        r = wifi_host_var(regions, info, d2h_mb, last, "mode 18", (*next)++, 263, "escan", params, size, size + 6,
            WIFI_HOST_IOVAR_IN + 0xa00u, &answer, &bytes, &status);
        if (r) return r < 0 ? -1 : 0;
        wlog("mode 18: escan v%u once more after 1 s: status %d\n", v2 ? 2u : 1u, status);
    }
    if (status) return 0;
    struct wifi_scan s;
    memset(&s, 0, sizeof(s));
    s.rssi_min = 1000;
    s.rssi_max = -1000;
    unsigned ms = 2000;                 /* the escan's own 2 s of polling */
    uint32_t read_back = UINT32_MAX;
    for (unsigned tick = 0; ; tick++) {
        const uint32_t w = last->d2h_count;
        unsigned posted = 0;
        if (w - at >= wifi_rings[2].items) {
            wlog("mode 18: %u completions since the last read, more than the ring; stopped\n", w - at);
            break;
        }
        if (w != at) FlushPoC_DcacheRegion((vm_offset_t)wifi_host, WIFI_HOST_BYTES);
        for (; at < w; at++) {
            const uint8_t *c = wifi_host + wifi_rings[2].offset + wifi_rings[2].size * (at % wifi_rings[2].items);
            if (c[0] != 0x0e) continue;
            const uint32_t id = wifi_le32(c + 4);
            const unsigned length = c[12] | c[13] << 8;
            if (id < 3 || id > 6) {
                wlog("mode 18: event in buffer id %#x, not an event buffer of ours\n", id);
                continue;
            }
            if (length >= 72 && length <= 0x800u) wifi_scan_event(&s, wifi_host + wifi_host_buffer(id), length);
            else s.malformed++;
            wifi_host_event_post((*next)++, id);
            posted++;
            s.given++;
        }
        wifi_d2h_parsed = at;
        clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
        if (last->d2h_write[0] != read_back && wifi_tcm_set(regions, info[4], last->d2h_write[0], deadline)) return -1;
        read_back = last->d2h_write[0];
        if (posted) {
            const unsigned windex = *next % wifi_rings[0].items;
            CleanPoC_DcacheRegion((vm_offset_t)wifi_host + wifi_rings[0].offset,
                (unsigned long)wifi_rings[0].items * wifi_rings[0].size);
            if (wifi_tcm_set(regions, info[1], windex, deadline) ||
                wifi_bp_write(regions, WIFI_PCIE_CORE + WIFI_H2D_DB0, 0xff000000u | windex, deadline)) {
                wlog("mode 18: event buffer write index or DB0 failed\n");
                return -1;
            }
        }
        if (s.done || tick >= 60) break;
        delay_for_interval(100, NSEC_PER_MSEC);
        ms += 100;
        clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
        if (wifi_host_view(regions, info, d2h_mb, last, deadline)) {
            wlog("mode 18: the link/BAR guard was lost while scanning\n");
            return -1;
        }
    }
    char list[120];
    unsigned n = 0;
    list[0] = 0;
    for (unsigned ch = 1; ch < 256 && n + 5 < sizeof(list); ch++)
        if (s.channels[ch / 32] & (1u << (ch % 32))) n += (unsigned)snprintf(list + n, sizeof(list) - n, " %u", ch);
    if (s.done)
        wlog("mode 18: escan finished after about %u ms, final status %u\n", ms, s.final_status);
    else
        wlog("mode 18: escan not finished after about %u ms\n", ms);
    wlog("mode 18: %u results, %u networks (%u on 2.4 GHz, %u on 5/6 GHz), RSSI %d to %d dBm; %u other events, "
        "%u malformed; %u event buffers given back\n", s.results, s.networks,
        s.networks - s.networks_5g, s.networks_5g, s.results ? s.rssi_min : 0, s.results ? s.rssi_max : 0, s.others,
        s.malformed, s.given);
    wlog("mode 18: channels heard:%s\n", n ? list : " none");
    return 0;
}

/*
 * S10 port, mode 15: what dhd_sync_with_dongle asks first, one at a time:
 * WLC_GET_VAR "ver" (logged) and "cur_etheraddr" (compared with the NVRAM's
 * macaddr, never logged). Mode 16: the address in the private attachment
 * (/chosen mac-address-wifi0) is the one Samsung's DHD gives the chip; when
 * the chip's differs, WLC_SET_VAR "cur_etheraddr" with it, then read it back.
 * Only verdicts are logged, never an address.
 */
static int
wifi_host_hello(struct wifi_region *regions, uint32_t mode, const uint32_t *info, uint32_t d2h_mb,
    struct wifi_host_view *last)
{
    const uint8_t *answer = NULL;
    unsigned bytes = 0;
    int status = 0, r;
    uint8_t chip[6], nvram[6];
    if (last->h2d_read != 7 || last->d2h_write[0] < 2) {
        wlog("mode 15: the ioctl was not answered (read %u, completions %u); no iovars sent\n", last->h2d_read,
            last->d2h_write[0]);
        return 0;
    }
    if (wifi_host_repost(regions, info, last, 7, 7) != 2) return -1;
    wlog("mode 15: completion read index %u given back; two more ioctl-response buffers posted\n",
        last->d2h_write[0]);
    r = wifi_host_var(regions, info, d2h_mb, last, "mode 15", 9, 262, "ver", NULL, 0, 256, WIFI_HOST_IOVAR_IN,
        &answer, &bytes, &status);
    if (r) return r < 0 ? -1 : 0;
    char text[200];
    unsigned n = 0;
    for (; n < bytes && n < sizeof(text) - 1 && answer[n]; n++)
        text[n] = answer[n] >= 0x20 && answer[n] <= 0x7e ? (char)answer[n] : ' ';
    text[n] = 0;
    wlog("mode 15: ver (status %d, %u bytes): %s\n", status, bytes, text);
    r = wifi_host_var(regions, info, d2h_mb, last, "mode 15", 10, 262, "cur_etheraddr", NULL, 0, 32,
        WIFI_HOST_IOVAR_IN + 0x200u, &answer, &bytes, &status);
    if (r) return r < 0 ? -1 : 0;
    if (bytes >= 6) memcpy(chip, answer, 6);
    int have_nvram = wifi_nvram_mac(nvram);
    wlog("mode 15: cur_etheraddr (status %d, %u bytes): %s\n", status, bytes,
        bytes < 6 ? "too short" : !have_nvram ? "no NVRAM macaddr to compare" :
        memcmp(chip, nvram, 6) ? "DIFFERS from the NVRAM macaddr" : "matches the NVRAM macaddr");
    if (mode < 16 || bytes < 6) return 0;
    unsigned next = 11;
    r = wifi_host_mac(regions, info, d2h_mb, last, chip, &next);
    if (r) return r < 0 ? -1 : 0;
    if (mode < 17) return 0;
    if (mode >= 19) {
        r = wifi_host_clm(regions, info, d2h_mb, last, &next);
        if (r) return r < 0 ? -1 : 0;
    }
    r = wifi_host_up(regions, info, d2h_mb, last, &next);
    if (r) return r < 0 ? -1 : 0;   /* unanswered, or WLC_UP refused: no scan */
    return mode >= 18 && wifi_host_scan(regions, info, d2h_mb, last, &next) < 0 ? -1 : 0;
}

/*
 * dhd_prot_init's dongle writes for this firmware (flags: IPC 7, hostready,
 * xorcsum, no DMA indices, 4-byte indices, no IDMA/IFRM/DAR): per common
 * ring base, max items and item size, indices zero (dhd_prot_ring_init),
 * the scratch buffer and its length, host_cap (dhd_set_host_cap: version 7,
 * HOSTCAP_H2D_ENABLE_HOSTRDY, HOSTCAP_DS_NO_OOB_DW, HOSTCAP_UR_FW_NO_TRAP;
 * not EXTENDED_TRAP_DATA, which needs a trap buffer), then dhd_bus_hostready
 * (0x12345678 to DB1). Then dhd_prot_rxbufpost_ctrl twice for ioctl
 * responses and four times for events, one write index update and one DB0
 * with DHD_WRPTR_UPDATE_H2D_DB_MAGIC. Bus mastering stays on for the caller
 * to turn off after the R4 is halted.
 */
static void
wifi_host_start(struct wifi_region *regions, uint32_t mode, uint32_t shared, uint32_t console, uint32_t *console_index)
{
    uint32_t flags = 0, d2h_mb = 0, rings_info = 0, info[5];
    const uint32_t host_cap = 7u | 0x400u | 0x1000u | 0x800000u;
    uint64_t deadline, scratch;
    struct wifi_host_view last;
    clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
    if (wifi_bp_read(regions, shared, &flags, deadline) || wifi_bp_read(regions, shared + 44, &d2h_mb, deadline) ||
        wifi_bp_read(regions, shared + 48, &rings_info, deadline)) {
        wlog("mode 13: shared area unreadable; host side not started\n");
        return;
    }
    if ((flags & 0xffu) != 7 || !(flags & 0x10000000u) || !(flags & 0x40000u) ||
        (flags & (0x10000u | 0x100000u | 0x400000u | 0x1000000u | 0x80000000u))) {
        wlog("mode 13: flags %#x are not IPC 7 with hostready and xorcsum, without DMA or 2-byte indices, "
            "IDMA, IFRM or DAR; host side not started\n", flags);
        return;
    }
    for (unsigned i = 0; i < 5; i++) {
        if (!wifi_in_ram(rings_info, sizeof(info)) || wifi_bp_read(regions, rings_info + 4 * i, &info[i], deadline)) {
            wlog("mode 13: rings_info %#x unreadable; host side not started\n", rings_info);
            return;
        }
    }
    if (!wifi_in_ram(info[0], 80) || !wifi_in_ram(info[1], 8) || !wifi_in_ram(info[2], 8) ||
        !wifi_in_ram(info[3], 12) || !wifi_in_ram(info[4], 12)) {
        wlog("mode 13: ring_info pointers outside RAM; host side not started\n");
        return;
    }
    if (wifi_host_memory()) return;
    wifi_d2h_count = wifi_d2h_ring = wifi_d2h_parsed = 0;
    wifi_events_held = wifi_ioctls_held = 0;
    wifi_quick = 0;
    CleanPoC_DcacheRegion((vm_offset_t)wifi_host, WIFI_HOST_BYTES);
    for (unsigned r = 0; r < 5; r++) {
        uint64_t pa = wifi_host_pa + wifi_rings[r].offset;
        uint32_t at = info[0] + 16 * r;
        if (wifi_tcm_set(regions, at + 4, wifi_rings[r].items | (uint32_t)wifi_rings[r].size << 16, deadline) ||
            wifi_tcm_set(regions, at + 8, (uint32_t)pa, deadline) ||
            wifi_tcm_set(regions, at + 12, (uint32_t)(pa >> 32), deadline)) goto stopped;
    }
    for (unsigned i = 0; i < 3; i++) {
        if ((i < 2 && (wifi_tcm_set(regions, info[1] + 4 * i, 0, deadline) ||
                       wifi_tcm_set(regions, info[2] + 4 * i, 0, deadline))) ||
            wifi_tcm_set(regions, info[3] + 4 * i, 0, deadline) ||
            wifi_tcm_set(regions, info[4] + 4 * i, 0, deadline)) goto stopped;
    }
    scratch = wifi_host_pa + WIFI_HOST_SCRATCH;
    if (wifi_tcm_set(regions, shared + 56, (uint32_t)scratch, deadline) ||
        wifi_tcm_set(regions, shared + 60, (uint32_t)(scratch >> 32), deadline) ||
        wifi_tcm_set(regions, shared + 52, 8, deadline) ||
        wifi_tcm_set(regions, shared + 84, host_cap, deadline)) goto stopped;
    wlog("mode 13: five common rings at pa %#llx, indices zero, scratch at %#llx (8 bytes), host_cap %#x: "
        "written and read back\n", (unsigned long long)wifi_host_pa, (unsigned long long)scratch, host_cap);
    if (wifi_master_set(regions, 4, deadline)) {
        /* Half on (the root port only) would fail every later guard: off now. */
        int off = wifi_master_set(regions, 0, deadline);
        wlog("mode 13: bus mastering did not come on; nothing rung; off again %s\n", off ? "NOT confirmed" : "verified");
        return;
    }
    wlog("mode 13: bus mastering on: root port, then endpoint\n");
    if (wifi_bp_write(regions, WIFI_PCIE_CORE + WIFI_H2D_DB1, 0x12345678u, deadline)) {
        wlog("mode 13: hostready doorbell write failed\n");
        return;
    }
    wlog("mode 13: hostready doorbell DB1 rung\n");
    delay_for_interval(200, NSEC_PER_MSEC);
    clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
    if (wifi_host_view(regions, info, d2h_mb, &last, deadline)) {
        wlog("mode 13: state after hostready unreadable\n");
        return;
    }
    wifi_log_view("after hostready", 6, &last);
    wifi_log_console(regions, console, deadline, console_index);
    /* dhd_prot_rxbufpost_ctrl: msg_type, if_id 0, flags 0 (no H2D phase),
     * epoch from seqnum H2D_EPOCH_INIT_VAL (254) modulo 253, a request id,
     * the buffer length and its address. */
    for (unsigned i = 0; i < 6; i++) {
        uint8_t *item = wifi_host + wifi_rings[0].offset + wifi_rings[0].size * i;
        uint64_t pa = wifi_host_pa + (i < 2 ? WIFI_HOST_IOCTL + 0x2000u * i : WIFI_HOST_EVENT + 0x800u * (i - 2));
        item[0] = i < 2 ? 0x0b : 0x0d;  /* MSG_TYPE_IOCTLRESP_BUF_POST, MSG_TYPE_EVENT_BUF_POST */
        item[3] = (uint8_t)((254 + i) % 253);
        wifi_put(item + 4, 1 + i, 4);
        wifi_put(item + 8, i < 2 ? 0x2000u : 0x800u, 2);
        wifi_put(item + 16, (uint32_t)pa, 4);
        wifi_put(item + 20, (uint32_t)(pa >> 32), 4);
    }
    CleanPoC_DcacheRegion((vm_offset_t)wifi_host + wifi_rings[0].offset, 6u * wifi_rings[0].size);
    if (wifi_tcm_set(regions, info[1], 6, deadline) ||
        wifi_bp_write(regions, WIFI_PCIE_CORE + WIFI_H2D_DB0, 0xff000006u, deadline)) {
        wlog("mode 13: control submission write index or DB0 failed\n");
        return;
    }
    wifi_ioctls_held = 0x6u;            /* request ids 1 and 2 */
    wifi_events_held = 0x78u;           /* 3 to 6 */
    wlog("mode 13: 2 ioctl-response and 4 event buffers posted, control submission write index 6, DB0 %#x\n",
        0xff000006u);
    if (wifi_host_poll(regions, info, d2h_mb, 6, "posts", &last)) return;
    if (mode >= 14 && wifi_host_ioctl(regions, info, d2h_mb, &last)) return;
    if (mode >= 15 && wifi_host_hello(regions, mode, info, d2h_mb, &last)) return;
    wifi_log_host_writes(&last, mode < 15);
    clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
    wifi_log_console(regions, console, deadline, console_index);
    return;
stopped:
    wlog("mode 13: host side stopped before bus mastering; nothing rung\n");
}

static int
wifi_firmware_boot(struct wifi_region *regions, uint32_t mode)
{
    const uint32_t top = WIFI_RAM_END - 4, varsize = wifi_nvram_bytes, varaddr = top - varsize;
    const uint32_t token = (~(varsize / 4) << 16) | ((varsize / 4) & 0xffffu);
    const uint32_t seed_meta = varaddr - 8, seed_addr = seed_meta - 128;
    const uint32_t vector = wifi_firmware_word(0);
    uint8_t seed[128], meta[8], token_bytes[4];
    uint32_t before = 0, got = 0, shared = 0, words[8] = { 0 }, console_index = 0;
    unsigned polls = 0, released = 0;
    uint64_t deadline;
    clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
    wlog("firmware boot begin: NVRAM %u bytes at %#x, token %#x at %#x, seed at %#x; %s\n",
        varsize, varaddr, token, top, seed_addr,
        mode >= 13 ? "IRQs stay off; DMA/BME only for mode 13's rings" : "DMA/BME/IRQs stay off");
    for (unsigned i = 0; i < 4; i++) token_bytes[i] = (uint8_t)(token >> (8 * i));
    for (unsigned i = 0; i < 4; i++) {
        meta[i] = (uint8_t)(0xfeedc0deu >> (8 * i));
        meta[4 + i] = (uint8_t)(128u >> (8 * i));
    }
    read_frandom(seed, sizeof(seed));
    if (!wifi_firmware_halted(regions, deadline) ||
        wifi_bp_block(regions, varaddr, wifi_nvram, varsize, deadline) ||
        wifi_bp_block(regions, top, token_bytes, 4, deadline) ||
        wifi_bp_block(regions, seed_meta, meta, 8, deadline) ||
        wifi_bp_block(regions, seed_addr, seed, sizeof(seed), deadline)) {
        wlog("firmware boot stopped: NVRAM/token/seed not written; R4 stays halted\n");
        return 0;
    }
    /* dhdpcie_bus_download_state: the image's first word goes to backplane
     * address 0; all ones there means a secured vector needing a signature. */
    if (wifi_bp_read(regions, 0, &before, deadline) || before == UINT32_MAX ||
        wifi_bp_write(regions, 0, vector, deadline) ||
        wifi_bp_read(regions, 0, &got, deadline) || got != vector ||
        !wifi_firmware_halted(regions, deadline)) {
        wlog("firmware boot stopped: reset vector before=%#x got=%#x want=%#x; R4 stays halted\n", before, got, vector);
        return 0;
    }
    wlog("NVRAM, token, seed and reset vector written and read back (address 0 held %#x); "
        "pausing 6 s so these lines reach the disk before the R4 runs\n", before);
    delay_for_interval(6000, NSEC_PER_MSEC);
    clock_interval_to_deadline(5000, NSEC_PER_MSEC, &deadline);
    if (wifi_r4_reset(regions, 0, deadline)) {
        wlog("R4 release failed; halting it again\n");
        goto halt;
    }
    released = 1;
    wlog("R4 released: firmware running; waiting up to 2 s for its shared area\n");
    /* dhdpcie_readshared: the last RAM word changes from the token to the
     * address of pciedev_shared_t. */
    for (; polls < 100; polls++) {
        if (wifi_bp_read(regions, top, &shared, deadline)) {
            wlog("shared poll lost the link/BAR guard after %u polls\n", polls);
            goto halt;
        }
        if (shared != 0 && shared != token) break;
        delay_for_interval(20, NSEC_PER_MSEC);
    }
    if (shared == 0 || shared == token || shared == UINT32_MAX || shared < WIFI_RAM_BASE || shared > top - 32) {
        wlog("shared area not published: last word=%#x after %u polls\n", shared, polls);
        goto halt;
    }
    /* Let the firmware finish its start-up messages, then read only. */
    for (unsigned i = 0; i < 5; i++) delay_for_interval(200, NSEC_PER_MSEC);
    for (unsigned i = 0; i < 8; i++) {
        if (wifi_bp_read(regions, shared + 4 * i, &words[i], deadline)) {
            wlog("shared area read failed at word %u\n", i);
            goto halt;
        }
    }
    wlog("shared at %#x after %u polls: flags=%#x version=%u trap=%#x assert_exp=%#x assert_file=%#x "
        "assert_line=%u console=%#x msgtrace=%#x fwid=%#x\n", shared, polls, words[0], words[0] & 0xffu,
        words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
    wifi_log_console(regions, words[5], deadline, &console_index);
    if (words[0] & 0x200u) {
        wifi_log_string(regions, "assert file", words[3], deadline);
        wifi_log_string(regions, "assert expression", words[2], deadline);
    }
    if ((words[0] & 0x400u) && words[1] >= WIFI_RAM_BASE && words[1] <= top - 80) {
        uint32_t trap[20];
        for (unsigned i = 0; i < 20; i++)
            if (wifi_bp_read(regions, words[1] + 4 * i, &trap[i], deadline)) goto halt;
        wlog("firmware trap: type=%#x epc=%#x cpsr=%#x spsr=%#x\n", trap[0], trap[1], trap[2], trap[3]);
        for (unsigned i = 0; i < 16; i += 4)
            wlog("firmware trap: r%u=%#x r%u=%#x r%u=%#x r%u=%#x\n", i, trap[4 + i], i + 1, trap[5 + i],
                i + 2, trap[6 + i], i + 3, trap[7 + i]);
    }
    if (mode >= 12) {
        /* S10 port, mode 12: the host-side layout, then 2 s more of the
         * firmware on its own and what it printed meanwhile. */
        clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
        wifi_log_rings(regions, shared, deadline);
        for (unsigned i = 0; i < 10; i++) delay_for_interval(200, NSEC_PER_MSEC);
        clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
        uint32_t flags = 0;
        if (!wifi_bp_read(regions, shared, &flags, deadline))
            wlog("after 2 s more: shared flags %#x%s%s\n", flags, (flags & 0x400u) ? " TRAP" : "",
                (flags & 0x200u) ? " ASSERT" : "");
        wifi_log_console(regions, words[5], deadline, &console_index);
    }
    if (mode >= 13) wifi_host_start(regions, mode, shared, words[5], &console_index);
halt:
    /* dhdpcie_bus_download_state(enter): halt the ARM and remove reset. */
    clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
    int halted = !wifi_r4_reset(regions, 0x20, deadline) && wifi_firmware_halted(regions, deadline);
    if (wifi_master) {
        /* S10 port, mode 13: the R4 first, so nothing new is queued, then
         * bus mastering off; the power-off follows either way. */
        clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
        if (wifi_master_set(regions, 0, deadline)) {
            wlog("mode 13: bus mastering could NOT be confirmed off; power-off follows\n");
            wifi_master = 0;
            halted = 0;
        } else {
            wlog("mode 13: bus mastering off again (endpoint, then root port); host memory kept, never reused\n");
        }
    }
    if (!halted) {
        wlog("firmware boot end: R4 was %s and could NOT be halted again; power-off follows\n",
            released ? "released" : "not released");
        return -1;
    }
    wlog("firmware boot end: R4 %s, halted again; power-off follows\n",
        released ? "ran" : "never ran");
    return 0;
}

static int
wifi_chipcommon_probe(struct wifi_region *regions, uint32_t mode)
{
    const struct wifi_region *dbi = &regions[8], *cfg = &regions[9], *bar = &regions[10];
    static const struct { unsigned off; uint32_t value; } descriptor[] = {
        { 0x90c, 0x14400000 }, { 0x910, 0 }, { 0x914, 0x14400fff },
        { 0x918, 0x14400000 }, { 0x91c, 0 }, { 0x904, 0 },
    };
    static const unsigned offsets[] = { 0, 4, 0xfc };
    uint32_t words[3] = { 0 }, low = 0, high = 0, window = 0, command = 0;
    int atu_owned = 0, ep_changed = 0, cleanup_failed = 0, cfg_selected = 0;
    unsigned erom_words = 0, target_signature = 0;
    uint32_t r4_cap = 0;
    unsigned r4_halted = 0;
    unsigned measured_tcm = 0;
    const char *stage = "initial-state";
    uint64_t deadline;
    clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
    if (!wifi_cfg_ready(regions, deadline)) {
        wlog("chipcommon probe skipped: configuration prerequisites\n");
        return 0;
    }
    /* Cached recovery resources establish BAR0 as a 64 KiB 64-bit aperture at
     * 0x14400000. Do not size-probe BARs or touch BAR2's dongle RAM aperture. */
    static const unsigned config_offsets[] = { 0x04, 0x10, 0x14, 0x80 };
    uint32_t saved[4];
    for (unsigned i = 0; i < sizeof(config_offsets) / sizeof(config_offsets[0]); i++) {
        if (!wifi_cfg_ready(regions, deadline)) {
            wlog("chipcommon probe skipped before config +%#x: prerequisites\n", config_offsets[i]);
            return 0;
        }
        saved[i] = wifi_read(cfg, 0x2000 + config_offsets[i]);
    }
    command = saved[0]; low = saved[1]; high = saved[2]; window = saved[3];
    if (command == UINT32_MAX || (command & 7u) || high != 0 ||
        (low != 4 && low != 0x14400004) || window == UINT32_MAX) {
        wlog("chipcommon probe skipped: command=%#x BAR0=%#x:%#x window=%#x\n",
            command, high, low, window);
        return 0;
    }
    wlog("chipcommon probe begin: BAR0 first 4 KiB only; no chip writes, RAM, DMA or firmware\n");
    if (wifi_bits(dbi, 0x900, UINT32_MAX, 0)) goto out;
    uint32_t control = wifi_read(dbi, 0x908);
    if (control != 0) {
        wlog("chipcommon probe skipped: outbound ATU0 control=%#x is not inactive zero\n", control);
        goto out;
    }
    atu_owned = 1;
    stage = "bar0-route";
    for (unsigned i = 0; i < sizeof(descriptor) / sizeof(descriptor[0]); i++) {
        wifi_write(dbi, descriptor[i].off, descriptor[i].value);
        if (wifi_read(dbi, descriptor[i].off) != descriptor[i].value) goto out;
    }
    if (wifi_atu_enable(dbi, 0) ||
        wifi_bits(dbi, 0x900, UINT32_MAX, 1)) goto out;
    stage = "bar0-config";
    ep_changed = 1;
    if (wifi_cfg_write_checked(regions, 0x14, 0, deadline) ||
        wifi_cfg_write_checked(regions, 0x10, 0x14400004, deadline) ||
        wifi_cfg_write_checked(regions, 0x80, 0x18000000, deadline) ||
        wifi_ep_memory_command(regions, 2, deadline)) goto out;
    stage = "chipcommon-reads";
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        if (!wifi_bar_ready(regions, 0x18000000, deadline)) goto out;
        words[i] = wifi_read(bar, offsets[i]);
        wlog("chipcommon +%#x raw=%#x\n", offsets[i], words[i]);
        if (words[i] == UINT32_MAX || (i == 0 && (words[i] & 0xffffu) != 0x4375)) goto out;
    }
    wlog("chipcommon identity verified: chip=%#x revision=%u package=%u interconnect=%u; EROM=%#x not followed\n",
        words[0] & 0xffffu, (words[0] >> 16) & 15u, (words[0] >> 20) & 15u,
        words[0] >> 28, words[2]);
    if (mode >= 7) {
        stage = "erom-prerequisites";
        if (words[0] != 0x10054375 || words[2] != 0x18111000) {
            wlog("EROM capture skipped: chip identity/pointer differs from measured baseline\n");
            goto out;
        }
        stage = "erom-window";
        if (wifi_ep_memory_command(regions, 0, deadline) ||
            wifi_cfg_write_checked(regions, 0x80, 0x18111000, deadline) ||
            wifi_ep_memory_command(regions, 2, deadline)) goto out;
        stage = "erom-capture";
        if (wifi_erom_capture(regions, &erom_words, &target_signature)) goto out;
        if (mode >= 8) {
            stage = "core-prerequisites";
            if (erom_words != 133 || target_signature != 0xff) {
                wlog("core snapshot skipped: measured EROM witnesses/count differ words=%u signature=%#x\n",
                    erom_words, target_signature);
                goto out;
            }
            stage = "core-snapshot";
            if (wifi_core_snapshot(regions, &r4_cap, &r4_halted)) goto out;
            if (mode >= 9) {
                stage = "tcm-prerequisites";
                if (!r4_halted || r4_cap != 0x351585) {
                    wlog("TCM inventory skipped: measured halted-R4/capability prerequisites absent cap=%#x halted=%u\n",
                        r4_cap, r4_halted);
                    goto out;
                }
                stage = "tcm-inventory";
                if (wifi_tcm_inventory(regions, &cleanup_failed, &measured_tcm)) goto out;
                if (mode >= 10) {
                    stage = "firmware-prerequisites";
                    if (!measured_tcm || !wifi_firmware || (mode >= 11 && !wifi_nvram)) {
                        wlog("firmware download skipped: measured TCM/input prerequisites absent\n");
                        goto out;
                    }
                    stage = "firmware-download";
                    if (wifi_firmware_download(regions)) goto out;
                    if (mode >= 11) {
                        stage = "firmware-boot";
                        if (wifi_firmware_boot(regions, mode)) {
                            cleanup_failed = -1;
                            goto out;
                        }
                    }
                }
            }
        }
    }
    stage = "complete";
out:
    /* Disable memory decoding before restoring BARs. PCI status W1C bits are
     * written as zero; bus mastering was never enabled, or (mode 13) is off
     * again since the R4 was halted. If the link is lost,
     * do not issue more config requests; caller still asserts PERST/power off. */
    cfg_selected = !wifi_restore_bits(dbi, 0x900, UINT32_MAX, 1);
    if (!cfg_selected) cleanup_failed = -1;
    if (ep_changed) {
        int disabled = 0;
        for (unsigned attempt = 0; cfg_selected && attempt < 3; attempt++) {
            if (!wifi_ep_memory_command(regions, 0, UINT64_MAX)) {
                disabled = 1; break;
            }
            delay_for_interval(1, NSEC_PER_MSEC);
        }
        if (!disabled) cleanup_failed = -1;
        else {
            cleanup_failed |= wifi_cfg_restore(regions, 0x80, window);
            cleanup_failed |= wifi_cfg_restore(regions, 0x14, high);
            cleanup_failed |= wifi_cfg_restore(regions, 0x10, low);
        }
    }
    if (atu_owned) {
        if (wifi_restore_bits(dbi, 0x900, UINT32_MAX, 0)) cleanup_failed = -1;
        else cleanup_failed |= wifi_restore_bits(dbi, 0x908, UINT32_MAX, 0);
    }
    cleanup_failed |= wifi_restore_bits(dbi, 0x900, UINT32_MAX, 1);
    wlog("chipcommon probe end: stage=%s cleanup=%s; BAR0 decoding and owned ATU0 disable requested\n",
        stage, cleanup_failed ? "FAILED" : "verified");
    return cleanup_failed;
}

/* Samsung uses outbound viewport 1 for CFG0, bus 1/device 0/function 0.
 * The live DT's 0x14ffe000/0x2000 config resource is split into two 4 KiB
 * CFG0/CFG1 halves. Map a 16 KiB aligned envelope but read only four words
 * in CFG0. Never map a BAR, program a memory/inbound region or write the EP. */
static int
wifi_endpoint_identity(struct wifi_region *regions, uint32_t mode)
{
    const struct wifi_region *elbi = &regions[4], *dbi = &regions[8], *cfg = &regions[9];
    static const unsigned offsets[] = { 0x00, 0x04, 0x08, 0x2c };
    static const struct { unsigned off; uint32_t value; } descriptor[] = {
        { 0x90c, 0x14ffe000 }, { 0x910, 0 }, { 0x914, 0x14ffefff },
        { 0x918, 0x01000000 }, { 0x91c, 0 }, { 0x904, 4 },
    };
    uint32_t words[4] = { 0 }, viewport = wifi_read(dbi, 0x900);
    uint32_t buses = wifi_read(dbi, 0x18);
    uint32_t header = wifi_read(dbi, 0x0c), initial_command = wifi_read(dbi, 0x04);
    int viewport_changed = 0, buses_changed = 0, atu_owned = 0, cleanup_failed = 0;
    const char *stage = "initial-state";
    if (viewport == UINT32_MAX || buses == UINT32_MAX || header == UINT32_MAX ||
        ((header >> 16) & 0x7fu) != 1 || initial_command == UINT32_MAX ||
        (initial_command & 0x147u) != 2) {
        wlog("endpoint probe skipped: invalid viewport/bus/header/root-command state\n");
        return 0;
    }
    wlog("endpoint probe begin: CFG0 01:00.0, four read-only words; root BME stays off\n");
    viewport_changed = 1;
    if (wifi_bits(dbi, 0x900, UINT32_MAX, 1)) goto out;
    uint32_t control = wifi_read(dbi, 0x908);
    if (control == UINT32_MAX || control != 0) {
        wlog("endpoint probe skipped: outbound ATU1 control=%#x is not inactive zero\n", control);
        goto out;
    }
    atu_owned = 1;
    buses_changed = 1;
    stage = "cfg0-programming";
    if (wifi_bits(dbi, 0x18, 0xffffff, 0x010100)) goto out;
    /* Own all descriptor bits while disabled. Like Samsung, write full words;
     * an old address/limit of all ones is data, not a failed RMW prerequisite.
     * Every requested new word is non-all-ones and must read back exactly. */
    for (unsigned i = 0; i < sizeof(descriptor) / sizeof(descriptor[0]); i++) {
        wifi_write(dbi, descriptor[i].off, descriptor[i].value);
        uint32_t checked = wifi_read(dbi, descriptor[i].off);
        if (checked != descriptor[i].value) {
            wlog("endpoint ATU1 mismatch off=%#x got=%#x want=%#x\n",
                descriptor[i].off, checked, descriptor[i].value);
            goto out;
        }
    }
    /* DesignWare enable readback: one write and at most five 9 ms waits. */
    if (wifi_atu_enable(dbi, 1)) goto out;
    /* Conservative fixed readiness interval; no retry of endpoint requests. */
    delay_for_interval(100, NSEC_PER_MSEC);
    uint64_t deadline;
    clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
    stage = "identity-reads";
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        uint32_t link = wifi_read(elbi, 0x74), status = wifi_read(dbi, 0x80);
        uint32_t command = wifi_read(dbi, 0x04), atu = wifi_read(dbi, 0x908);
        if (mach_absolute_time() >= deadline || link == UINT32_MAX ||
            (link & 31u) < 0x0d || (link & 31u) > 0x14 || status == UINT32_MAX ||
            ((status >> 16) & 15u) != 1 || ((status >> 20) & 63u) != 1 ||
            command == UINT32_MAX || (command & 0x147u) != 2 || atu != 0x80000000) {
            wlog("endpoint probe stopped before +%#x: link/root/ATU/deadline guard\n", offsets[i]);
            goto out;
        }
        words[i] = wifi_read(cfg, 0x2000 + offsets[i]);
        wlog("endpoint CFG0 +%#x raw=%#x\n", offsets[i], words[i]);
        if (words[i] == UINT32_MAX || (i == 0 && words[i] != 0x447514e4) ||
            (i == 1 && (words[i] & 7u)) || (i == 3 && words[i] != 0x437514e4)) {
            wlog("endpoint probe stopped: unexpected identity/command at +%#x\n", offsets[i]);
            goto out;
        }
    }
    wlog("endpoint identity verified: 14e4:4475 subsystem 14e4:4375 PCI revision=%u class=%#x; no BAR/DMA/firmware\n",
        words[2] & 255u, words[2] >> 8);
    stage = "complete";
    if (mode >= 6) cleanup_failed |= wifi_chipcommon_probe(regions, mode);
out:
    /* Leave our outbound descriptor disabled, retaining its programming bytes.
     * Restore root bus numbers and viewport selection before PHY shutdown. */
    if (atu_owned) {
        if (wifi_restore_bits(dbi, 0x900, UINT32_MAX, 1)) cleanup_failed = -1;
        else cleanup_failed |= wifi_restore_bits(dbi, 0x908, UINT32_MAX, 0);
    }
    if (buses_changed) cleanup_failed |= wifi_restore_bits(dbi, 0x18, 0xffffff, buses);
    if (viewport_changed) cleanup_failed |= wifi_restore_bits(dbi, 0x900, UINT32_MAX, viewport);
    wlog("endpoint probe end: stage=%s cleanup=%s; owned CFG0 disable requested; no firmware/networking\n",
        stage, cleanup_failed ? "FAILED" : "verified");
    return cleanup_failed;
}

static void
wifi_gen1_probe(struct wifi_region *regions, uint32_t mode)
{
    const struct wifi_region *gpio = &regions[2], *elbi = &regions[4];
    const struct wifi_region *phy = &regions[5], *pcs = &regions[6];
    const struct wifi_region *sysreg = &regions[7], *dbi = &regions[8];
    struct { unsigned off; uint32_t mask, value; } saved[] = {
        { 0x18, 2, 0 }, { 0xf4, 0x11, 0 }, { 0x1b8, 3, 0 }, { 0x2c8, 0xfff, 0 },
    };
    uint32_t clock_mux, clock_pull, hs, lane, idle;
    uint32_t ro_before = 0, link = 0, status = 0;
    int ro_changed = 0, dbi_ready = 0, trained = 0, cleanup_failed = 0;
    unsigned polls = 0;
    const char *stage = "pins-and-power";
    /* Accept the inactive controller/reset state verified in epoch 51. */
    if (wifi_read(elbi, 0x2c) != 0 || (wifi_read(elbi, 0x74) & 31u) != 0 ||
        wifi_read(elbi, 0x1d0) != 1 || wifi_read(elbi, 0x288) != 1 ||
        wifi_read(elbi, 0x28c) != 1 || wifi_read(elbi, 0x290) != 1) {
        wlog("Gen1 probe skipped: controller differs from verified idle baseline\n");
        return;
    }
    clock_mux = wifi_read(gpio, 0x20); clock_pull = wifi_read(gpio, 0x28);
    if (clock_mux == UINT32_MAX || clock_pull == UINT32_MAX || (clock_mux & 15u) != 0) {
        wlog("Gen1 probe skipped: CLKREQ pad differs from verified idle baseline\n");
        return;
    }
    hs = wifi_read(sysreg, 0x1044); lane = wifi_read(sysreg, 0x1050);
    idle = wifi_read(pcs, 0xec);
    if (hs == UINT32_MAX || lane == UINT32_MAX || idle == UINT32_MAX) {
        wlog("Gen1 probe skipped: controller/shared state differs from verified idle baseline\n");
        return;
    }
    for (unsigned i = 0; i < sizeof(saved) / sizeof(saved[0]); i++) {
        saved[i].value = wifi_read(elbi, saved[i].off);
        if (saved[i].value == UINT32_MAX) {
            wlog("Gen1 probe skipped: invalid ELBI control word\n");
            return;
        }
    }
    wlog("Gen1 probe begin: channel 0, 26 MHz PHY profile, x1; one 500 ms attempt\n");
    /* Keep all PCIe interrupt sources masked. There is no IRQ handler or DMA
     * client in this experiment. Do not restore an old interrupt-enable mask. */
    if (wifi_bits(elbi, 0x0c, UINT32_MAX, 0) || wifi_bits(elbi, 0x10, UINT32_MAX, 0) ||
        wifi_bits(elbi, 0x14, UINT32_MAX, 0) ||
        wifi_bits(gpio, 0x28, 15u, 3u) || wifi_bits(gpio, 0x20, 15u, 2u) ||
        wifi_bits(pcs, 0x100, 1u << 6, 0) || wifi_bits(pcs, 0x104, 1u << 7, 0))
        goto out;
    wifi_write(phy, 0x80, 0xc0); wifi_write(phy, 0x15c, 0x7e);
    if (wifi_bits(pcs, 0xec, 8u, 8u)) goto out; /* Ignore RX electrical idle for DBI access. */
    wifi_write(elbi, 0x1d0, 0); delay(20);
    if (wifi_bits(elbi, 0x1d0, 1, 1) || wifi_bits(gpio, 0x24, 4, 4)) goto out;
    delay_for_interval(20, NSEC_PER_MSEC);
    stage = "phy-calibration";
    if (wifi_bits(elbi, 0xf4, 0x11, 0x11) || wifi_bits(elbi, 0x1b8, 3, 2) ||
        wifi_bits(elbi, 0x2c8, 0xfff, 0) || wifi_bits(sysreg, 0x1044, 2, 2) ||
        wifi_phy_sysreg_profile(sysreg) ||
        wifi_bits(sysreg, 0x1050, 0x1000, 0x1000) || wifi_reset_pulse(elbi, 0x288, 10))
        goto out;
    for (unsigned i = 0; i < sizeof(b2l_wifi_cmn); i++) wifi_write(phy, i * 4, b2l_wifi_cmn[i]);
    for (unsigned i = 0; i < sizeof(b2l_wifi_trsv); i++) wifi_write(phy, 0xc0 + i * 4, b2l_wifi_trsv[i]);
    wifi_write(pcs, 0xf8, 0x70); wifi_write(pcs, 0x100, 0x87); wifi_write(pcs, 0x104, 0x50);
    if (wifi_bits(pcs, 0x0c, 0x12, 0x10) || wifi_reset_pulse(elbi, 0x290, 10) ||
        wifi_reset_pulse(elbi, 0x28c, 100)) goto out;
    uint32_t cdr = wifi_read(pcs, 0xd0);
    if (cdr == UINT32_MAX) { wlog("Gen1 CDR initial read invalid\n"); goto out; }
    wifi_write(pcs, 0xd0, cdr | 0xc0); delay(20);
    cdr = wifi_read(pcs, 0xd0);
    if (cdr == UINT32_MAX) { wlog("Gen1 CDR pulse read invalid\n"); goto out; }
    wifi_write(pcs, 0xd0, (cdr & ~0xc0u) | 0x80); wifi_write(pcs, 0xd0, cdr & ~0xc0u);
    if (wifi_bits(elbi, 0x18, 2, 0)) goto out; /* Enable bus-number handling; no ATU. */
    stage = "root-dbi";
    uint32_t root_id = wifi_read(dbi, 0);
    ro_before = wifi_read(dbi, 0x8bc);
    /* Samsung programs the root vendor/device IDs during full PCI bus setup;
     * zero can be an uninitialized root identity, not an absent endpoint. */
    if (root_id == UINT32_MAX || ro_before == UINT32_MAX) {
        wlog("Gen1 probe stopped: invalid root DBI identity/control\n");
        goto out;
    }
    wlog("Gen1 root DBI ID=%#x; endpoint remains unconfigured\n", root_id);
    dbi_ready = 1;
    stage = "root-policy";
    /* Root command: memory decoding only, never bus mastering. Status W1C
     * fields are written as zero. No endpoint configuration window is mapped. */
    if (wifi_control16(dbi, 0x04, 0x147, 2) || wifi_control16(dbi, 0x44, 0x103, 0)) goto out;
    ro_changed = 1;
    if (wifi_bits(dbi, 0x8bc, 1, 1) || wifi_bits(dbi, 0x710, 0x3f0000, 0x10000) ||
        wifi_bits(dbi, 0x80c, 0x1f00, 0x100) ||
        wifi_bits(dbi, 0x7c, 0x383ff, 0x38012) || wifi_bits(dbi, 0xb40, UINT32_MAX, 26) ||
        wifi_control16(dbi, 0xa0, 15, 1) || wifi_control16(dbi, 0x80, 3, 0) ||
        wifi_restore_bits(dbi, 0x8bc, 1, ro_before)) goto out;
    ro_changed = 0;
    uint32_t width = wifi_read(dbi, 0x80c), control = wifi_read(dbi, 0x80);
    if (width == UINT32_MAX || control == UINT32_MAX) goto out;
    wifi_write(dbi, 0x80c, width | (1u << 17));
    wifi_write(dbi, 0x80, (control & 0xffffu) | 0x20u);
    if (wifi_bits(elbi, 0x0c, UINT32_MAX, 0) || wifi_bits(elbi, 0x10, UINT32_MAX, 0) ||
        wifi_bits(elbi, 0x14, UINT32_MAX, 0) || wifi_bits(pcs, 0xec, 8, 0) ||
        wifi_bits(elbi, 0x2c, 1, 1)) goto out;
    stage = "link-training";
    uint64_t deadline;
    clock_interval_to_deadline(500, NSEC_PER_MSEC, &deadline);
    for (unsigned tick = 0; tick < 500 && mach_absolute_time() < deadline; tick++) {
        link = wifi_read(elbi, 0x74);
        polls++;
        if (link == UINT32_MAX) break;
        unsigned state = link & 31u;
        if (state >= 0x0d && state <= 0x14) {
            /* Like the vendor DBI accessor, avoid RX-idle clock gating while
             * sampling config, including a link that drops during the read. */
            if (wifi_bits(pcs, 0xec, 8, 8)) goto out;
            status = wifi_read(dbi, 0x80);
            if (wifi_bits(pcs, 0xec, 8, 0) || status == UINT32_MAX) goto out;
            unsigned speed = (status >> 16) & 15u, lanes = (status >> 20) & 63u;
            if (speed == 1 && lanes == 1) { trained = 1; break; }
            if (speed > 1 || lanes > 1) break;
        }
        delay_for_interval(1, NSEC_PER_MSEC);
    }
    wlog("Gen1 link %s: ELBI=%#x speed=%u lanes=%u; endpoint/DMA/firmware untouched\n",
        trained ? "up" : "not established", link, (status >> 16) & 15u, (status >> 20) & 63u);
    if (trained && mode >= 5) {
        if (wifi_bits(pcs, 0xec, 8, 8)) {
            wlog("endpoint probe skipped: DBI access prerequisite failed\n");
            goto out;
        }
        cleanup_failed |= wifi_endpoint_identity(regions, mode);
    }
out:
    if (!trained) wlog("Gen1 probe stopped: stage=%s polls=%u last_ELBI=%#x\n", stage, polls, link);
    /* Assert endpoint reset, stop training and force the vendor PHY power-down
     * state before the caller re-isolates and removes WLAN power. Internal
     * PHY/DBI programming is retained, not claimed to match S-Boot bytewise. */
    cleanup_failed |= wifi_restore_bits(gpio, 0x24, 4, 0);
    cleanup_failed |= wifi_restore_bits(elbi, 0x2c, 1, 0);
    cleanup_failed |= wifi_restore_bits(elbi, 0x0c, UINT32_MAX, 0);
    cleanup_failed |= wifi_restore_bits(elbi, 0x10, UINT32_MAX, 0);
    cleanup_failed |= wifi_restore_bits(elbi, 0x14, UINT32_MAX, 0);
    if (dbi_ready) {
        if (wifi_restore_bits(pcs, 0xec, 8, 8)) cleanup_failed = -1;
        else {
            int master_failed = -1;
            for (unsigned attempt = 0; attempt < 3; attempt++) {
                if (!wifi_control16(dbi, 0x04, 0x147, 2)) { master_failed = 0; break; }
                delay_for_interval(1, NSEC_PER_MSEC);
            }
            cleanup_failed |= master_failed;
            if (ro_changed) cleanup_failed |= wifi_restore_bits(dbi, 0x8bc, 1, ro_before);
        }
    }
    cleanup_failed |= wifi_restore_bits(phy, 0x15c, 0xff, 0xfe);
    cleanup_failed |= wifi_restore_bits(phy, 0x80, 0xff, 0xc1);
    cleanup_failed |= wifi_restore_bits(pcs, 0x100, 0x40, 0x40);
    cleanup_failed |= wifi_restore_bits(pcs, 0x104, 0x80, 0x80);
    cleanup_failed |= wifi_restore_bits(pcs, 0xec, 8, idle);
    cleanup_failed |= wifi_restore_bits(sysreg, 0x1044, 2, hs);
    cleanup_failed |= wifi_restore_bits(sysreg, 0x1050, 0x10fe, lane);
    cleanup_failed |= wifi_restore_bits(gpio, 0x20, 15, clock_mux);
    cleanup_failed |= wifi_restore_bits(gpio, 0x28, 15, clock_pull);
    for (unsigned i = 0; i < sizeof(saved) / sizeof(saved[0]); i++)
        cleanup_failed |= wifi_restore_bits(elbi, saved[i].off, saved[i].mask, saved[i].value);
    wlog("Gen1 quiesce %s; PHY power-down requested, PCIe IRQs stay masked\n",
        cleanup_failed ? "FAILED" : "verified");
}

static int
wifi_controller_snapshot(const struct wifi_region *elbi)
{
    static const unsigned offsets[] = { 0x02c, 0x074, 0x1d0, 0x288, 0x28c, 0x290 };
    wlog("ELBI snapshot begin: PERST held low, PHY bypass verified; read only\n");
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        uint32_t value = wifi_read(elbi, offsets[i]);
        wlog("ELBI +%#x raw=%#x\n", offsets[i], value);
        if (value == UINT32_MAX) {
            wlog("ELBI snapshot failed: all-ones register; stopping controller reads\n");
            return -1;
        }
    }
    wlog("ELBI snapshot complete; no controller writes, link training or endpoint access\n");
    return 0;
}

static void
wifi_power_probe(struct wifi_region *regions, uint32_t mode)
{
    const uint32_t wlan_function = 15u << 20, wlan_level = 1u << 5;
    const uint32_t reset_function = 15u << 8, reset_level = 1u << 2;
    uint32_t phy = wifi_read(&regions[0], 0x71c);
    uint32_t wc = wifi_read(&regions[1], 0x80), wd = wifi_read(&regions[1], 0x84);
    uint32_t rc = wifi_read(&regions[2], 0x20), rd = wifi_read(&regions[2], 0x24);
    /* This first active trial accepts only the idle state measured in epoch 48.
     * Do not interrupt an already-owned pin, powered PHY or active endpoint. */
    if (phy == UINT32_MAX || wc == UINT32_MAX || wd == UINT32_MAX ||
        rc == UINT32_MAX || rd == UINT32_MAX || (phy & 1u) ||
        (wc & wlan_function) || (wd & wlan_level) ||
        (rc & reset_function) || (rd & reset_level)) {
        wlog("power probe skipped: shared state differs from verified idle baseline\n");
        return;
    }
    /* Exact observed clock words; this probe changes no clock setting. These
     * checks do not authorize accessing ELBI/DBI or training the link. */
    for (unsigned off = 0x2030; off <= 0x2044; off += 4) {
        if (wifi_read(&regions[3], off) != 0x200000) {
            wlog("power probe skipped: gate +%#x differs from baseline\n", off);
            return;
        }
    }
    for (unsigned off = 0x3034; off <= 0x3044; off += 4) {
        if (wifi_read(&regions[3], off) != 0x3f0002) {
            wlog("power probe skipped: QCH +%#x differs from baseline\n", off);
            return;
        }
    }
    wlog("power probe begin: hold PERST low; temporary WLAN power and PHY bypass\n");
    /* Program low latches before selecting output. Modes 2/3 hold reset; mode
     * 4..6 temporarily release/reassert it inside the separate link probe. */
    int failed = wifi_bits(&regions[2], 0x24, reset_level, 0) ||
        wifi_bits(&regions[2], 0x20, reset_function, 1u << 8) ||
        wifi_bits(&regions[1], 0x84, wlan_level, 0) ||
        wifi_bits(&regions[1], 0x80, wlan_function, 1u << 20) ||
        wifi_bits(&regions[1], 0x84, wlan_level, wlan_level);
    if (!failed) {
        delay_for_interval(200, NSEC_PER_MSEC); /* Samsung WIFI_TURNON_DELAY */
        failed = wifi_bits(&regions[0], 0x71c, 1u, 1u);
        if (!failed) {
            delay_for_interval(1, NSEC_PER_MSEC);
            failed = (wifi_read(&regions[2], 0x24) & reset_level) != 0 ||
                (wifi_read(&regions[1], 0x84) & wlan_level) == 0;
        }
    }
    wlog("power probe readback %s\n", failed ? "failed" : "passed");
    if (!failed && mode >= 3 && !wifi_controller_snapshot(&regions[4]) && mode >= 4)
        wifi_gen1_probe(regions, mode);
    /* Re-isolate before powering down. Restore data while pads are still
     * outputs, then restore mux fields. Attempt every restoration on failure. */
    int restore_failed = 0;
    restore_failed |= wifi_restore_bits(&regions[0], 0x71c, 1u, phy);
    restore_failed |= wifi_restore_bits(&regions[1], 0x84, wlan_level, wd);
    restore_failed |= wifi_restore_bits(&regions[2], 0x24, reset_level, rd);
    restore_failed |= wifi_restore_bits(&regions[1], 0x80, wlan_function, wc);
    restore_failed |= wifi_restore_bits(&regions[2], 0x20, reset_function, rc);
    wlog("power probe restore %s; no firmware or networking\n", restore_failed ? "FAILED" : "verified");
}

static void
wifi_diag_run(uint32_t mode)
{
    struct wifi_region regions[] = {
        { 0x15860000, 0, 0x4000 }, /* PMU: PCIe0 PHY isolation */
        { 0x10430000, 0, 0x4000 }, /* PERIC0 GPIO: GPG0-5 WLAN_REG_ON */
        { 0x13c40000, 0, 0x4000 }, /* FSYS1 GPIO: GPF2-0 CLKREQ, GPF2-2 PERST */
        { 0x13c00000, 0, 0x4000 }, /* CMU_FSYS1, shared with booted UFS */
        { 0x13ed0000, 0, 0x4000 }, /* PCIe0 ELBI, modes 3/4/5 */
        { 0x13ef0000, 0, 0x4000 }, /* PCIe0 PHY, modes 4/5 */
        { 0x13ee0000, 0, 0x4000 }, /* PCIe0 PCS */
        { 0x13c10000, 0, 0x4000 }, /* SYSREG_FSYS1, PCIe0 fields only */
        { 0x13f00000, 0, 0x4000 }, /* PCIe0 root DBI */
        { 0x14ffc000, 0, 0x4000 }, /* Modes 5/6: only CFG0 +0x2000 */
        { 0x14400000, 0, 0x4000 }, /* Mode 6: BAR0 first-page chipcommon reads */
    };
    static const unsigned gates[] = { 0x2030, 0x2034, 0x2038, 0x203c, 0x2040, 0x2044 };
    static const unsigned qch[] = { 0x3034, 0x3038, 0x303c, 0x3040, 0x3044 };
    unsigned count = 4;

    if (mode == 3) count = 5;
    if (mode == 4) count = 9;
    if (mode == 5) count = 10;
    if (mode >= 6) count = sizeof(regions) / sizeof(regions[0]);
    /* Map every shared block before reading any of them. Release all mappings
     * even when an allocation fails. Mode 3 also maps ELBI, but cannot read it
     * until the verified power/reset/isolation sequence has completed. */
    for (unsigned i = 0; i < count; i++) {
        regions[i].mapped = ml_io_map_unmappable(regions[i].physical,
            regions[i].size, VM_WIMG_IO);
        if (!regions[i].mapped) {
            wlog("shared-block map failed at %#lx; no registers sampled\n",
                (unsigned long)regions[i].physical);
            goto out;
        }
    }
    wlog("shared-block snapshot begin; no PCIe controller access\n");
    uint32_t phy = wifi_read(&regions[0], 0x71c);
    uint32_t wlan_con = wifi_read(&regions[1], 0x80);
    uint32_t wlan_dat = wifi_read(&regions[1], 0x84);
    uint32_t pcie_con = wifi_read(&regions[2], 0x20);
    uint32_t pcie_dat = wifi_read(&regions[2], 0x24);
    wlog("PMU PHY_CONTROL=%#x; GPG0 CON=%#x DAT=%#x; GPF2 CON=%#x DAT=%#x\n",
        phy, wlan_con, wlan_dat, pcie_con, pcie_dat);
    if (phy == UINT32_MAX || wlan_con == UINT32_MAX || wlan_dat == UINT32_MAX ||
        pcie_con == UINT32_MAX || pcie_dat == UINT32_MAX) {
        wlog("invalid all-ones shared register; pin state undecoded\n");
        goto out;
    }
    wlog("PHY isolation_bypass=%u; WLAN_REG_ON function=%u level=%u; "
        "PERST function=%u level=%u; CLKREQ function=%u level=%u\n",
        phy & 1u, (wlan_con >> 20) & 15u, (wlan_dat >> 5) & 1u,
        (pcie_con >> 8) & 15u, (pcie_dat >> 2) & 1u,
        pcie_con & 15u, pcie_dat & 1u);
    for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
        uint32_t value = wifi_read(&regions[3], gates[i]);
        wlog("CMU gate +%#x raw=%#x\n", gates[i], value);
    }
    for (unsigned i = 0; i < sizeof(qch) / sizeof(qch[0]); i++) {
        uint32_t value = wifi_read(&regions[3], qch[i]);
        wlog("CMU QCH +%#x raw=%#x\n", qch[i], value);
    }
    wlog("snapshot complete; PCI identity, firmware and Wi-Fi remain unprobed\n");
    if (mode >= 2) wifi_power_probe(regions, mode);
out:
    for (unsigned i = count; i > 0; i--) {
        if (regions[i - 1].mapped)
            ml_io_unmap(regions[i - 1].mapped, regions[i - 1].size);
    }
}

/* S10 port, modes 11 and 12: the same sequence later, in its own kernel thread. */
static uint32_t wifi_deferred_mode;

static void
wifi_deferred(void *argument, wait_result_t result)
{
    (void)argument;
    (void)result;
    delay_for_interval(WIFI_DEFER_S, NSEC_PER_SEC);
    wlog("mode %u: deferred run begins\n", wifi_deferred_mode);
    wifi_diag_run(wifi_deferred_mode);
    wlog("mode %u: deferred run finished\n", wifi_deferred_mode);
}

void
b2l_wifi_diag_start(void)
{
    uint32_t mode = 0;
    if (!PE_parse_boot_argn("b2l_wifi_diag", &mode, sizeof(mode)) || mode < 1 || mode > 19) {
        wlog("diagnostic disabled; no MMIO access\n");
        return;
    }
    if (mode >= 10 && wifi_firmware_input(mode)) {
        wlog("firmware input absent/invalid; no MMIO access\n");
        return;
    }
    if (mode >= 11) {
        thread_t thread;
        wifi_deferred_mode = mode;
        if (kernel_thread_start(wifi_deferred, NULL, &thread) != KERN_SUCCESS) {
            wlog("mode %u: thread start failed; no MMIO access\n", mode);
            return;
        }
        thread_deallocate(thread);
        wlog("mode %u: firmware download and boot deferred %u s, after the Metal test window\n", mode,
            WIFI_DEFER_S);
        return;
    }
    wifi_diag_run(mode);
}
