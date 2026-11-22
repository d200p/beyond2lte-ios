#!/usr/bin/env python3
"""Exercise actual Gen1 and read-only endpoint flows with strict MMIO guards."""
import os
import hashlib
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
data_header = (repo / 'xnu-overlay/osfmk/arm64/b2l_wifi_phy_data.h').read_text()
for name, count, digest in [
    ('b2l_wifi_cmn', 48, 'd2133509bb6aa73d82ece56d896a8c7f5867681506fd981f093582bb00d0302c'),
    ('b2l_wifi_trsv', 80, '444696357d4794f501d3996c64427188a806ee78a0b2e0f0de2e25d58742c180'),
]:
    match = re.search(name + r'\[\d+\]\s*=\s*\{(.*?)\};', data_header, re.S)
    assert match
    values = bytes(int(value, 16) for value in re.findall(r'0x[0-9a-fA-F]+', match.group(1)))
    assert len(values) == count and hashlib.sha256(values).hexdigest() == digest
source = (repo / 'xnu-overlay/osfmk/arm64/b2l_wifi_diag.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
rd = 'return *(volatile const uint32_t *)(region->mapped + offset);'
wr = '''    *(volatile uint32_t *)(region->mapped + offset) = value;
    __asm__ volatile ("dsb sy" ::: "memory");'''
assert source.count(rd) == source.count(wr) == 1
source = source.replace(rd, 'return host_read(region->mapped, offset);')
source = source.replace(wr, '    host_write(region->mapped, offset, value);')

pre = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "b2l_wifi_phy_data.h"
#include "wifi_firmware_fixture.h"
typedef uintptr_t vm_offset_t;
typedef unsigned long vm_size_t;
#define VM_WIMG_IO 0x66
#define NSEC_PER_MSEC 1000000u
static _Alignas(16384) uint32_t memory[11][4096];
static const uintptr_t physical[] = {
    0x15860000, 0x10430000, 0x13c40000, 0x13c00000, 0x13ed0000,
    0x13ef0000, 0x13ee0000, 0x13c10000, 0x13f00000, 0x14ffc000, 0x14400000
};
static unsigned maps, unmaps, fail_map, writes, drop_write, link_polls;
static unsigned trained, scenario, gen1_reads;
static unsigned invalid_profile_once;
static unsigned mode = 4, endpoint_reads, endpoint_fault, atu_enable_write, fault_served;
static uint32_t atu[7];
static uint32_t atu0[7];
static unsigned chip_reads, chip_fault, config_writes;
static unsigned erom_reads, erom_fault;
static uint32_t erom[896];
static unsigned wrapper_reads[2], r4_cap_reads, core_fault;
static uint32_t wrapper_values[2][3];
static unsigned bank_reads, bank_writes, bank_index, tcm_fault;
static uint32_t bank_info[13];
static uint32_t firmware_ram[1297036/4];
static unsigned fw_reads, fw_writes, fw_guards, fw_fault, fw_drop_word, fw_windows, fw_drop_window;
static uint32_t original_viewport;
static uint32_t original_endpoint_command, original_bar0;
static uint64_t enabled_at;
static uint64_t now_us;
static char logs[131072];
static unsigned log_size;
static void b2l_early_trace(const char *fmt, ...) {
    va_list args; va_start(args, fmt);
    int n = vsnprintf(logs + log_size, sizeof(logs) - log_size, fmt, args);
    va_end(args); assert(n >= 0 && (unsigned)n < sizeof(logs) - log_size);
    log_size += (unsigned)n;
}
static int PE_parse_boot_argn(const char *name, void *value, unsigned size) {
    assert(!strcmp(name, "b2l_wifi_diag") && size == 4); *(uint32_t *)value = mode; return 1;
}
static uint64_t mach_absolute_time(void) { return now_us; }
static void clock_interval_to_deadline(unsigned n, unsigned unit, uint64_t *at) {
    assert((n == 500 || (mode >= 5 && n == 100) || (mode >= 10 && n == 5000) || (mode >= 11 && n == 2000)) &&
        unit == NSEC_PER_MSEC); *at = now_us + n * 1000;
}
static void delay(unsigned us) { assert(us == 10 || us == 20 || us == 100); now_us += us; }
/* S10 port, mode 11: the deferral, the R4 model and its synthetic firmware. */
#define NSEC_PER_SEC 1000000000u
#define KERN_SUCCESS 0
typedef int kern_return_t;
typedef int wait_result_t;
typedef struct host_thread *thread_t;
typedef void (*thread_continue_t)(void *, wait_result_t);
static unsigned thread_starts, thread_fail, threads_released, deferred_s;
static void delay_for_interval(unsigned n, unsigned unit) {
    if (unit == NSEC_PER_SEC) {
        /* The deferral sleeps before any mapping; it is not part of the probe's time. */
        assert(mode >= 11 && n == 360 && !deferred_s && !maps); deferred_s = n; return;
    }
    assert(unit == NSEC_PER_MSEC && (n == 1 || n == 20 || n == 200 ||
        (mode >= 5 && (n == 9 || n == 100)) || (mode >= 11 && n == 6000))); now_us += n * 1000;
}
static kern_return_t kernel_thread_start(thread_continue_t fn, void *arg, thread_t *out) {
    assert(mode >= 11 && !thread_starts++ && !maps);
    if (thread_fail) return 5;
    *out = (thread_t)&thread_starts;
    fn(arg, 0); /* The thread's whole run, synchronously. */
    return KERN_SUCCESS;
}
static void thread_deallocate(thread_t thread) { assert(thread == (thread_t)&thread_starts); threads_released++; }
void read_frandom(void *p, unsigned n) {
    assert(mode >= 11 && n == 128);
    for (unsigned i = 0; i < n; i++) ((uint8_t *)p)[i] = (uint8_t)(0xa5u ^ i);
}
/* S10 port, mode 13: phone memory as the CPU sees it (host_cpu) and as the
 * chip sees it (host_dram); only the cache calls move bytes between them. */
typedef unsigned ppnum_t;
typedef uint64_t addr64_t;
typedef void *pmap_t;
typedef void *vm_map_t;
#define PAGE_SIZE 16384u
#define PAGE_MASK 16383u
#define PAGE_SHIFT 14
#define KMA_DATA 1u
#define KMA_ZERO 2u
#define KMA_PERMANENT 4u
#define VM_KERN_MEMORY_IOKIT 1
#define ptoa_64(p) ((uint64_t)(p) << PAGE_SHIFT)
#define HOST13_PA 0x9f000000u
#define HOST13_BYTES 0x20000u
static vm_map_t kernel_map;
static pmap_t kernel_pmap;
static _Alignas(16384) uint8_t host_cpu[HOST13_BYTES];
static uint8_t host_dram[HOST13_BYTES], host_dev[HOST13_BYTES];
static unsigned host13_fault, host_allocs, cleans, flushes, bme_ever, hostready13, db0_13, ioctl14_db, iovar15_db;
static uint32_t mailbox13;
static kern_return_t kmem_alloc_contig(vm_map_t map, vm_offset_t *va, vm_size_t size, vm_offset_t mask,
    ppnum_t max, ppnum_t pmask, unsigned flags, int tag) {
    assert(mode >= 13 && !map && size == HOST13_BYTES && mask == PAGE_MASK && max == 0x3ffff && !pmask);
    assert(flags == (KMA_DATA | KMA_ZERO | KMA_PERMANENT) && tag == VM_KERN_MEMORY_IOKIT && !host_allocs++);
    if (host13_fault == 1) return 3;
    memset(host_cpu, 0, sizeof(host_cpu));      /* KMA_ZERO, through the cache */
    memset(host_dram, 0xee, sizeof(host_dram)); /* stale until cleaned */
    *va = (vm_offset_t)host_cpu;
    return KERN_SUCCESS;
}
ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va) {
    uintptr_t base = (uintptr_t)host_cpu;
    assert(!pmap && va >= base && va < base + HOST13_BYTES && !((va - base) % PAGE_SIZE));
    unsigned page = (unsigned)((va - base) / PAGE_SIZE);
    return (HOST13_PA >> PAGE_SHIFT) + page + (host13_fault == 2 && page == 3 ? 7u : 0u);
}
static unsigned host13_offset(vm_offset_t va, unsigned long size) {
    uintptr_t base = (uintptr_t)host_cpu;
    assert(mode >= 13 && host_allocs == 1 && va >= base && size && va + size <= base + HOST13_BYTES);
    return (unsigned)(va - base);
}
static void CleanPoC_DcacheRegion(vm_offset_t va, unsigned long size) {
    unsigned off = host13_offset(va, size);
    memcpy(host_dram + off, host_cpu + off, size);
    cleans++;
}
static void FlushPoC_DcacheRegion(vm_offset_t va, unsigned long size) {
    unsigned off = host13_offset(va, size);
    for (unsigned long i = off; i < off + size; i++) {
        if (!host_dev[i]) assert(host_cpu[i] == host_dram[i]); /* no CPU write left uncleaned */
        host_cpu[i] = host_dram[i];
    }
    flushes++;
}
struct b2l_ram_range { uint64_t base, size; };
static struct b2l_ram_range b2l_owned_ram[1] = { { 0x80000000u, 0x20000000u } };
static unsigned b2l_owned_ram_count = 1;
static int b2l_ram_contains_range(const struct b2l_ram_range *r, unsigned n, uint64_t pa, uint64_t size) {
    assert(r == b2l_owned_ram && n == 1 && size == HOST13_BYTES);
    return host13_fault != 3 && pa >= r->base && pa + size <= r->base + r->size;
}
/* Bus mastering: off on both sides, or (mode 13, host memory in place) on at
 * the root port, with the endpoint on or not yet. */
static int bme_ok(void) {
    uint32_t rc = memory[8][4/4] & 0x145, ep = memory[9][0x2004/4] & 4;
    if (!(rc & 4)) return !rc && !ep;
    return mode >= 13 && host_allocs && rc == 4;
}
static int master13_on(void) {
    return mode >= 13 && (memory[9][0x2004/4] & 7) == 6 && (memory[8][4/4] & 0x145) == 4;
}
#define TCM11_TOP ((0x2ffffcu - 0x170000u) / 4)
static uint32_t tcm11[0x190000 / 4];
static uint32_t vector0, r4_io, r4_reset;
static unsigned r4_running, r4_runs, run_polls, status_stuck, boot_fault, nvram_fixture_bytes;
static const char console_text[] =
    "\r\nRTE (PCIE-MSG_BUF) 17.10.157 (r) on BCM4375 r5 @ 37.4/168.0/1600.0MHz\n"
    "000000.007 pciedev ready\nlast line without newline";
static void tcm11_put(uint32_t address, uint32_t value) {
    assert(address >= 0x170000 && address < 0x300000 && !(address & 3));
    tcm11[(address - 0x170000) / 4] = value;
}
static void start_firmware(void) {
    /* Released only after NVRAM, token and reset vector are in place. */
    uint32_t words = nvram_fixture_bytes / 4;
    assert(tcm11[TCM11_TOP] == ((~words << 16) | words) && vector0 == 0x2e211407); /* fixture bytes 7,20,33,46 */
    tcm11_put(0x2f0000, mode >= 13 && host13_fault != 4 ? 0x30040007u : boot_fault == 2 ? 0x407 : 7);
    tcm11_put(0x2f0004, 0x2f0200);
    tcm11_put(0x2f0014, 0x2f0100);
    tcm11_put(0x2f001c, 0x5a5a1234);
    tcm11_put(0x2f0108, 0x2f1000);
    tcm11_put(0x2f010c, 0x2000);
    tcm11_put(0x2f0110, sizeof(console_text) - 1);
    memcpy((uint8_t *)tcm11 + (0x2f1000 - 0x170000), console_text, sizeof(console_text) - 1);
    for (unsigned i = 0; i < 20; i++) tcm11_put(0x2f0200 + 4 * i, 0x100 + i);
    /* Mode 12: the rest of pciedev_shared_t, ring_info_t, ring_mem_t, indices. */
    tcm11_put(0x2f0020, 0x00400010);        /* lfrag 16, host rx buffers 64 */
    tcm11_put(0x2f0028, 0x2f0500);          /* h2d mailbox data */
    tcm11_put(0x2f002c, 0x2f0504);          /* d2h mailbox data */
    tcm11_put(0x2f0030, 0x2f0400);          /* rings_info */
    tcm11_put(0x2f0050, 0x12);              /* flags2 */
    static const uint32_t info[5] = { 0x2f0600, 0x2f0700, 0x2f0710, 0x2f0720, 0x2f0730 };
    for (unsigned i = 0; i < 5; i++) tcm11_put(0x2f0400 + 4 * i, info[i]);
    tcm11_put(0x2f0434, (42u << 16) | 40u); /* submission queues, tx flow rings */
    tcm11_put(0x2f0438, (8u << 16) | 3u);   /* vdevs, completion rings */
    static const uint32_t rings[5][2] = { { 1, 64 | 40u << 16 }, { 3, 512 | 32u << 16 }, { 1, 64 | 24u << 16 },
                                          { 2, 1024 | 16u << 16 }, { 3, 512 | 32u << 16 } };
    for (unsigned r = 0; r < 5; r++) {
        tcm11_put(0x2f0600 + 16 * r, r | rings[r][0] << 16);
        tcm11_put(0x2f0604 + 16 * r, rings[r][1]);
    }
    tcm11_put(0x2f0720, 1);                 /* the firmware wrote one control completion */
}
/* Mode 13 writes into the running firmware's RAM only what dhd_prot_init
 * writes, all before bus mastering except the control submission write index. */
static uint32_t tcm13(uint32_t address) { return tcm11[(address - 0x170000) / 4]; }
static void tcm13_write(uint32_t address, uint32_t value) {
    int ring_mem = address >= 0x2f0600 && address < 0x2f0650 && (address & 15) >= 4;
    int index = (address >= 0x2f0700 && address < 0x2f0708) || (address >= 0x2f0710 && address < 0x2f0718) ||
        (address >= 0x2f0720 && address < 0x2f072c) || (address >= 0x2f0730 && address < 0x2f073c);
    int shared = address == 0x2f0034 || address == 0x2f0038 || address == 0x2f003c || address == 0x2f0054;
    assert(mode >= 13 && host_allocs == 1 && (ring_mem || index || shared));
    if (address == 0x2f0700 && value)
        assert(master13_on() && hostready13 && ((value == 6 && !db0_13) || (mode >= 14 && value == 7 && db0_13 == 1) ||
            (mode >= 15 && db0_13 >= 2 && value > tcm13(0x2f0700) && value < 64)));
    else if (address == 0x2f0730 && value)     /* the read index given back, before each repost */
        assert(mode >= 15 && master13_on() && db0_13 >= 2 && value == tcm13(0x2f0720));
    else assert(!bme_ever && !hostready13 && (index ? !value : 1));
    tcm11[(address - 0x170000) / 4] = value;
}
/* Mode 14: the ioctl, then (unless fault 7) its acknowledgement, its
 * completion and WLC_IOCTL_MAGIC in the first ioctl-response buffer. */
static void ioctl14(uint32_t value) {
    assert(mode >= 14 && value == 0xff000007 && tcm13(0x2f0700) == 7 && tcm13(0x2f0710) == 6 && !ioctl14_db++);
    db0_13++;
    const uint8_t *item = host_dram + 40 * 6;
    uint32_t request, cmd, low, high;
    memcpy(&request, item + 4, 4); memcpy(&cmd, item + 8, 4); memcpy(&low, item + 24, 4); memcpy(&high, item + 28, 4);
    assert(item[0] == 9 && !item[1] && !item[2] && item[3] == 7 && request == 0xfffe && cmd == 0);
    assert((item[12] | item[13] << 8) == 1 && (item[14] | item[15] << 8) == 4 && (item[16] | item[17] << 8) == 4);
    assert(low == HOST13_PA + 0x11800 && !high);
    for (unsigned b = 18; b < 24; b++) assert(!item[b]);
    for (unsigned b = 32; b < 40; b++) assert(!item[b]);
    for (unsigned b = 0; b < 64; b++) assert(!host_dram[0x11800 + b]);
    if (host13_fault == 7) return;  /* no answer */
    tcm11[(0x2f0710 - 0x170000) / 4] = 7;
    uint32_t ack[6] = { 0x0200000au, 0xfffe, 0x00020000u, 0, 0, 0 };
    uint32_t cpl[6] = { 0x0300000cu, 1, 0x00020000u, 4u | 1u << 16, 0, 0 };
    ack[5] = ack[0] ^ ack[1] ^ ack[2] ^ ack[3] ^ ack[4];
    cpl[5] = cpl[0] ^ cpl[1] ^ cpl[2] ^ cpl[3] ^ cpl[4];
    memcpy(host_dram + 0x5000 + 24, ack, sizeof(ack)); memset(host_dev + 0x5000 + 24, 1, sizeof(ack));
    memcpy(host_dram + 0x5000 + 48, cpl, sizeof(cpl)); memset(host_dev + 0x5000 + 48, 1, sizeof(cpl));
    const uint32_t magic = 0x14e46c77;
    memcpy(host_dram + 0x12000, &magic, 4); memset(host_dev + 0x12000, 1, 4);
    tcm11[(0x2f0720 - 0x170000) / 4] = 3;
    mailbox13 = 0x200;
}
/* Modes 15 to 17: whatever the host posted since the firmware's read index
 * (ioctl-response buffers, then one ioctl), checked and answered as the
 * chip does: an ack, a completion naming the newest posted buffer, the
 * value in it; WLC_UP also brings one event. Faults: 8 "ver" unanswered,
 * 9 the set unanswered, 10 the chip keeps its address, 11 WLC_UP unanswered. */
static uint8_t chip16[6];
static unsigned have16, up17, posted15[16], nposted15;
/* Mode 18: the event buffers the chip holds (newest last), the event mask,
 * and the scan: five results (one network twice), then the final event.
 * "scan_ver" answers major 2 as the chip did (boot 79), so escan version 2.
 * Faults: 12 the escan refused, 15 the final event never comes, 16
 * "scan_ver" unsupported (escan version 1). */
static unsigned ev18[8], nev18, escan18, scan18_next;
static uint8_t mask18[16];
static const struct { uint8_t bssid[6]; uint16_t chanspec; int16_t rssi; } scan18[5] = {
    { { 0x00, 0x11, 0x22, 0xaa, 0xbb, 0x01 }, 0x1001, -40 },
    { { 0x00, 0x11, 0x22, 0xaa, 0xbb, 0x02 }, 0x1006, -67 },
    { { 0x00, 0x11, 0x22, 0xaa, 0xbb, 0x01 }, 0x1001, -42 },
    { { 0x00, 0x11, 0x22, 0xaa, 0xbb, 0x03 }, 0xd024, -71 },
    { { 0x00, 0x11, 0x22, 0xaa, 0xbb, 0x04 }, 0xd095, -80 },
};
static unsigned buffer13(unsigned id) {
    return id <= 2 ? 0x12000 + 0x2000 * (id - 1) : id <= 6 ? 0x16000 + 0x800 * (id - 3) :
        id <= 8 ? 0x18000 + 0x2000 * (id - 7) : id <= 10 ? 0x1e000 + 0x1000 * (id - 9) : 0x1d000 + 0x800 * (id - 11);
}
static void d2h13(const uint32_t words[6]) {
    const unsigned at = tcm13(0x2f0720);
    uint32_t w[6];
    memcpy(w, words, sizeof(w));
    w[0] |= (uint32_t)(at + 1) << 24;   /* the epoch */
    w[5] = w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4];
    memcpy(host_dram + 0x5000 + 24 * at, w, sizeof(w)); memset(host_dev + 0x5000 + 24 * at, 1, sizeof(w));
    tcm11[(0x2f0720 - 0x170000) / 4] = at + 1;
}
static void scan18_deliver(void) {
    while (escan18 && scan18_next <= 5 && nev18) {
        if (scan18_next == 5 && host13_fault == 15) return;
        const unsigned id = ev18[--nev18], last = scan18_next == 5, datalen = last ? 12 : 140;
        uint8_t packet[212] = { 0 };
        memcpy(packet, firmware_chip_mac, 6); memcpy(packet + 6, firmware_chip_mac, 6);
        uint8_t *m = packet + 24;          /* wl_event_msg_t, big-endian */
        m[7] = 69; m[11] = last ? 0 : 8; m[23] = (uint8_t)datalen;
        uint8_t *e = m + 48;               /* wl_escan_result_t, little-endian */
        e[0] = (uint8_t)datalen; e[4] = 1; e[8] = 0x18; e[9] = 0x53;
        if (!last) {
            uint8_t *bi = e + 12;
            e[10] = 1; bi[0] = 109; bi[4] = 128;
            memcpy(bi + 8, scan18[scan18_next].bssid, 6);
            bi[18] = 8; memcpy(bi + 19, "ModelNet", 8);
            bi[72] = (uint8_t)scan18[scan18_next].chanspec; bi[73] = (uint8_t)(scan18[scan18_next].chanspec >> 8);
            bi[78] = (uint8_t)scan18[scan18_next].rssi; bi[79] = (uint8_t)((uint16_t)scan18[scan18_next].rssi >> 8);
        }
        const unsigned length = 72 + datalen;
        memcpy(host_dram + buffer13(id), packet, length); memset(host_dev + buffer13(id), 1, length);
        const uint32_t event[6] = { 0x0eu, id, 0x00020000u, length, 0, 0 };
        d2h13(event);
        scan18_next++;
    }
}
static void iovar15(uint32_t value) {
    const unsigned w = tcm13(0x2f0700), r = tcm13(0x2f0710);
    assert(mode >= 15 && value == (0xff000000u | w) && w > r);
    db0_13++; iovar15_db++;
    const unsigned posts_only = host_dram[40 * (w - 1)] != 9;
    for (unsigned item = r; item + 1 < w + posts_only; item++) {     /* the reposts */
        const uint8_t *b = host_dram + 40 * item;
        uint32_t request, low, high;
        memcpy(&request, b + 4, 4); memcpy(&low, b + 16, 4); memcpy(&high, b + 20, 4);
        if (b[0] == 0x0d) {         /* mode 18: an event buffer given back */
            assert(mode >= 18 && (unsigned)b[3] == (254 + item) % 253 && request >= 3 && request <= 6);
            assert((unsigned)(b[8] | b[9] << 8) == 0x800u && low == HOST13_PA + buffer13(request) && !high);
            assert(tcm13(0x2f0730) == tcm13(0x2f0720) && nev18 < 8);
            for (unsigned k = 0; k < nev18; k++) assert(ev18[k] != request);    /* never posted twice */
            ev18[nev18++] = request;
            continue;
        }
        assert(b[0] == 0x0b && (unsigned)b[3] == item + 1 && request >= 7 && request <= 12);
        assert((unsigned)(b[8] | b[9] << 8) == (request < 9 ? 0x2000u : request < 11 ? 0x1000u : 0x800u));
        assert(low == HOST13_PA + buffer13(request) && !high && tcm13(0x2f0730) == tcm13(0x2f0720));
        assert(nposted15 < 16); posted15[nposted15++] = request;
    }
    if (posts_only) {
        assert(mode >= 18 && escan18);
        tcm11[(0x2f0710 - 0x170000) / 4] = w;
        scan18_deliver();
        return;
    }
    const unsigned item = w - 1;
    const uint8_t *e = host_dram + 40 * item;
    uint32_t request, cmd, low, high;
    memcpy(&request, e + 4, 4); memcpy(&cmd, e + 8, 4); memcpy(&low, e + 24, 4); memcpy(&high, e + 28, 4);
    const unsigned length = e[14] | e[15] << 8;
    assert(e[0] == 9 && (unsigned)e[3] == item + 1 && request == 0xfffe && (unsigned)(e[12] | e[13] << 8) == item - 7);
    assert((unsigned)(e[16] | e[17] << 8) == length && !high && low >= HOST13_PA + 0x1c000 && low < HOST13_PA + 0x1d000);
    const char *input = (const char *)host_dram + (low - HOST13_PA);
    static const char version[32] = "wl0: test version 18.41.117";
    const void *data = NULL;
    unsigned resp = 0, answer = 1, start_scan = 0;
    int status = 0;
    if (cmd == 262 && !strcmp(input, "ver")) {
        assert(length == 256); data = version; resp = 32; answer = host13_fault != 8;
    } else if (cmd == 262 && !strcmp(input, "cur_etheraddr")) {
        assert(length == 32); data = have16 && host13_fault != 10 ? chip16 : firmware_chip_mac; resp = 6;
    } else if (cmd == 263 && !strcmp(input, "cur_etheraddr")) {
        assert(mode >= 16 && length == 20 && !memcmp(input + 14, firmware_chosen_mac, 6));
        memcpy(chip16, input + 14, 6); have16 = 1; answer = host13_fault != 9;
    } else if (cmd == 2) {
        static const uint8_t zero[4];
        assert(mode >= 17 && length == 4 && !memcmp(input, zero, 4)); up17 = 1; answer = host13_fault != 11;
    } else if (cmd == 262 && !strcmp(input, "isup")) {
        assert(mode >= 17 && length == 32 && up17); data = &up17; resp = 4;
    } else if (cmd == 262 && !strcmp(input, "scan_ver")) {
        static const uint8_t version[32] = { 1, 0, 6, 0, 2, 0 };     /* version 1, length 6, major 2 */
        assert(mode >= 18 && length == 32 && up17);
        if (host13_fault == 16) status = -23;   /* BCME_UNSUPPORTED */
        else { data = version; resp = 32; }
    } else if (cmd == 262 && !strcmp(input, "event_msgs")) {
        assert(mode >= 18 && length == 32); data = mask18; resp = 16;
    } else if (cmd == 263 && !strcmp(input, "event_msgs")) {
        assert(mode >= 18 && length == 27 && (uint8_t)input[11 + 8] == 0x20);
        for (unsigned b = 0; b < 16; b++) assert(b == 8 || !input[11 + b]);
        memcpy(mask18, input + 11, 16);
    } else if (cmd == 263 && !strcmp(input, "escan")) {
        const uint8_t *q = (const uint8_t *)input + 6;
        const unsigned v2 = host13_fault != 16, ssid = v2 ? 12 : 8, times = v2 ? 60 : 52;
        uint32_t version, nprobes, active, passive, home, channels;
        memcpy(&version, q, 4); memcpy(&nprobes, q + times, 4); memcpy(&active, q + times + 4, 4);
        memcpy(&passive, q + times + 8, 4); memcpy(&home, q + times + 12, 4); memcpy(&channels, q + times + 16, 4);
        assert(mode >= 18 && length == (v2 ? 86u : 78u) && (mask18[8] & 0x20) && !escan18);
        assert(version == (v2 ? 2u : 1u) && q[4] == 1 && !q[5] && q[6] == 0x18 && q[7] == 0x53);
        if (v2) assert(q[8] == 2 && !q[9] && q[10] == 76 && !q[11]);  /* wl_scan_params_v2_t version, length */
        for (unsigned b = ssid; b < ssid + 36; b++) assert(!q[b]);    /* no SSID */
        for (unsigned b = ssid + 36; b < ssid + 42; b++) assert(q[b] == 0xff);     /* the broadcast BSSID */
        assert(q[ssid + 42] == 2);                                    /* any BSS type */
        if (v2) assert(!q[55] && q[56] == 1 && !q[57] && !q[58] && !q[59]);     /* pad, passive (a word) */
        else assert(q[51] == 1);                                      /* passive (a byte) */
        assert(nprobes == UINT32_MAX && active == UINT32_MAX && passive == 110 && home == UINT32_MAX && !channels);
        if (host13_fault == 12) status = -2; else start_scan = 1;
    } else {
        assert(0);
    }
    if (!answer) return;
    tcm11[(0x2f0710 - 0x170000) / 4] = w;
    assert(nposted15);
    const unsigned id = posted15[--nposted15];  /* the newest posted buffer, as the chip did */
    const uint32_t ack[6] = { 0x0au, 0xfffe, 0x00020000u, 0, 0, 0 };
    const uint32_t cpl[6] = { 0x0cu, id, 0x00020000u | (uint16_t)status, resp | (item - 7) << 16, cmd, 0 };
    d2h13(ack); d2h13(cpl);
    if (resp) {
        memcpy(host_dram + buffer13(id), data, resp); memset(host_dev + buffer13(id), 1, resp);
    }
    if (cmd == 2) {
        /* one event in event buffer id 4: ether header (the chip's address),
         * bcmeth header, then wl_event_msg_t with type 54, status 0 */
        uint8_t packet[48] = { 0 };
        memcpy(packet, firmware_chip_mac, 6); memcpy(packet + 6, firmware_chip_mac, 6);
        packet[24 + 7] = 54;
        assert(nev18);
        const unsigned eid = ev18[--nev18];
        memcpy(host_dram + buffer13(eid), packet, sizeof(packet)); memset(host_dev + buffer13(eid), 1, sizeof(packet));
        const uint32_t event[6] = { 0x0eu, eid, 0x00020000u, 48, 0, 0 };
        d2h13(event);
    }
    if (start_scan) {
        escan18 = 1;
        scan18_deliver();
    }
}
static void pcie13_write(unsigned off, uint32_t value) {
    static const uint32_t rings[5][3] = { { 0x00000, 64, 40 }, { 0x01000, 512, 32 }, { 0x05000, 64, 24 },
                                          { 0x06000, 1024, 24 }, { 0x0c000, 512, 40 } };
    assert(mode >= 13 && master13_on() && r4_running);
    if (off == 0x144) {
        /* dhd_bus_hostready: everything dhd_prot_init writes is in place, and
         * the zeroed rings reached memory. */
        assert(value == 0x12345678 && !hostready13++);
        for (unsigned r = 0; r < 5; r++) {
            assert(tcm13(0x2f0604 + 16 * r) == (rings[r][1] | rings[r][2] << 16));
            assert(tcm13(0x2f0608 + 16 * r) == HOST13_PA + rings[r][0] && !tcm13(0x2f060c + 16 * r));
        }
        assert(tcm13(0x2f0034) == 8 && tcm13(0x2f0038) == HOST13_PA + 0x11000 && !tcm13(0x2f003c));
        assert(tcm13(0x2f0054) == 0x801407 && !tcm13(0x2f0720));
        for (unsigned i = 0; i < HOST13_BYTES; i++) assert(!host_dram[i]);
        return;
    }
    assert(off == 0x140 && hostready13);
    if (db0_13 == 1) { ioctl14(value); return; }
    if (db0_13 >= 2) { iovar15(value); return; }
    assert(value == 0xff000006 && !db0_13++ && tcm13(0x2f0700) == 6);
    for (unsigned i = 0; i < 6; i++) {
        const uint8_t *item = host_dram + 40 * i;   /* what the chip reads */
        uint32_t request, low, high;
        memcpy(&request, item + 4, 4); memcpy(&low, item + 16, 4); memcpy(&high, item + 20, 4);
        assert(item[0] == (i < 2 ? 0x0b : 0x0d) && !item[1] && !item[2] && item[3] == 1 + i && request == 1 + i);
        assert((item[8] | item[9] << 8) == (i < 2 ? 0x2000 : 0x800) && !high &&
            low == HOST13_PA + (i < 2 ? 0x12000 + 0x2000 * i : 0x16000 + 0x800 * (i - 2)));
        for (unsigned b = 10; b < 16; b++) assert(!item[b]);
        for (unsigned b = 24; b < 40; b++) assert(!item[b]);
    }
    if (host13_fault == 5) return;  /* the firmware never takes them */
    tcm11[(0x2f0710 - 0x170000) / 4] = 6;
    /* One control completion with a valid xorcsum, and an event. */
    uint32_t c[6] = { 0x01000002u, 0x77, 0x00020000u, 0x11, 0x22, 0 };
    c[5] = c[0] ^ c[1] ^ c[2] ^ c[3] ^ c[4];
    memcpy(host_dram + 0x5000, c, sizeof(c)); memset(host_dev + 0x5000, 1, sizeof(c));
    static const uint32_t event[4] = { 0xe1, 0xe2, 0xe3, 0xe4 };
    memcpy(host_dram + 0x16000, event, sizeof(event)); memset(host_dev + 0x16000, 1, sizeof(event));
    tcm11[(0x2f0720 - 0x170000) / 4] = 1;
    mailbox13 = 0x100;
}
static uintptr_t ml_io_map_unmappable(uintptr_t pa, unsigned long size, unsigned flags) {
    assert(maps < (mode >= 6 ? 11u : mode + 5) && pa == physical[maps] && size == 0x4000 && flags == VM_WIMG_IO);
    unsigned index = maps++;
    return maps == fail_map ? 0 : (uintptr_t)memory[index];
}
static void ml_io_unmap(uintptr_t va, unsigned long size) {
    unsigned successful = maps - (fail_map && fail_map <= maps ? 1 : 0);
    assert(size == 0x4000 && unmaps < successful);
    assert(va == (uintptr_t)memory[successful - ++unmaps]);
}
static unsigned region_index(uintptr_t address) {
    assert(maps == (mode >= 6 ? 11u : mode + 5) && !fail_map && !unmaps);
    for (unsigned i = 0; i < 11; i++) if (address == (uintptr_t)memory[i]) return i;
    assert(0); return 99; /* No config/BAR/ATU/SysMMU/CMU-other mappings. */
}
static void powered(unsigned index) {
    if (index < 4) return;
    assert(now_us >= 201000);
    assert(memory[0][0x71c/4] & 1);
    assert(((memory[1][0x80/4] >> 20) & 15) == 1 && (memory[1][0x84/4] & 32));
    assert(((memory[2][0x20/4] >> 8) & 15) == 1);
    if (index >= 8) assert(memory[6][0xec/4] & 8); /* DBI/config accesses ignore RX idle. */
}
static int valid_link_setup(void) {
    for (unsigned i = 0; i < sizeof(b2l_wifi_cmn); i++)
        if (memory[5][i] != b2l_wifi_cmn[i]) return 0;
    for (unsigned i = 0; i < sizeof(b2l_wifi_trsv); i++)
        if (memory[5][0x30+i] != b2l_wifi_trsv[i]) return 0;
    return (memory[2][0x24/4] & 4) && (memory[2][0x20/4] & 15) == 2 &&
        (memory[2][0x28/4] & 15) == 3 && (memory[7][0x1044/4] & 2) &&
        (memory[7][0x1050/4] & 0x10fe) == (scenario == 8 ? 0x1004u : 0x100cu) &&
        memory[4][0x1d0/4] == 1 && memory[4][0x288/4] == 1 &&
        memory[4][0x28c/4] == 1 && memory[4][0x290/4] == 1 &&
        !(memory[4][0x0c/4] | memory[4][0x10/4] | memory[4][0x14/4]) &&
        memory[6][0xf8/4] == 0x70 && memory[6][0x100/4] == 0x87 &&
        memory[6][0x104/4] == 0x50 && !(memory[6][0xd0/4] & 0xc0) &&
        !(memory[8][0x04/4] & 0x141) && bme_ok() && (memory[8][0xa0/4] & 15) == 1 &&
        (memory[8][0x710/4] & 0x3f0000) == 0x10000 &&
        (memory[8][0x80c/4] & 0x1f00) == 0x100 && memory[8][0xb40/4] == 26;
}
/* Mode 11 after the TCM inventory: address 0, the R4 wrapper and all of RAM. */
static int boot11_window(uint32_t window) {
    return mode >= 11 && bank_reads == 13 &&
        (window == 0 || window == 0x18102000 || (window >= 0x170000 && window < 0x300000) ||
         (mode >= 13 && window == 0x18003000));
}
static uint32_t host_read(uintptr_t address, unsigned off) {
    unsigned i = region_index(address); assert(!(off & 3) && off < 0x4000); powered(i);
    int allowed = (i == 0 && off == 0x71c) ||
        (i == 1 && (off == 0x80 || off == 0x84)) ||
        (i == 2 && (off == 0x20 || off == 0x24 || off == 0x28)) ||
        (i == 3 && ((off >= 0x2030 && off <= 0x2044) || (off >= 0x3034 && off <= 0x3044))) ||
        (i == 4 && (off == 0x2c || off == 0x74 || off == 0x1d0 || off == 0x288 ||
                    off == 0x28c || off == 0x290 || off == 0xc || off == 0x10 || off == 0x14 ||
                    off == 0x18 || off == 0xf4 || off == 0x1b8 || off == 0x2c8)) ||
        (i == 5 && (off == 0x80 || off == 0x15c)) ||
        (i == 6 && (off == 0xc || off == 0xd0 || off == 0xec || off == 0x100 || off == 0x104)) ||
        (i == 7 && (off == 0x1044 || off == 0x1050)) ||
        (i == 8 && (off == 0 || off == 4 || off == 0x44 || off == 0x7c || off == 0x80 ||
                    off == 0xa0 || off == 0x710 || off == 0x80c || off == 0x8bc || off == 0xb40 ||
                    (mode >= 5 && (off == 0x0c || off == 0x18 || (off >= 0x900 && off <= 0x91c))))) ||
        (mode >= 5 && i == 9 && (off == 0x2000 || off == 0x2004 || off == 0x2008 || off == 0x202c ||
            (mode >= 6 && (off == 0x2010 || off == 0x2014 || off == 0x2080)))) ||
        (mode >= 6 && i == 10 && (off == 0 || off == 4 || off == 0xfc ||
            (mode >= 7 && memory[9][0x2080/4] == 0x18111000 && off < 0xe00) ||
            (mode >= 8 && (memory[9][0x2080/4] == 0x18102000 || memory[9][0x2080/4] == 0x18103000) &&
                (off == 0x408 || off == 0x500 || off == 0x800)) ||
            (mode >= 9 && memory[9][0x2080/4] == 0x18002000 && (off == 0x40 || off == 0x44)) ||
            (mode == 10 && memory[9][0x2080/4] >= 0x170000 &&
             memory[9][0x2080/4] <= 0x2ac000 && off < 4096) ||
            (boot11_window(memory[9][0x2080/4]) && off < 4096)));
    assert(allowed);
    if (i == 8 && (off == 0x0c || off == 0x18 || (off >= 0x900 && off <= 0x91c)))
        assert(mode >= 5 && trained && valid_link_setup());
    if (i >= 5) gen1_reads++;
    if (mode >= 5 && atu_enable_write) {
        if (i == 4 && off == 0x74 &&
            (endpoint_fault == 5 || (endpoint_fault == 6 && endpoint_reads))) return 0;
        if (i == 8 && off == 0x04 && endpoint_fault == 15 && atu[1]) return 6;
        if (i == 8 && off == 0x80 && endpoint_fault == 17) return 0x120000;
        if (mode >= 6 && chip_fault == 9 && i == 4 && off == 0x74 &&
            (memory[9][0x2004/4] & 7) == 2) return 0;
        if (mode >= 7 && erom_fault == 4 && erom_reads && i == 4 && off == 0x74) return 0;
        if (mode >= 8 && core_fault == 7 && wrapper_reads[0] && i == 4 && off == 0x74) return 0;
        if (mode >= 9 && tcm_fault == 5 && bank_reads && i == 4 && off == 0x74) return 0;
        if (mode == 10 && fw_fault == 7 && fw_reads >= 16 && i == 4 && off == 0x74) return 0;
    }
    if (i == 9) {
        assert(!(mode >= 6 && chip_fault == 9 && (memory[9][0x2004/4] & 7) == 2));
        assert(!(mode >= 7 && erom_fault == 4 && erom_reads));
        assert(!(mode >= 8 && core_fault == 7 && wrapper_reads[0]));
        assert(!(mode >= 9 && tcm_fault == 5 && bank_reads));
        assert(!(mode == 10 && fw_fault == 7 && fw_reads >= 16));
        static const unsigned expected[] = { 0x2000, 0x2004, 0x2008, 0x202c };
        if (endpoint_reads < 4) assert(off == expected[endpoint_reads++]);
        else assert(mode >= 6 && (off == 0x2004 || off == 0x2010 || off == 0x2014 || off == 0x2080));
        assert(trained && valid_link_setup() && now_us >= enabled_at + 100000);
        assert(memory[8][0x900/4] == 1 && (memory[8][0x18/4] & 0xffffff) == 0x10100);
        assert(!(memory[8][4/4] & 0x141) && bme_ok());
        assert(atu[0] == 4 && atu[1] == 0x80000000 && atu[2] == 0x14ffe000 &&
            atu[3] == 0 && atu[4] == 0x14ffefff && atu[5] == 0x01000000 && atu[6] == 0);
        if (endpoint_fault == 11) now_us += 110000;
        if (off == 0x2000 && endpoint_fault == 1) return 0x123414e4;
        if (off == 0x2000 && endpoint_fault == 2) return UINT32_MAX;
        if (off == 0x2000 && endpoint_fault == 14) return 0;
        if (off == 0x2004 && endpoint_fault == 3) return 4;
        if (off == 0x202c && endpoint_fault == 4) return 0x000014e4;
        if (off == 0x2008 && endpoint_fault == 13) return UINT32_MAX;
        return memory[i][off/4];
    }
    if (i == 10) {
        uint32_t window = memory[9][0x2080/4];
        if (boot11_window(window)) {
            assert(off < 4096 && (memory[9][0x2004/4] & 3) == 2 && atu0[1] == 0x80000000);
            assert(memory[8][0x900/4] == 1 && !(memory[8][4/4] & 0x141) && bme_ok());
            if (window == 0) { assert(off == 0); return boot_fault == 4 ? UINT32_MAX : vector0; }
            if (window == 0x18003000) { assert(off == 0xc30 && master13_on()); return mailbox13; }
            if (window == 0x18102000) {
                assert(off == 0x408 || off == 0x800 || off == 0x804);
                if (off == 0x804) {
                    if (status_stuck) { status_stuck--; return 1; }
                    return 0;
                }
                return off == 0x408 ? r4_io : r4_reset;
            }
            unsigned index = (window - 0x170000 + off) / 4;
            /* The synthetic firmware publishes its shared area on the third poll. */
            if (r4_running && index == TCM11_TOP && ++run_polls == 3 && boot_fault != 1)
                tcm11[index] = 0x2f0000;
            return tcm11[index];
        }
        if (mode == 10 && window >= 0x170000 && window <= 0x2ac000) {
            unsigned index = (window - 0x170000 + off)/4;
            assert(off < 4096 && index < sizeof(firmware_ram)/4 && index == fw_reads);
            assert(bank_reads == 13 && bank_index == 7 && fw_guards >= 2);
            assert((memory[9][0x2004/4] & 7) == 2 && atu0[1] == 0x80000000);
            fw_reads++;
            return fw_fault == 5 && fw_reads == 5 ? firmware_ram[index] ^ 1 : firmware_ram[index];
        }
        if (mode == 10 && window == 0x18102000 && bank_reads == 13) {
            static const unsigned expected[] = {0x408,0x800};
            assert(off == expected[fw_guards % 2] && wrapper_reads[1] == 3);
            fw_guards++;
            if (off == 0x408 && fw_fault == 4) return UINT32_MAX;
            if (fw_reads >= 1024) {
                if (off == 0x408 && fw_fault == 1) return 0x20;
                if (off == 0x408 && fw_fault == 2) return 1;
                if (off == 0x800 && fw_fault == 3) return 1;
            }
            return wrapper_values[1][off == 0x408 ? 0 : 2];
        }
        if (mode >= 8 && (memory[9][0x2080/4] == 0x18102000 || memory[9][0x2080/4] == 0x18103000)) {
            unsigned core = memory[9][0x2080/4] == 0x18103000 ? 0 : 1;
            static const unsigned expected[] = { 0x408, 0x500, 0x800 };
            assert(chip_reads == 3 && erom_reads == 133 && wrapper_reads[core] < 3);
            assert(off == expected[wrapper_reads[core]] && (memory[9][0x2004/4] & 7) == 2);
            assert(memory[8][0x900/4] == 1 && atu[1] == 0x80000000 && atu0[1] == 0x80000000);
            if (core == 1) assert(wrapper_reads[0] == 3);
            uint32_t value = wrapper_values[core][wrapper_reads[core]++];
            if (core_fault == 6) now_us += 110000;
            return value;
        }
        if (mode >= 9 && memory[9][0x2080/4] == 0x18002000 && (off == 0x40 || off == 0x44)) {
            assert(r4_cap_reads == 1 && (wrapper_values[1][0] & 0x23) == 0x21 && !(wrapper_values[1][2] & 1));
            assert((memory[9][0x2004/4] & 7) == 2 && atu0[1] == 0x80000000);
            if (off == 0x40) {
                if (tcm_fault == 1 && !bank_writes) return UINT32_MAX;
                if (tcm_fault == 8 && !bank_writes) return 13;
                if (tcm_fault == 7 && bank_writes == 1) return bank_index ^ 1;
                return bank_index;
            }
            assert(bank_reads < 13 && bank_index == bank_reads && bank_writes);
            uint32_t info = bank_info[bank_reads++];
            if (tcm_fault == 4) now_us += 110000;
            return (tcm_fault == 2 && bank_reads == 1) || (tcm_fault == 3 && bank_reads == 6) ? UINT32_MAX : info;
        }
        if (mode >= 8 && memory[9][0x2080/4] == 0x18002000) {
            assert(off == 4 && wrapper_reads[0] == 3 && wrapper_reads[1] == 3 && !r4_cap_reads++);
            assert((wrapper_values[1][0] & 3) == 1 && !(wrapper_values[1][2] & 1));
            assert((memory[9][0x2004/4] & 7) == 2 && atu0[1] == 0x80000000);
            return core_fault == 5 ? UINT32_MAX : tcm_fault == 9 ? 0x351584 : 0x351585;
        }
        if (mode >= 7 && memory[9][0x2080/4] == 0x18111000) {
            assert(chip_reads == 3 && off == erom_reads * 4 && off < 0xe00 && erom_reads < 896);
            assert((memory[9][0x2004/4] & 7) == 2 && memory[9][0x2010/4] == 0x14400004);
            assert(!memory[9][0x2014/4] && memory[8][0x900/4] == 1);
            assert(atu[1] == 0x80000000 && atu0[1] == 0x80000000 && atu0[0] == 0 &&
                atu0[2] == 0x14400000 && atu0[4] == 0x14400fff && atu0[5] == 0x14400000);
            if (erom_fault == 3) now_us += 110000;
            return erom[erom_reads++];
        }
        static const unsigned expected[] = { 0, 4, 0xfc };
        assert(mode >= 6 && chip_reads < 3 && off == expected[chip_reads++]);
        assert(endpoint_reads == 4 && memory[8][0x900/4] == 1);
        assert((memory[9][0x2004/4] & 7) == 2 && memory[9][0x2010/4] == 0x14400004);
        assert(!memory[9][0x2014/4] && memory[9][0x2080/4] == 0x18000000);
        assert(atu[1] == 0x80000000 && atu0[0] == 0 && atu0[1] == 0x80000000 &&
            atu0[2] == 0x14400000 && !atu0[3] && atu0[4] == 0x14400fff &&
            atu0[5] == 0x14400000 && !atu0[6]);
        if (chip_fault == 1 && !off) return UINT32_MAX;
        if (chip_fault == 2 && !off) return 0x14834376;
        if (chip_fault == 3 && off == 4) return UINT32_MAX;
        if (chip_fault == 4 && off == 0xfc) return UINT32_MAX;
        if (chip_fault == 5) now_us += 110000;
        return memory[i][off/4];
    }
    if (i == 4 && off == 0x74 && (memory[4][0x2c/4] & 1)) {
        if (!trained) link_polls++;
        if (scenario == 4) now_us += 300000; /* Preemption must not extend polling indefinitely. */
        if (scenario != 1 && scenario != 4 && link_polls >= 3 && valid_link_setup()) {
            trained = 1;
            return 0xf15411;
        }
    }
    if (i == 8 && off == 0x80 && trained && (memory[4][0x2c/4] & 1)) {
        if (scenario == 3) return UINT32_MAX;
        return (memory[8][off/4] & 0xffff) | ((scenario == 2 ? 2u : 1u) << 16) | (1u << 20);
    }
    if (i == 8 && off == 0 && scenario == 5) return UINT32_MAX;
    if (i == 8 && mode >= 5 && off >= 0x904 && off <= 0x91c) {
        if (memory[8][0x900/4] == 0) {
            assert(mode >= 6);
            return atu0[(off - 0x904)/4];
        }
        assert(memory[8][0x900/4] == 1); /* No inbound/other regions. */
        if (endpoint_fault == 12 && off == 0x908 && atu_enable_write && !fault_served++) return UINT32_MAX;
        if (endpoint_fault == 19 && off == 0x914) return UINT32_MAX;
        return atu[(off - 0x904)/4];
    }
    if (i == 7 && off == 0x1050 && invalid_profile_once) {
        invalid_profile_once = 0;
        return UINT32_MAX;
    }
    return memory[i][off/4];
}
static void host_write(uintptr_t address, unsigned off, uint32_t value) {
    unsigned i = region_index(address); assert(!(off & 3) && off < 0x4000); powered(i);
    uint32_t before = memory[i][off/4], mask = 0;
    if (i == 10) {
        uint32_t window = memory[9][0x2080/4];
        if (boot11_window(window)) {
            assert(off < 4096 && (memory[9][0x2004/4] & 3) == 2 && atu0[1] == 0x80000000);
            assert(memory[8][0x900/4] == 1 && !(memory[8][4/4] & 0x141) && bme_ok());
            writes++;
            if (window == 0x18003000) { pcie13_write(off, value); return; }
            if (window == 0) {
                assert(off == 0 && !r4_running && !r4_runs && boot_fault != 4);
                if (boot_fault == 3) return; /* Lost write: the readback must catch it. */
                vector0 = value;
                if (boot_fault == 5) status_stuck = 30; /* Release waits fail; the halt works. */
                return;
            }
            if (window == 0x18102000) {
                assert(off == 0x408 || off == 0x800);
                if (off == 0x800) { assert(value <= 1); r4_reset = value; }
                else { assert(value == 3 || value == 1 || value == 0x23 || value == 0x21); r4_io = value; }
                unsigned running = !r4_reset && (r4_io & 0x23) == 1;
                if (running && !r4_running) { r4_runs++; start_firmware(); }
                r4_running = running;
                return;
            }
            if (mode >= 13 && r4_running) { tcm13_write(window + off, value); return; }
            /* RAM is written only while the R4 is halted and before it has ever run. */
            assert(!r4_running && !r4_runs && !(r4_io & 2) && (r4_io & 0x20));
            tcm11[(window - 0x170000 + off) / 4] = value;
            return;
        }
        if (mode == 10 && window >= 0x170000 && window <= 0x2ac000) {
            unsigned index = (window - 0x170000 + off)/4;
            assert(off < 4096 && index < sizeof(firmware_ram)/4 && index == fw_writes);
            assert(bank_reads == 13 && bank_index == 7 && fw_guards >= 2);
            assert((memory[9][0x2004/4] & 7) == 2 && atu0[1] == 0x80000000);
            assert(!memcmp(firmware_fixture + index*4, &value, 4));
            writes++; fw_writes++;
            if (fw_fault == 6 && fw_writes == 1) now_us += 6000000;
            if (fw_writes != fw_drop_word) firmware_ram[index] = value;
            return;
        }
        assert(mode >= 9 && off == 0x40 && value < 13);
        assert(memory[9][0x2080/4] == 0x18002000 && (memory[9][0x2004/4] & 7) == 2);
        assert(r4_cap_reads == 1 && (wrapper_values[1][0] & 0x23) == 0x21 && !(wrapper_values[1][2] & 1));
        assert(atu[1] == 0x80000000 && atu0[1] == 0x80000000 && memory[8][0x900/4] == 1);
        assert(!(tcm_fault == 5 && bank_reads));
        writes++; bank_writes++;
        if (writes == drop_write || (tcm_fault == 6 && bank_reads == 13 && value == 7)) return;
        bank_index = value;
        if (tcm_fault == 11 && bank_writes == 1) now_us += 110000;
        return;
    }
    if (i == 9) {
        assert(mode >= 6 && endpoint_reads == 4 && memory[8][0x900/4] == 1 && atu[1] == 0x80000000);
        assert(off == 0x2004 || off == 0x2010 || off == 0x2014 || off == 0x2080);
        if (off == 0x2004) {
            assert(!(value >> 16) && ((value & 3) == 0 || (value & 3) == 2));
            /* S10 port, mode 13: the endpoint masters only after the root port forwards. */
            assert(!(value & 4) || (mode >= 13 && host_allocs && (memory[8][4/4] & 4)));
            assert(!((before ^ value) & 0xfff8)); /* Preserve unrelated command bits. */
            if (value & 4) bme_ever = 1;
            if (host13_fault == 6 && (value & 4)) value &= ~4u; /* it does not stick */
        }
        else assert(!(memory[9][0x2004/4] & 3)); /* Never move a live BAR. */
        if (off == 0x2010) assert(value == 4 || value == 0x14400004);
        if (off == 0x2014) assert(value == 0);
        if (off == 0x2080) assert(value == 0x18000000 || value == 0x18001000 || (mode >= 7 && value == 0x18111000) ||
            (mode >= 8 && (value == 0x18103000 || value == 0x18102000 || value == 0x18002000)) ||
            (mode == 10 && value >= 0x170000 && value <= 0x2ac000 && !(value & 4095)) ||
            (mode >= 11 && (value == 0 || (value >= 0x170000 && value < 0x300000 && !(value & 4095)))) ||
            (mode >= 13 && value == 0x18003000));
        writes++; config_writes++;
        if (mode == 10 && bank_reads == 13 && off == 0x2080 &&
            (value == 0x18102000 || (value >= 0x170000 && value <= 0x2ac000))) {
            fw_windows++;
            if (fw_windows == fw_drop_window) return;
        }
        if (mode == 10 && fw_fault == 8 && fw_reads && off == 0x2080 && value == 0x18001000) return;
        if (mode == 10 && fw_fault == 9 && fw_reads == sizeof(firmware_ram)/4 && off == 0x2004 && !value) return;
        if (writes == drop_write) return;
        if (chip_fault == 6 && off == 0x2004 && !value && (before & 7) == 2) return;
        if (chip_fault == 7 && off == 0x2080 && value == 0x18001000) return;
        if (mode >= 7 && erom_fault == 10 && erom_reads && off == 0x2080 && value == 0x18001000) return;
        if (mode >= 7 && erom_fault == 12 && erom_reads && off == 0x2004 && !value) return;
        if (mode >= 8 && core_fault == 8 && r4_cap_reads && off == 0x2080 && value == 0x18001000) return;
        if (mode >= 8 && core_fault == 9 && r4_cap_reads && off == 0x2004 && !value) return;
        memory[i][off/4] = off == 0x2004 ? (before & 0xffff0000) | value : value;
        return;
    }
    if (i == 0 && off == 0x71c) mask = 1;
    if (i == 1 && off == 0x80) mask = 15u << 20;
    if (i == 1 && off == 0x84) mask = 32;
    if (i == 2 && off == 0x20) mask = 15u | (15u << 8);
    if (i == 2 && off == 0x24) mask = 4;
    if (i == 2 && off == 0x28) mask = 15;
    if (i == 4 && (off == 0xc || off == 0x10 || off == 0x14)) { mask = UINT32_MAX; assert(!value); }
    if (i == 4 && off == 0x18) mask = 2;
    if (i == 4 && off == 0xf4) mask = 0x11;
    if (i == 4 && off == 0x1b8) mask = 3;
    if (i == 4 && off == 0x2c8) mask = 0xfff;
    if (i == 4 && (off == 0x2c || off == 0x1d0 || off == 0x288 || off == 0x28c || off == 0x290)) mask = 1;
    if (i == 5 && off <= 0x1fc) { mask = 255; assert(value <= 255); }
    if (i == 6 && off == 0xec) mask = 8;
    if (i == 6 && (off == 0x100 || off == 0x104 || off == 0xf8)) mask = 255;
    if (i == 6 && off == 0xc) mask = 0x12;
    if (i == 6 && off == 0xd0) mask = 0xc0;
    if (i == 7 && off == 0x1044) mask = 2;
    if (i == 7 && off == 0x1050) mask = 0x10fe;
    if (i == 8) {
        if (mode >= 5 && off >= 0x904 && off <= 0x91c) {
            assert(trained && !(memory[8][4/4] & 0x145));
            if (memory[8][0x900/4] == 0) {
                assert(mode >= 6);
                static const uint32_t allowed[] = { 0, 0x80000000, 0x14400000, 0, 0x14400fff, 0x14400000, 0 };
                unsigned slot = (off - 0x904)/4;
                assert(value == allowed[slot] || (off == 0x908 && value == 0));
                writes++;
                if (chip_fault == 8 && off == 0x908 && !value && atu0[1]) return;
                if (chip_fault == 14 && off == 0x908 && value) return;
                if (writes != drop_write) atu0[slot] = value;
                return;
            }
            assert(memory[8][0x900/4] == 1);
            static const uint32_t allowed[] = { 4, 0x80000000, 0x14ffe000, 0, 0x14ffefff, 0x01000000, 0 };
            unsigned slot = (off - 0x904)/4;
            assert(value == allowed[slot] || (off == 0x908 && value == 0));
            writes++;
            if (off == 0x908 && value == 0x80000000) {
                atu_enable_write = writes;
                enabled_at = now_us;
            }
            if (writes == drop_write || (endpoint_fault == 9 && off == 0x908 && value) ||
                (endpoint_fault == 10 && off == 0x908 && !value && atu[1])) return;
            atu[slot] = value;
            return;
        }
        if (mode >= 5 && off == 0x18) mask = 0xffffff;
        if (mode >= 5 && off == 0x900) { mask = UINT32_MAX; assert(value == original_viewport || value == 1 || (mode >= 6 && value == 0)); }
        if (off == 4 || off == 0x44 || off == 0x80 || off == 0xa0) {
            assert(!(value >> 16));
            if (off == 4) {
                assert(!(value & 0x141));
                /* S10 port, mode 13: the root port forwards only with the host memory in
                 * place and the endpoint memory-only; it stops only after the endpoint. */
                if (value & 4) { assert(mode >= 13 && host_allocs && (memory[9][0x2004/4] & 7) == 2); bme_ever = 1; }
                else assert(!(memory[9][0x2004/4] & 4));
            }
            if (off == 0x44) assert(!(value & 0x8000));
            writes++;
            if (writes != drop_write) {
                memory[i][off/4] = (before & 0xffff0000) | value;
                if (off == 0x44) memory[i][off/4] |= before & 0x8000; /* W1C zero preserves PME_Status. */
            }
            return;
        }
        if (off == 0x710) mask = 0x3f0000;
        if (off == 0x80c) mask = 0x21f00;
        if (off == 0x7c) { mask = 0x383ff; assert(memory[8][0x8bc/4] & 1); }
        if (off == 0x8bc) mask = 1;
        if (off == 0xb40) { mask = UINT32_MAX; assert(value == 26); }
    }
    assert(mask && !((before ^ value) & ~mask));
    writes++;
    if (scenario == 6 && i == 4 && off == 0x2c && before == 1 && value == 0) return;
    if (scenario == 7 && i == 5 && off == 0x80 && value == 0xc1) return;
    if (writes != drop_write) {
        memory[i][off/4] = value;
        if (i == 7 && off == 0x1050 && value == 0x1d0c) {
            if (scenario == 8 || scenario == 9) memory[i][off/4] &= ~8u;
            if (scenario == 10) memory[i][off/4] &= ~4u;
            if (scenario == 11) invalid_profile_once = 1;
        }
    }
}
'''

post = r'''
static void reset(unsigned which, unsigned failure) {
    maps = unmaps = writes = drop_write = link_polls = 0;
    trained = gen1_reads = log_size = invalid_profile_once = 0;
    endpoint_reads = endpoint_fault = atu_enable_write = fault_served = 0;
    enabled_at = 0; memset(atu, 0, sizeof(atu));
    memset(atu0, 0, sizeof(atu0)); chip_reads = chip_fault = config_writes = 0;
    erom_reads = erom_fault = 0; memset(erom, 0, sizeof(erom));
    wrapper_reads[0] = wrapper_reads[1] = r4_cap_reads = core_fault = 0;
    bank_reads = bank_writes = tcm_fault = 0; bank_index = 7;
    fw_reads = fw_writes = fw_guards = fw_fault = fw_drop_word = fw_windows = fw_drop_window = 0;
    if (mode >= 10) { firmware_fixture_setup(); memset(firmware_ram, 0, sizeof(firmware_ram)); }
    for (unsigned b = 0; b < 13; b++) bank_info[b] = mode >= 10 ? (b == 4 ? 0x88c07 : 0x108c0f) : b < 5 ? 0x203 : 0xf;
    thread_starts = thread_fail = threads_released = deferred_s = 0;
    host13_fault = host_allocs = cleans = flushes = bme_ever = hostready13 = db0_13 = ioctl14_db = iovar15_db = 0; mailbox13 = 0;
    firmware_mac_mode = 0; memset(chip16, 0, sizeof(chip16)); have16 = up17 = 0;
    nposted15 = 1; posted15[0] = 2;     /* the first ioctl's answer takes buffer 1 */
    nev18 = 4; ev18[0] = 3; ev18[1] = 4; ev18[2] = 5; ev18[3] = 6; escan18 = scan18_next = 0; memset(mask18, 0, 16);
    memset(host_cpu, 0, sizeof(host_cpu)); memset(host_dram, 0, sizeof(host_dram)); memset(host_dev, 0, sizeof(host_dev));
    wifi_host = NULL; wifi_host_pa = 0; wifi_master = 0;
    memset(tcm11, 0, sizeof(tcm11)); vector0 = 0; r4_io = 0x21; r4_reset = 0;
    r4_running = r4_runs = run_polls = status_stuck = boot_fault = nvram_fixture_bytes = 0;
    memset(wrapper_values, 0, sizeof(wrapper_values)); wrapper_values[0][0] = 1; wrapper_values[1][0] = 0x21;
    erom[0] = 0x4bf80001; erom[1] = 0x39000201; erom[2] = 0x18000005;
    erom[3] = 0x4bf83c01; erom[4] = 0x42000201; erom[5] = 0x18001005; erom[6] = 0xf;
    original_viewport = 0;
    now_us = 0; fail_map = failure; scenario = which;
    memset(memory, 0, sizeof(memory)); memset(logs, 0, sizeof(logs));
    memory[1][0x84/4] = 1;
    for (unsigned off = 0x2030; off <= 0x2044; off += 4) memory[3][off/4] = 0x200000;
    for (unsigned off = 0x3034; off <= 0x3044; off += 4) memory[3][off/4] = 0x3f0002;
    memory[4][0x74/4] = 0xf15400;
    memory[4][0x1d0/4] = memory[4][0x288/4] = memory[4][0x28c/4] = memory[4][0x290/4] = 1;
    memory[4][0xc/4] = memory[4][0x10/4] = memory[4][0x14/4] = 0x55;
    memory[4][0x18/4] = 0x12; memory[4][0xf4/4] = 0x20;
    memory[4][0x1b8/4] = 0x80; memory[4][0x2c8/4] = 0xaaaa0555;
    memory[7][0x1044/4] = 0x8040; memory[7][0x1050/4] = 0x80000101;
    if (which >= 8 && which <= 11) memory[7][0x1050/4] = which == 9 ? 0x1d82 : 0x1d02;
    memory[8][0] = 0x123416c3;
    memory[8][4/4] = 0xdead0144; /* BME, parity and SERR must be cleared before training. */
    memory[8][0x44/4] = 0xbeef8003; /* PME_Status must not be acknowledged by copying it. */
    memory[8][0x18/4] = 0xa5000000; /* Preserve the bridge latency byte. */
    memory[8][0x0c/4] = 0x00010000; /* Type-1 bridge, not endpoint BAR2 at +0x18. */
    memory[9][0x2000/4] = 0x447514e4;
    memory[9][0x2004/4] = 0x00100000;
    original_endpoint_command = 0x00100000; original_bar0 = 4;
    memory[9][0x2008/4] = 0x02800005;
    memory[9][0x202c/4] = 0x437514e4;
    memory[9][0x2010/4] = 4; memory[9][0x2080/4] = 0x18001000;
    memory[10][0] = 0x14834375; memory[10][1] = 0x48000000;
    memory[10][0xfc/4] = 0x18100000;
    if (mode >= 7) {
        memory[10][0] = 0x10054375; memory[10][1] = 0x58400009;
        memory[10][0xfc/4] = 0x18111000;
    }
    if (mode >= 8) {
        /* Only the fixed target witnesses are relevant to this MMIO fixture;
         * full table semantics are independently checked by test_wifi_erom.py. */
        memset(erom, 0, sizeof(erom));
        erom[0x44/4] = 0x4bf83e01; erom[0x48/4] = 0x0f084411; erom[0x50/4] = 0x18002005;
        erom[0x68/4] = 0x181020c5; erom[0x70/4] = 0x4bf83c01; erom[0x74/4] = 0x40084411;
        erom[0x7c/4] = 0x18003005; erom[0xa0/4] = 0x181030c5; erom[132] = 0xf;
    }
}
static void verify_quiet(void) {
    assert(!(memory[0][0x71c/4] & 1));
    assert(memory[1][0x80/4] == 0 && memory[1][0x84/4] == 1);
    assert(!memory[2][0x20/4] && !memory[2][0x24/4] && !memory[2][0x28/4]);
    assert(!(memory[4][0x2c/4] & 1));
    uint32_t original_lane = scenario >= 8 && scenario <= 11 ?
        (scenario == 9 ? 0x1d82 : 0x1d02) : 0x80000101;
    assert(memory[7][0x1044/4] == 0x8040 && memory[7][0x1050/4] == original_lane);
    assert(memory[4][0x18/4] == 0x12 && memory[4][0xf4/4] == 0x20);
    assert(memory[4][0x1b8/4] == 0x80 && memory[4][0x2c8/4] == 0xaaaa0555);
    assert(strstr(logs, "power probe restore verified"));
    if (strstr(logs, "Gen1 probe begin:"))
        assert(!(memory[4][0xc/4] | memory[4][0x10/4] | memory[4][0x14/4]));
    assert(unmaps == (mode >= 6 ? 11u : mode + 5) && link_polls <= 500 &&
        now_us < (mode >= 18 ? 60000000u : mode >= 16 ? 40000000u : mode >= 11 ? 20000000u : mode == 10 ? 7000000u :
                  1000000u));
    if (mode >= 11) assert(!r4_running && !r4_reset && r4_io == 0x21); /* Halted before power-off. */
    if (mode >= 5) {
        uint32_t expected_control = endpoint_fault == 7 || endpoint_fault == 10 ? 0x80000000 : 0;
        if (endpoint_fault == 16) expected_control = UINT32_MAX;
        assert(atu[1] == expected_control && memory[8][0x900/4] == (endpoint_fault == 8 ? UINT32_MAX : original_viewport));
        assert(memory[8][0x18/4] == 0xa5000000);
    }
    if (mode >= 6) {
        assert(atu0[1] == (chip_fault == 8 || chip_fault == 10 ? 0x80000000u : 0));
        int erom_dirty_command = mode >= 7 && (erom_fault == 4 || erom_fault == 12);
        if (mode >= 8 && (core_fault == 7 || core_fault == 9)) erom_dirty_command = 1;
        if (mode >= 9 && tcm_fault == 5) erom_dirty_command = 1;
        if (mode == 10 && (fw_fault == 7 || fw_fault == 9)) erom_dirty_command = 1;
        assert(memory[9][0x2004/4] == (original_endpoint_command | (chip_fault == 6 || chip_fault == 9 || erom_dirty_command ? 2u : 0)));
        uint32_t expected_bar = chip_fault == 6 || chip_fault == 9 || erom_dirty_command ? 0x14400004u : chip_fault == 11 ? 0 : original_bar0;
        uint32_t expected_window = chip_fault == 6 || chip_fault == 7 || chip_fault == 9 ?
            0x18000000u : chip_fault == 13 ? UINT32_MAX : 0x18001000u;
        if (mode >= 7 && (erom_fault == 4 || erom_fault == 10 || erom_fault == 12)) expected_window = 0x18111000;
        if (mode >= 8 && core_fault == 7) expected_window = 0x18103000;
        if (mode >= 8 && (core_fault == 8 || core_fault == 9)) expected_window = 0x18002000;
        if (mode >= 9 && tcm_fault == 5) expected_window = 0x18002000;
        if (mode == 10 && fw_fault == 7) expected_window = 0x170000;
        if (mode == 10 && fw_fault == 8) expected_window = 0x18102000;
        if (mode == 10 && fw_fault == 9) expected_window = 0x2ac000;
        assert(memory[9][0x2010/4] == expected_bar && memory[9][0x2014/4] == (chip_fault == 12 ? 1u : 0));
        assert(memory[9][0x2080/4] == expected_window);
    }
}
int main(void) {
    reset(0, 0); b2l_wifi_diag_start(); verify_quiet();
    assert(strstr(logs, "Gen1 link up:") && strstr(logs, "Gen1 quiesce verified"));
    assert(link_polls == 3 && !(memory[8][4/4] & 4));
    unsigned normal_writes = writes;
    for (unsigned which = 1; which <= 5; which++) {
        reset(which, 0); b2l_wifi_diag_start(); verify_quiet();
        assert(!strstr(logs, "Gen1 link up:"));
        assert(strstr(logs, "Gen1 quiesce verified"));
        if (which == 1) assert(link_polls == 500);
        if (which == 4) assert(link_polls == 2); /* Absolute 500 ms deadline. */
    }
    reset(0, 0); memory[8][0] = 0; b2l_wifi_diag_start(); verify_quiet();
    assert(strstr(logs, "Gen1 link up:")); /* Root identity may not be programmed yet. */
    for (unsigned failure = 1; failure <= 9; failure++) {
        reset(0, failure); b2l_wifi_diag_start();
        assert(!writes && !gen1_reads && maps == failure && unmaps == failure - 1);
    }
    reset(0, 0); memory[4][0x1d0/4] = 0; b2l_wifi_diag_start();
    assert(!gen1_reads && !strstr(logs, "Gen1 probe begin:") && strstr(logs, "Gen1 probe skipped:"));
    verify_quiet();
    reset(0, 0); memory[2][0x20/4] = 2; b2l_wifi_diag_start();
    assert(!gen1_reads && strstr(logs, "Gen1 probe skipped: CLKREQ"));
    assert(memory[2][0x20/4] == 2 && !(memory[0][0x71c/4] & 1));
    assert(!(memory[1][0x84/4] & 32) && unmaps == 9);
    /* A lost write anywhere, including cleanup, must not leave power/reset or
     * root bus mastering enabled. Training requires all actual PHY values. */
    unsigned bits_diagnostic = 0, reset_diagnostic = 0, pci_diagnostic = 0;
    for (unsigned failure = 1; failure <= normal_writes; failure++) {
        reset(0, 0); drop_write = failure; b2l_wifi_diag_start(); verify_quiet();
        if (strstr(logs, "Gen1 root DBI ID=")) assert(!(memory[8][4/4] & 4));
        if (strstr(logs, "bits mismatch pa=0x13ed0000 off=0xf4 mask=0x11 before=0x20 got=0x20 want=0x11")) {
            bits_diagnostic++;
            assert(strstr(logs, "stage=phy-calibration polls=0 last_ELBI=0"));
        }
        if (strstr(logs, "reset pulse mismatch off=0x288 got=0 want=1")) {
            reset_diagnostic++;
            assert(strstr(logs, "stage=phy-calibration polls=0 last_ELBI=0"));
        }
        if (strstr(logs, "PCI control mismatch off=0x4 mask=0x147 before=0xdead0144 got=0xdead0144 want=0x2")) {
            pci_diagnostic++;
            assert(strstr(logs, "stage=root-policy polls=0 last_ELBI=0"));
        }
    }
    assert(bits_diagnostic && reset_diagnostic && pci_diagnostic);
    /* Replay the exact hardware mismatch, keeping the vendor write of 0x1d0c.
     * A different baseline, another missing bit or all-ones readback must stop. */
    reset(8, 0); b2l_wifi_diag_start(); verify_quiet();
    assert(strstr(logs, "Gen1 link up:") && strstr(logs, "Gen1 quiesce verified"));
    assert(strstr(logs, "observed board readback before=0x1d02 wrote=0x1d0c got=0x1d04"));
    unsigned board_writes = writes;
    for (unsigned failure = 1; failure <= board_writes; failure++) {
        reset(8, 0); drop_write = failure; b2l_wifi_diag_start(); verify_quiet();
    }
    for (unsigned fault = 9; fault <= 11; fault++) {
        reset(fault, 0); b2l_wifi_diag_start(); verify_quiet();
        assert(!link_polls && !strstr(logs, "Gen1 root DBI ID="));
        assert(strstr(logs, "PHY SYSREG profile mismatch"));
        assert(strstr(logs, "stage=phy-calibration polls=0") && strstr(logs, "Gen1 quiesce verified"));
    }
    for (unsigned fault = 6; fault <= 7; fault++) {
        reset(fault, 0); b2l_wifi_diag_start();
        assert(strstr(logs, "Gen1 quiesce FAILED") && strstr(logs, "power probe restore verified"));
        assert(!(memory[0][0x71c/4] & 1) && !(memory[1][0x84/4] & 32));
        assert(!memory[2][0x20/4] && !(memory[2][0x24/4] & 4));
        assert(now_us < 1000000 && unmaps == 9 && writes < normal_writes + 20);
    }
    puts("Gen1: exact MMIO boundaries, PHY profile, reset/power ordering, no BME/ATU/endpoint access, deadline, dropped writes and quiescence passed");
    mode = 5;
    reset(8, 0); b2l_wifi_diag_start(); verify_quiet();
    assert(endpoint_reads == 4 && strstr(logs, "endpoint identity verified: 14e4:4475 subsystem 14e4:4375 PCI revision=5 class=0x28000"));
    assert(strstr(logs, "endpoint probe end: stage=complete cleanup=verified"));
    unsigned endpoint_writes = writes;
    for (unsigned failure = 1; failure <= endpoint_writes; failure++) {
        reset(8, 0); drop_write = failure; b2l_wifi_diag_start(); verify_quiet();
        assert(endpoint_reads <= 4);
    }
    for (unsigned failure = 1; failure <= 10; failure++) {
        reset(8, failure); b2l_wifi_diag_start();
        assert(!writes && !endpoint_reads && maps == failure && unmaps == failure - 1);
    }
    for (unsigned fault = 1; fault <= 19; fault++) {
        reset(8, 0); endpoint_fault = fault;
        if (fault == 7) atu[1] = 0x80000000; /* Existing active region must not be changed. */
        if (fault == 8) memory[8][0x900/4] = UINT32_MAX; /* Unsupported unrolled ATU. */
        if (fault == 16) atu[1] = UINT32_MAX;
        if (fault == 18) memory[8][0x0c/4] = 0;
        b2l_wifi_diag_start(); verify_quiet();
        if (fault == 1 || fault == 2 || fault == 6 || fault == 11 || fault == 14) assert(endpoint_reads == 1);
        if (fault == 3) assert(endpoint_reads == 2);
        if (fault == 13) assert(endpoint_reads == 3);
        if (fault == 4 || fault == 10) assert(endpoint_reads == 4);
        if (fault == 5 || fault == 7 || fault == 8 || fault == 9 || fault == 12 || fault == 15 || fault == 16 || fault == 17 || fault == 18 || fault == 19)
            assert(!endpoint_reads);
        if (fault == 10) {
            assert(strstr(logs, "cleanup=FAILED") && strstr(logs, "Gen1 quiesce FAILED"));
            assert(writes < endpoint_writes + 20);
        } else assert(!strstr(logs, "endpoint identity verified:") && strstr(logs, "Gen1 quiesce verified"));
    }
    reset(8, 0); original_viewport = memory[8][0x900/4] = 0x80000002;
    b2l_wifi_diag_start(); verify_quiet();
    assert(endpoint_reads == 4 && strstr(logs, "endpoint identity verified:"));
    reset(8, 0); atu[0] = atu[2] = atu[3] = atu[4] = atu[5] = atu[6] = UINT32_MAX;
    b2l_wifi_diag_start(); verify_quiet();
    assert(endpoint_reads == 4 && strstr(logs, "endpoint identity verified:"));
    reset(1, 0); b2l_wifi_diag_start(); verify_quiet();
    assert(link_polls == 500 && !endpoint_reads && !atu_enable_write);
    puts("Endpoint: exact read-only CFG0 scope, ATU1/bus guards and cleanup, link/deadline checks, all mapping/dropped-write failures passed");
    mode = 6;
    reset(8, 0); b2l_wifi_diag_start(); verify_quiet();
    assert(chip_reads == 3 && strstr(logs, "chipcommon identity verified: chip=0x4375 revision=3 package=8 interconnect=1"));
    assert(strstr(logs, "chipcommon probe end: stage=complete cleanup=verified"));
    unsigned chip_writes = writes;
    for (unsigned failure = 1; failure <= chip_writes; failure++) {
        reset(8, 0); drop_write = failure; b2l_wifi_diag_start(); verify_quiet();
        assert(chip_reads <= 3);
    }
    for (unsigned failure = 1; failure <= 11; failure++) {
        reset(8, failure); b2l_wifi_diag_start();
        assert(!writes && !endpoint_reads && !chip_reads && maps == failure && unmaps == failure - 1);
    }
    for (unsigned fault = 1; fault <= 14; fault++) {
        reset(8, 0); chip_fault = fault;
        if (fault == 10) atu0[1] = 0x80000000;
        if (fault == 11) memory[9][0x2010/4] = 0;
        if (fault == 12) memory[9][0x2014/4] = 1;
        if (fault == 13) memory[9][0x2080/4] = UINT32_MAX;
        b2l_wifi_diag_start(); verify_quiet();
        if (fault >= 6 && fault <= 9) {
            assert(strstr(logs, "chipcommon probe end:") && strstr(logs, "cleanup=FAILED"));
            assert(strstr(logs, "Gen1 quiesce FAILED") && strstr(logs, "power probe restore verified"));
            assert(writes < chip_writes + 25);
            assert(chip_reads == (fault == 9 ? 0u : 3u));
        } else {
            assert(!strstr(logs, "chipcommon identity verified:") && strstr(logs, "Gen1 quiesce verified"));
            assert(chip_reads == (fault >= 10 ? 0u : fault == 3 ? 2u : fault == 4 ? 3u : 1u));
        }
    }
    reset(8, 0); original_endpoint_command = memory[9][0x2004/4] = 0xabcd0400;
    original_bar0 = memory[9][0x2010/4] = 0x14400004;
    original_viewport = memory[8][0x900/4] = 0x80000002;
    b2l_wifi_diag_start(); verify_quiet();
    assert(chip_reads == 3 && strstr(logs, "chipcommon identity verified:"));
    puts("Chipcommon: first-page reads, BAR0/window/command and ATU0 cleanup, no BME/RAM/chip writes, map and dropped-write failures passed");
    mode = 7;
    reset(8, 0); b2l_wifi_diag_start(); verify_quiet();
    assert(erom_reads == 7 && strstr(logs, "EROM capture complete: words=7"));
    unsigned erom_writes = writes;
    for (unsigned failure = 1; failure <= erom_writes; failure++) {
        reset(8, 0); drop_write = failure; b2l_wifi_diag_start(); verify_quiet();
        assert(erom_reads <= 7);
    }
    for (unsigned fault = 1; fault <= 12; fault++) {
        if (fault == 11) continue;
        reset(8, 0); erom_fault = fault;
        if (fault == 1) erom[0] = UINT32_MAX;
        if (fault == 2 || fault == 9) memset(erom, 0, sizeof(erom));
        if (fault == 5) memory[10][0] = 0x10044375;
        if (fault == 6) memory[10][0xfc/4] = 0x18112000;
        if (fault == 7) {
            erom[2] = 0x1800003d; erom[3] = 0xf; /* Untagged address high. */
            erom[4] = 0x2008; erom[5] = 0xf; /* Untagged size high. */
        }
        if (fault == 8) erom[2] = UINT32_MAX;
        if (fault == 9) erom[895] = 0xf;
        b2l_wifi_diag_start(); verify_quiet();
        if (fault == 4 || fault == 10 || fault == 12) {
            assert(strstr(logs, "cleanup=FAILED") && strstr(logs, "Gen1 quiesce FAILED"));
            assert(strstr(logs, "power probe restore verified"));
        } else assert(strstr(logs, "Gen1 quiesce verified"));
        if (fault == 1 || fault == 3 || fault == 4) assert(erom_reads == 1);
        if (fault == 2 || fault == 9) assert(erom_reads == 896);
        if (fault == 5 || fault == 6) assert(!erom_reads && !strstr(logs, "EROM capture begin:"));
        if (fault == 7 || fault == 10 || fault == 12) assert(erom_reads == 7);
        if (fault == 8) assert(erom_reads == 3);
        if (fault == 7 || fault == 9 || fault == 10 || fault == 12) assert(strstr(logs, "EROM capture complete:"));
        else assert(!strstr(logs, "EROM capture complete:"));
        assert(writes < erom_writes + 4000);
    }
    puts("EROM: measured page only, 896-word/deadline bounds, data extensions vs terminators, invalid data and cleanup faults passed");
    mode = 8;
    reset(8, 0); b2l_wifi_diag_start(); verify_quiet();
    assert(wrapper_reads[0] == 3 && wrapper_reads[1] == 3 && r4_cap_reads == 1);
    assert(strstr(logs, "core snapshot complete: wrappers=2 R4_capabilities=read"));
    unsigned core_writes = writes;
    for (unsigned failure = 1; failure <= core_writes; failure++) {
        reset(8, 0); drop_write = failure; b2l_wifi_diag_start(); verify_quiet();
        assert(wrapper_reads[0] <= 3 && wrapper_reads[1] <= 3 && r4_cap_reads <= 1);
    }
    for (unsigned fault = 1; fault <= 9; fault++) {
        reset(8, 0); core_fault = fault;
        if (fault == 1) wrapper_values[1][0] = 0;
        if (fault == 2) wrapper_values[1][0] = 3;
        if (fault == 3) wrapper_values[1][2] = 1;
        if (fault == 4) wrapper_values[0][0] = UINT32_MAX;
        b2l_wifi_diag_start(); verify_quiet();
        if (fault <= 3) {
            assert(!r4_cap_reads && strstr(logs, "core snapshot complete: wrappers=2 R4_capabilities=skipped"));
        }
        if (fault == 4 || fault == 6 || fault == 7) assert(wrapper_reads[0] == 1 && !wrapper_reads[1] && !r4_cap_reads);
        if (fault >= 7) {
            assert(strstr(logs, "cleanup=FAILED") && strstr(logs, "Gen1 quiesce FAILED"));
            assert(strstr(logs, "power probe restore verified"));
        } else assert(strstr(logs, "Gen1 quiesce verified"));
    }
    static const unsigned witness_offsets[] = { 0x44, 0x48, 0x50, 0x68, 0x70, 0x74, 0x7c, 0xa0 };
    for (unsigned i = 0; i < 8; i++) {
        reset(8, 0); erom[witness_offsets[i]/4] ^= 0x1000; b2l_wifi_diag_start(); verify_quiet();
        assert(!wrapper_reads[0] && !wrapper_reads[1] && !r4_cap_reads);
        assert(strstr(logs, "core snapshot skipped: measured EROM witnesses/count differ"));
    }
    reset(8, 0); erom[132] = 0; erom[133] = 0xf; b2l_wifi_diag_start(); verify_quiet();
    assert(erom_reads == 134 && !wrapper_reads[0] && !strstr(logs, "core snapshot begin:"));
    puts("Core state: measured EROM witnesses, fixed read-only wrappers, gated/reset R4 rejection and dropped-write/cleanup failures passed");
    mode = 9;
    reset(8, 0); b2l_wifi_diag_start(); verify_quiet();
    assert(bank_reads == 13 && bank_writes == 14 && bank_index == 7);
    /* Five 4-KiB banks and eight 128-KiB banks cover both unit-size encodings. */
    assert(strstr(logs, "TCM inventory end: stage=complete banks=13 bytes=1069056 selector_restore=verified"));
    unsigned tcm_writes = writes;
    for (unsigned failure = 1; failure <= tcm_writes; failure++) {
        reset(8, 0); drop_write = failure; b2l_wifi_diag_start(); verify_quiet();
        assert(bank_reads <= 13 && bank_index == 7);
        assert(!strstr(logs, "selector_restore=FAILED"));
    }
    for (unsigned fault = 1; fault <= 11; fault++) {
        reset(8, 0); tcm_fault = fault;
        if (fault == 10) wrapper_values[1][0] = 1; /* Running R4: no selector access. */
        b2l_wifi_diag_start(); verify_quiet();
        if (fault == 1 || (fault >= 8 && fault <= 10)) assert(!bank_writes && !bank_reads);
        if (fault == 2) assert(bank_reads == 1 && bank_index == 7);
        if (fault == 3) assert(bank_reads == 6 && bank_index == 7);
        if (fault == 4) assert(bank_reads == 1 && bank_index == 7);
        if (fault == 5) assert(bank_reads == 1 && bank_index == 0);
        if (fault == 6) assert(bank_reads == 13 && bank_index == 12 && bank_writes == 16);
        if (fault == 7) assert(!bank_reads && bank_writes == 2 && bank_index == 7);
        if (fault == 11) assert(!bank_reads && bank_writes == 2 && bank_index == 7);
        if (fault == 5 || fault == 6) {
            assert(strstr(logs, "selector_restore=FAILED") && strstr(logs, "cleanup=FAILED"));
            assert(strstr(logs, "power probe restore verified"));
        } else assert(bank_index == 7 && strstr(logs, "Gen1 quiesce verified"));
    }
    for (unsigned i = 0; i < 8; i++) {
        reset(8, 0); erom[witness_offsets[i]/4] ^= 0x1000; b2l_wifi_diag_start(); verify_quiet();
        assert(!wrapper_reads[0] && !bank_reads && !bank_writes);
    }
    reset(8, 0); erom[132] = 0; erom[133] = 0xf; b2l_wifi_diag_start(); verify_quiet();
    assert(!wrapper_reads[0] && !bank_reads && !bank_writes);
    /* Same clock/forced-clock/reset guards also apply to the mutation mode. */
    for (unsigned fault = 1; fault <= 3; fault++) {
        reset(8, 0);
        if (fault == 1) wrapper_values[1][0] = 0;
        if (fault == 2) wrapper_values[1][0] = 0x23;
        if (fault == 3) wrapper_values[1][2] = 1;
        b2l_wifi_diag_start(); verify_quiet();
        assert(!bank_reads && !bank_writes && strstr(logs, "TCM inventory skipped:"));
    }
    puts("TCM inventory: halted measured R4, selector-only writes, bounded 13-bank sizing, both units and selector restoration/fault guards passed");
    mode = 10;
    for (unsigned fault = 1; fault <= 10; fault++) {
        reset(8, 0); firmware_fault = fault; b2l_wifi_diag_start();
        assert(!maps && !writes && strstr(logs,"firmware input absent/invalid; no MMIO access"));
    }
    reset(8, 0); b2l_wifi_diag_start(); verify_quiet();
    assert(fw_writes == sizeof(firmware_ram)/4 && fw_reads == fw_writes);
    assert(!memcmp(firmware_ram, firmware_fixture, sizeof(firmware_ram)));
    assert(strstr(logs,"firmware download complete: bytes=1297036 readback=verified R4=halted"));
    unsigned normal_windows = fw_windows;
    for (unsigned fault = 1; fault <= 10; fault++) {
        reset(8, 0); fw_fault=fault;
        if (fault == 10) bank_info[1] ^= 0x8000; /* Same size, changed raw profile. */
        b2l_wifi_diag_start(); verify_quiet();
        assert((strstr(logs,"firmware download complete:") != NULL) == (fault == 8));
        if (fault <= 3) assert(fw_reads==1024 && fw_writes==1024);
        if (fault == 4 || fault == 10) assert(!fw_reads && !fw_writes);
        if (fault == 5) assert(fw_reads==5 && fw_writes==16);
        if (fault == 6) assert(!fw_reads && fw_writes==16);
        if (fault == 7) assert(fw_reads==16 && fw_writes==16);
        if (fault >= 7 && fault <= 9) {
            assert(strstr(logs,"cleanup=FAILED") && strstr(logs,"power probe restore verified"));
        } else assert(strstr(logs,"Gen1 quiesce verified"));
    }
    const unsigned lost_words[]={1,16,17,1024,1025,160000,sizeof(firmware_ram)/4};
    for (unsigned i=0;i<sizeof(lost_words)/sizeof(lost_words[0]);i++) {
        reset(8,0);fw_drop_word=lost_words[i];b2l_wifi_diag_start();verify_quiet();
        assert(fw_reads==fw_drop_word && fw_writes >= fw_reads && fw_writes-fw_reads < 16);
        assert(!strstr(logs,"firmware download complete:") && strstr(logs,"Gen1 quiesce verified"));
    }
    const unsigned lost_windows[]={1,2,3,317,634,normal_windows};
    for (unsigned i=0;i<sizeof(lost_windows)/sizeof(lost_windows[0]);i++) {
        reset(8,0);fw_drop_window=lost_windows[i];b2l_wifi_diag_start();verify_quiet();
        assert(!strstr(logs,"firmware download complete:") && strstr(logs,"Gen1 quiesce verified"));
    }
    for (unsigned i=0;i<8;i++) {
        reset(8,0);erom[witness_offsets[i]/4]^=0x1000;b2l_wifi_diag_start();verify_quiet();
        assert(!fw_reads && !fw_writes && !bank_reads);
    }
    reset(8,0);wrapper_values[1][0]=1;b2l_wifi_diag_start();verify_quiet();
    assert(!fw_reads && !fw_writes && !bank_reads);
    reset(8,0);tcm_fault=9;b2l_wifi_diag_start();verify_quiet();
    assert(!fw_reads && !fw_writes && !bank_reads);
    puts("Firmware: authenticated reserved input, exact measured TCM scope, full synthetic image/readback, halted R4 per page, deadline/link/readback/lost-write and cleanup guards passed");
    /* S10 port, mode 11: the DHD start sequence on the synthetic firmware. */
    mode = 11;
    static const char entries[] = "boardtype=0x1234\0boardrev=0x1101\0macaddr=00:90:4c:c5:12:38";
    uint8_t good[64] = { 0 };
    memcpy(good, entries, sizeof(entries)); /* 59 bytes, then DHD's NUL padding to 64 */
    for (unsigned bad = 1; bad <= 5; bad++) {
        uint8_t nvram[64];
        memcpy(nvram, good, sizeof(nvram));
        if (bad == 1) nvram[3] = 1;          /* a control character */
        if (bad == 2) nvram[9] = '-';        /* an entry without '=' */
        if (bad == 3) nvram[62] = 'x';       /* the last word is not zero */
        if (bad == 4) nvram[60] = 'y';       /* text after the end */
        reset(8, 0);
        if (bad != 5) firmware_fixture_nvram(nvram, sizeof(nvram)); /* 5: the firmware alone */
        b2l_wifi_diag_start();
        assert(!maps && !writes && !thread_starts && strstr(logs, "firmware input absent/invalid; no MMIO access"));
        assert((strstr(logs, "private NVRAM input rejected: bytes=64") != NULL) == (bad != 5));
    }
    reset(8, 0); firmware_fixture_nvram(good, sizeof(good)); thread_fail = 1; b2l_wifi_diag_start();
    assert(!maps && !writes && thread_starts == 1 && !threads_released && !deferred_s);
    assert(strstr(logs, "mode 11: thread start failed; no MMIO access"));
    const uint32_t varaddr = 0x2ffffc - sizeof(good);
    for (unsigned fault = 0; fault <= 5; fault++) {
        reset(8, 0); firmware_fixture_nvram(good, sizeof(good)); nvram_fixture_bytes = sizeof(good);
        boot_fault = fault;
        b2l_wifi_diag_start(); verify_quiet();
        assert(thread_starts == 1 && threads_released == 1 && deferred_s == 360);
        assert(strstr(logs, "private NVRAM input verified: bytes=64 entries=3 boardtype=0x1234 boardrev=0x1101"));
        assert(strstr(logs, "firmware download complete: bytes=1297036 readback=verified R4=halted"));
        assert(!memcmp(tcm11, firmware_fixture, 1297036));
        assert(!memcmp((uint8_t *)tcm11 + (varaddr - 0x170000), good, sizeof(good)));
        assert(tcm11[TCM11_TOP] == (fault == 0 || fault == 2 ? 0x2f0000u : 0xffef0010u));
        assert(tcm11[(varaddr - 8 - 0x170000) / 4] == 0xfeedc0de && tcm11[(varaddr - 4 - 0x170000) / 4] == 128);
        for (unsigned b = 0; b < 128; b++)
            assert(((uint8_t *)tcm11)[varaddr - 136 - 0x170000 + b] == (uint8_t)(0xa5u ^ b));
        assert(vector0 == (fault == 3 || fault == 4 ? 0 : 0x2e211407u));
        assert(r4_runs == (fault <= 2 ? 1u : 0u));
        assert(strstr(logs, "cleanup=verified") && strstr(logs, "Gen1 quiesce verified"));
        assert(strstr(logs, "mode 11: deferred run finished"));
        if (fault <= 2) {
            assert(strstr(logs, "R4 released: firmware running"));
            assert(strstr(logs, "firmware boot end: R4 ran, halted again"));
        }
        if (fault == 0 || fault == 2) {
            assert(strstr(logs, "shared at 0x2f0000 after 2 polls: flags="));
            assert(strstr(logs, "console at 0x2f0100: buffer=0x2f1000 size=8192 index="));
            assert(strstr(logs, "console: RTE (PCIE-MSG_BUF) 17.10.157 (r) on BCM4375 r5 @ 37.4/168.0/1600.0MHz\n"));
            assert(strstr(logs, "console: 000000.007 pciedev ready\n"));
            assert(strstr(logs, "console: last line without newline\n"));
            assert(strstr(logs, "lines=4\n"));
            assert((strstr(logs, "firmware trap: type=0x100 epc=0x101") != NULL) == (fault == 2));
        }
        if (fault == 1) assert(strstr(logs, "shared area not published: last word=0xffef0010 after 100 polls"));
        if (fault == 3) assert(strstr(logs, "firmware boot stopped: reset vector before=0 got=0 want=0x2e211407"));
        if (fault == 4) assert(strstr(logs, "firmware boot stopped: reset vector before=0xffffffff"));
        if (fault >= 3) assert(!strstr(logs, "R4 released"));
        if (fault == 5) assert(strstr(logs, "R4 wrapper resetstatus stays 0x1") &&
            strstr(logs, "firmware boot end: R4 never ran, halted again"));
    }
    puts("Firmware boot: NVRAM layout guard, deferred thread, DHD NVRAM/token/seed/vector order, release and re-halt, shared/console/trap reads, read-only RAM while running passed");
    /* S10 port, mode 12: the host-side layout, read only, then 2 s more. */
    mode = 12;
    for (unsigned fault = 0; fault <= 1; fault++) {
        reset(8, 0); firmware_fixture_nvram(good, sizeof(good)); nvram_fixture_bytes = sizeof(good);
        boot_fault = fault ? 1 : 0;
        b2l_wifi_diag_start(); verify_quiet();
        assert(thread_starts == 1 && threads_released == 1 && deferred_s == 360 && r4_runs == 1);
        assert(strstr(logs, "mode 12: deferred run finished") && strstr(logs, "cleanup=verified"));
        assert(strstr(logs, "firmware boot end: R4 ran, halted again"));
        if (fault) {
            /* no shared area: nothing of mode 12 is read */
            assert(strstr(logs, "shared area not published") && !strstr(logs, "rings info at"));
            continue;
        }
        assert(strstr(logs, "shared flags 0x7 (IPC revision 7): none"));
        assert(strstr(logs, "lfrag_pkts=16 max_host_rxbufs=64 dma_rxoffset=0 h2d_mb=0x2f0500 d2h_mb=0x2f0504 "
            "rings_info=0x2f0400"));
        assert(strstr(logs, "flags2=0x12"));
        assert(strstr(logs, "h2d mailbox data at 0x2f0500: 0\n") && strstr(logs, "d2h mailbox data at 0x2f0504: 0\n"));
        assert(strstr(logs, "rings info at 0x2f0400: ringmem=0x2f0600 h2d_w=0x2f0700 h2d_r=0x2f0710 "
            "d2h_w=0x2f0720 d2h_r=0x2f0730 max_tx_flowrings=40 submission=42 completion=3 vdevs=8"));
        assert(strstr(logs, "ring 0: id 0 type 1 max_item 64 item 40 bytes base 0:0"));
        assert(strstr(logs, "ring 2: id 2 type 1 max_item 64 item 24 bytes base 0:0"));
        assert(strstr(logs, "ring 4: id 4 type 3 max_item 512 item 32 bytes base 0:0"));
        assert(strstr(logs, "rings: d2h write indices at 0x2f0720: 1 0 0"));
        assert(strstr(logs, "rings: h2d read indices at 0x2f0710: 0 0 0"));
        assert(strstr(logs, "after 2 s more: shared flags 0x7\n"));
        assert(strstr(logs, "console: no new text since index"));
    }
    puts("Firmware host layout: whole shared area, ring info, common rings and indices, mailboxes, a second console read, all read only, passed");
    /* S10 port, mode 13: the host side of the rings on the synthetic firmware.
     * 1 no memory, 2 a page hole, 3 not owned RAM, 4 no hostready flag,
     * 5 the firmware never takes the buffers, 6 the endpoint will not master. */
    mode = 13;
    for (unsigned fault = 0; fault <= 6; fault++) {
        reset(8, 0); firmware_fixture_nvram(good, sizeof(good)); nvram_fixture_bytes = sizeof(good);
        host13_fault = fault;
        b2l_wifi_diag_start(); verify_quiet();
        assert(thread_starts == 1 && threads_released == 1 && deferred_s == 360 && r4_runs == 1);
        assert(strstr(logs, "mode 13: deferred run finished") && strstr(logs, "cleanup=verified"));
        assert(strstr(logs, "firmware boot end: R4 ran, halted again"));
        assert(!wifi_master && !(memory[8][4/4] & 4) && !(memory[9][0x2004/4] & 4));
        assert(host_allocs == (fault == 4 ? 0u : 1u));
        if (fault == 4) {
            assert(strstr(logs, "mode 13: flags 0x7 are not IPC 7 with hostready and xorcsum") && !bme_ever);
            continue;
        }
        if (fault == 1) {
            assert(strstr(logs, "mode 13: no contiguous host memory below 4 GiB") && !bme_ever && !cleans);
            continue;
        }
        if (fault == 2 || fault == 3) {
            assert(strstr(logs, "is not contiguous owned RAM below 4 GiB; kept, not used") && !bme_ever && !cleans);
            continue;
        }
        assert(strstr(logs, "mode 13: host memory 131072 bytes at pa 0x9f000000: contiguous owned RAM"));
        assert(strstr(logs, "five common rings at pa 0x9f000000, indices zero, scratch at 0x9f011000 (8 bytes), "
            "host_cap 0x801407: written and read back"));
        assert(bme_ever);
        if (fault == 6) {
            assert(strstr(logs, "bus mastering did not come on; nothing rung; off again verified"));
            assert(!hostready13 && !db0_13 && !strstr(logs, "bus mastering off again (endpoint"));
            continue;
        }
        assert(strstr(logs, "bus mastering on: root port, then endpoint") && hostready13 == 1 && db0_13 == 1);
        assert(strstr(logs, "after hostready: control submissions read 0 of 6; completion writes 0 0 0; mailbox int 0;"));
        assert(strstr(logs, "2 ioctl-response and 4 event buffers posted, control submission write index 6, DB0 0xff000006"));
        assert(strstr(logs, "bus mastering off again (endpoint, then root port); host memory kept, never reused"));
        assert(strstr(logs, "mode 13: scratch 0 0\n") && flushes == 1 && cleans >= 2);
        if (fault == 5) {
            assert(strstr(logs, "2000 ms after the posts: control submissions read 0 of 6; completion writes 0 0 0"));
            assert(!strstr(logs, " 200 ms after the posts") && !strstr(logs, "ring 2 item 0"));
            continue;
        }
        assert(strstr(logs, " 200 ms after the posts: control submissions read 6 of 6; completion writes 1 0 0; "
            "mailbox int 0x100"));
        assert(strstr(logs, "2000 ms after the posts: control submissions read 6 of 6; completion writes 1 0 0"));
        char line[200];
        snprintf(line, sizeof(line), "ring 2 item 0: type 0x2 if 0 flags 0 epoch 1 request 0x77 status 0 ring 2; "
            "0x11 0x22 %#x 0; xor 0\n", 0x01000002u ^ 0x77u ^ 0x00020000u ^ 0x11u ^ 0x22u);
        assert(strstr(logs, line));
        assert(strstr(logs, "mode 13: event buffer 2: 0xe1 0xe2 0xe3 0xe4\n"));
    }
    puts("Firmware host rings: owned contiguous memory, DHD's ring/scratch/host_cap writes, cache cleans before each doorbell, bus mastering order and its undo, posted buffers, completions read back passed");
    /* S10 port, mode 14: one ioctl after the buffers. 5: the buffers are
     * never taken, so no ioctl; 7: the ioctl gets no answer. */
    mode = 14;
    static const unsigned faults14[] = { 0, 5, 7 };
    for (unsigned k = 0; k < 3; k++) {
        unsigned fault = faults14[k];
        reset(8, 0); firmware_fixture_nvram(good, sizeof(good)); nvram_fixture_bytes = sizeof(good);
        host13_fault = fault;
        b2l_wifi_diag_start(); verify_quiet();
        assert(strstr(logs, "mode 14: deferred run finished") && strstr(logs, "cleanup=verified"));
        assert(strstr(logs, "bus mastering off again (endpoint, then root port)"));
        assert(!wifi_master && !(memory[8][4/4] & 4) && !(memory[9][0x2004/4] & 4) && hostready13 == 1);
        if (fault == 5) {
            assert(strstr(logs, "mode 14: the firmware took 0 of the 6 buffers; no ioctl sent") && !ioctl14_db);
            continue;
        }
        assert(ioctl14_db == 1 && db0_13 == 2);
        assert(strstr(logs, "mode 14: ioctl WLC_GET_MAGIC sent (4 bytes in and out, input at pa 0x9f011800), "
            "write index 7, DB0 0xff000007"));
        if (fault == 7) {
            assert(strstr(logs, "2000 ms after the ioctl: control submissions read 6 of 7; completion writes 1 0 0"));
            assert(!strstr(logs, "ring 2 item 1") && !strstr(logs, "ioctl-response buffer 0"));
            continue;
        }
        assert(strstr(logs, " 200 ms after the ioctl: control submissions read 7 of 7; completion writes 3 0 0; "
            "mailbox int 0x200"));
        char line[200];
        snprintf(line, sizeof(line), "ring 2 item 1: type 0xa if 0 flags 0 epoch 2 request 0xfffe status 0 ring 2; "
            "0 0 %#x 0; xor 0\n", 0x0200000au ^ 0xfffeu ^ 0x00020000u);
        assert(strstr(logs, line));
        snprintf(line, sizeof(line), "ring 2 item 2: type 0xc if 0 flags 0 epoch 3 request 0x1 status 0 ring 2; "
            "0x10004 0 %#x 0; xor 0\n", 0x0300000cu ^ 1u ^ 0x00020000u ^ 0x10004u);
        assert(strstr(logs, line));
        assert(strstr(logs, "mode 13: ioctl-response buffer 0: 0x14e46c77 0 0 0\n"));
    }
    puts("Firmware ioctl: WLC_GET_MAGIC as dhd_fillup_ioct_reqst sends it, after the buffers only, its acknowledgement, completion and answer read back passed");
    /* S10 port, mode 15: "ver", then "cur_etheraddr". 7: the first ioctl is
     * never answered, so no iovar; 8: "ver" is never answered, so no
     * cur_etheraddr. */
    mode = 15;
    static const unsigned faults15[] = { 0, 7, 8 };
    for (unsigned k = 0; k < 3; k++) {
        unsigned fault = faults15[k];
        reset(8, 0); firmware_fixture_nvram(good, sizeof(good)); nvram_fixture_bytes = sizeof(good);
        host13_fault = fault;
        b2l_wifi_diag_start(); verify_quiet();
        assert(strstr(logs, "mode 15: deferred run finished") && strstr(logs, "cleanup=verified"));
        assert(strstr(logs, "bus mastering off again (endpoint, then root port)"));
        assert(!wifi_master && !(memory[8][4/4] & 4) && !(memory[9][0x2004/4] & 4));
        if (fault == 7) {
            assert(strstr(logs, "mode 15: the ioctl was not answered (read 6, completions 1); no iovars sent"));
            assert(!iovar15_db);
            continue;
        }
        assert(strstr(logs, "mode 15: completion read index 3 given back; two more ioctl-response buffers posted"));
        assert(strstr(logs, "mode 15: WLC_GET_VAR ver sent (256 bytes in and out), write index 10, DB0 0xff00000a"));
        if (fault == 8) {
            assert(strstr(logs, "mode 15: ver: no completion in 2 s") && iovar15_db == 1);
            assert(!strstr(logs, "WLC_GET_VAR cur_etheraddr"));
            continue;
        }
        assert(iovar15_db == 2 && db0_13 == 4);
        assert(strstr(logs, "mode 15: ver (status 0, 32 bytes): wl0: test version 18.41.117\n"));
        assert(strstr(logs, "mode 15: WLC_GET_VAR cur_etheraddr sent (32 bytes in and out), write index 11, DB0 0xff00000b"));
        assert(strstr(logs, "mode 15: cur_etheraddr (status 0, 6 bytes): matches the NVRAM macaddr\n"));
        assert(!strstr(logs, "00:90:4c") && !strstr(logs, "0xc54c9000"));
        assert(strstr(logs, "ring 2 item 6: type 0xc"));
    }
    puts("Firmware iovars: buffers reposted, read index given back, ver then cur_etheraddr one at a time, the MAC compared and never logged passed");
    /* S10 port, mode 16: the attachment's MAC. 0: set and read back; 9: the
     * set is never answered; 10: the chip keeps its own; /chosen absent; the
     * chip's already the attachment's. */
    mode = 16;
    static const unsigned faults16[][2] = { { 0, 0 }, { 9, 0 }, { 10, 0 }, { 0, 1 }, { 0, 2 } };
    for (unsigned k = 0; k < 5; k++) {
        reset(8, 0); firmware_fixture_nvram(good, sizeof(good)); nvram_fixture_bytes = sizeof(good);
        host13_fault = faults16[k][0]; firmware_mac_mode = faults16[k][1];
        b2l_wifi_diag_start(); verify_quiet();
        assert(strstr(logs, "mode 16: deferred run finished") && strstr(logs, "cleanup=verified"));
        assert(strstr(logs, "bus mastering off again (endpoint, then root port)"));
        assert(!strstr(logs, "0x5e1cb202") && !strstr(logs, "02:b2") && !strstr(logs, "0xc54c9000"));
        if (firmware_mac_mode == 1) {
            assert(strstr(logs, "mode 16: no mac-address-wifi0 in /chosen; nothing set") && db0_13 == 4);
            continue;
        }
        if (firmware_mac_mode == 2) {
            assert(strstr(logs, "mode 16: the chip's address already is the attachment's; nothing set") && db0_13 == 4);
            continue;
        }
        assert(strstr(logs, "mode 16: the chip's address is not the attachment's; completion read index 7 given back"));
        assert(strstr(logs, "mode 16: WLC_SET_VAR cur_etheraddr sent (20 bytes in and out), write index 14, DB0 0xff00000e"));
        if (host13_fault == 9) {
            assert(strstr(logs, "mode 16: cur_etheraddr: no completion in 2 s") && db0_13 == 5);
            assert(!strstr(logs, "write index 15"));
            continue;
        }
        assert(strstr(logs, "mode 16: cur_etheraddr set: status 0\n"));
        assert(strstr(logs, "mode 16: WLC_GET_VAR cur_etheraddr sent (32 bytes in and out), write index 15, DB0 0xff00000f"));
        assert(db0_13 == 6);
        assert(strstr(logs, host13_fault == 10 ? "mode 16: cur_etheraddr read back (status 0, 6 bytes): still NOT the attachment's\n"
                                               : "mode 16: cur_etheraddr read back (status 0, 6 bytes): now the attachment's\n"));
    }
    puts("Firmware MAC: the attachment's address set when the chip's differs, read back, absent/same/unanswered cases, never logged passed");
    /* S10 port, mode 17: WLC_UP, isup, the first event. After the set (MAC
     * mode 0), with the address already right (2), and WLC_UP unanswered. */
    mode = 17;
    static const unsigned faults17[][2] = { { 0, 0 }, { 0, 2 }, { 11, 2 } };
    for (unsigned k = 0; k < 3; k++) {
        reset(8, 0); firmware_fixture_nvram(good, sizeof(good)); nvram_fixture_bytes = sizeof(good);
        host13_fault = faults17[k][0]; firmware_mac_mode = faults17[k][1];
        b2l_wifi_diag_start(); verify_quiet();
        assert(strstr(logs, "mode 17: deferred run finished") && strstr(logs, "cleanup=verified"));
        assert(strstr(logs, "bus mastering off again (endpoint, then root port)"));
        assert(!strstr(logs, "0x5e1cb202") && !strstr(logs, "0xc54c9000") && !strstr(logs, "buffer 0:"));
        assert(strstr(logs, "mode 17: completion read index ") &&
            strstr(logs, "given back; two more ioctl-response buffers posted"));
        const unsigned up_item = firmware_mac_mode == 2 ? 13 : 17;
        char line[160];
        snprintf(line, sizeof(line), "mode 17: ioctl 2 (WLC_UP) sent (4 bytes in and out), write index %u, DB0 %#x",
            up_item + 1, 0xff000000u | (up_item + 1));
        assert(strstr(logs, line) && up17);
        if (host13_fault == 11) {
            assert(strstr(logs, "mode 17: WLC_UP: no completion in 2 s") && !strstr(logs, "isup sent"));
            continue;
        }
        assert(strstr(logs, "mode 17: WLC_UP: status 0\n"));
        assert(strstr(logs, "mode 17: WLC_GET_VAR isup sent (32 bytes in and out)"));
        assert(strstr(logs, "mode 17: isup (status 0, 4 bytes): 1\n"));
        assert(strstr(logs, "mode 17: event 1: 48 bytes, type 54 status 0\n"));
        assert(strstr(logs, "mode 17: 1 firmware events so far\n"));
    }
    puts("Firmware up: WLC_UP and isup one at a time, the first event by type only, no address in the log passed");
    /* S10 port, mode 18: one passive scan. With the address already right
     * (2) and after the set (0); the escan refused (12); never finished (15). */
    mode = 18;
    static const unsigned faults18[][2] = { { 0, 2 }, { 0, 0 }, { 12, 2 }, { 15, 2 }, { 16, 2 } };
    for (unsigned k = 0; k < 5; k++) {
        reset(8, 0); firmware_fixture_nvram(good, sizeof(good)); nvram_fixture_bytes = sizeof(good);
        host13_fault = faults18[k][0]; firmware_mac_mode = faults18[k][1];
        b2l_wifi_diag_start(); verify_quiet();
        assert(strstr(logs, "mode 18: deferred run finished") && strstr(logs, "cleanup=verified"));
        assert(strstr(logs, "bus mastering off again (endpoint, then root port)"));
        assert(!strstr(logs, "ModelNet") && !strstr(logs, "aa:bb") && !strstr(logs, "0xaa2211") &&
            !strstr(logs, "0xbbaa") && !strstr(logs, "0x5e1cb202") && !strstr(logs, "0xc54c9000"));
        assert(strstr(logs, "mode 17: WLC_UP: status 0\n"));
        assert(strstr(logs, "ioctl-response buffers 9 to 12 posted again, 1 used event buffers given back\n"));
        const unsigned first = firmware_mac_mode == 2 ? 20 : 24;    /* scan_ver's item */
        char line[200];
        snprintf(line, sizeof(line), "mode 18: WLC_GET_VAR scan_ver sent (32 bytes in and out), write index %u, DB0 %#x",
            first + 1, 0xff000000u | (first + 1));
        assert(strstr(logs, line));
        const unsigned v2 = host13_fault != 16;
        assert(strstr(logs, v2 ? "mode 18: scan_ver (status 0, 32 bytes): version 1, length 6, major 2; escan version 2 sent\n"
                               : "mode 18: scan_ver (status -23, 0 bytes): not given; escan version 1 sent\n"));
        assert(strstr(logs, "mode 18: event_msgs (status 0, 16 bytes): ESCAN_RESULT off\n"));
        assert(strstr(logs, "mode 18: event_msgs set with ESCAN_RESULT: status 0\n"));
        snprintf(line, sizeof(line), "mode 18: WLC_SET_VAR escan sent (%u bytes in and out), write index %u, DB0 %#x",
            v2 ? 86u : 78u, first + 4, 0xff000000u | (first + 4));
        assert(strstr(logs, line));
        if (host13_fault == 12) {
            assert(strstr(logs, "mode 18: escan v2 (passive, every channel, 110 ms each) set: status -2\n"));
            assert(!strstr(logs, "results,") && !escan18);
            continue;
        }
        assert(strstr(logs, v2 ? "mode 18: escan v2 (passive, every channel, 110 ms each) set: status 0\n"
                               : "mode 18: escan v1 (passive, every channel, 110 ms each) set: status 0\n"));
        assert(strstr(logs, "mode 18: result 1: channel 1 (2.4 GHz), RSSI -40 dBm\n"));
        assert(strstr(logs, "mode 18: result 2: channel 6 (2.4 GHz), RSSI -67 dBm\n"));
        assert(strstr(logs, "mode 18: result 3: channel 1 (2.4 GHz), RSSI -42 dBm, a network heard before\n"));
        assert(strstr(logs, "mode 18: result 4: channel 36 (5 GHz), RSSI -71 dBm\n"));
        assert(strstr(logs, "mode 18: result 5: channel 149 (5 GHz), RSSI -80 dBm\n"));
        assert(strstr(logs, "mode 18: channels heard: 1 6 36 149\n"));
        if (host13_fault == 15) {
            assert(strstr(logs, "mode 18: escan not finished after about 8000 ms\n"));
            assert(strstr(logs, "mode 18: 5 results, 4 networks (2 on 2.4 GHz, 2 on 5/6 GHz), RSSI -80 to -40 dBm; "
                "0 other events, 0 malformed; 5 event buffers given back, 0 not (ring limits)\n"));
            continue;
        }
        assert(strstr(logs, "mode 18: escan finished after about 2100 ms, final status 0\n"));
        assert(strstr(logs, "mode 18: 5 results, 4 networks (2 on 2.4 GHz, 2 on 5/6 GHz), RSSI -80 to -40 dBm; "
            "0 other events, 0 malformed; 6 event buffers given back, 0 not (ring limits)\n"));
        assert(scan18_next == 6 && nev18 == 4);
    }
    puts("Firmware scan: one passive escan, event buffers given back, results by channel and level, no name or address in the log passed");
}
'''

with tempfile.TemporaryDirectory(prefix='b2l-wifi-gen1-') as temporary:
    path = Path(temporary) / 'gen1.c'
    binary = Path(temporary) / 'gen1-test'
    path.write_text(pre + source + post)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
        '-fno-omit-frame-pointer', '-I', str(repo / 'xnu-overlay/osfmk/arm64'), '-I', str(repo / 'tests'),
        str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
