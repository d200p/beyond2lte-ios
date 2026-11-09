#!/usr/bin/env python3
"""Exercise the actual Gen1 bring-up flow with strict MMIO and protocol guards."""
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
typedef uintptr_t vm_offset_t;
typedef unsigned long vm_size_t;
#define VM_WIMG_IO 0x66
#define NSEC_PER_MSEC 1000000u
static _Alignas(16384) uint32_t memory[9][4096];
static const uintptr_t physical[] = {
    0x15860000, 0x10430000, 0x13c40000, 0x13c00000, 0x13ed0000,
    0x13ef0000, 0x13ee0000, 0x13c10000, 0x13f00000
};
static unsigned maps, unmaps, fail_map, writes, drop_write, link_polls;
static unsigned trained, scenario, gen1_reads;
static uint64_t now_us;
static char logs[8192];
static unsigned log_size;
static void b2l_early_trace(const char *fmt, ...) {
    va_list args; va_start(args, fmt);
    int n = vsnprintf(logs + log_size, sizeof(logs) - log_size, fmt, args);
    va_end(args); assert(n >= 0 && (unsigned)n < sizeof(logs) - log_size);
    log_size += (unsigned)n;
}
static int PE_parse_boot_argn(const char *name, void *value, unsigned size) {
    assert(!strcmp(name, "b2l_wifi_diag") && size == 4); *(uint32_t *)value = 4; return 1;
}
static uint64_t mach_absolute_time(void) { return now_us; }
static void clock_interval_to_deadline(unsigned n, unsigned unit, uint64_t *at) {
    assert(n == 500 && unit == NSEC_PER_MSEC); *at = now_us + 500000;
}
static void delay(unsigned us) { assert(us == 10 || us == 20 || us == 100); now_us += us; }
static void delay_for_interval(unsigned n, unsigned unit) {
    assert(unit == NSEC_PER_MSEC && (n == 1 || n == 20 || n == 200)); now_us += n * 1000;
}
static uintptr_t ml_io_map_unmappable(uintptr_t pa, unsigned long size, unsigned flags) {
    assert(maps < 9 && pa == physical[maps] && size == 0x4000 && flags == VM_WIMG_IO);
    unsigned index = maps++;
    return maps == fail_map ? 0 : (uintptr_t)memory[index];
}
static void ml_io_unmap(uintptr_t va, unsigned long size) {
    unsigned successful = maps - (fail_map && fail_map <= maps ? 1 : 0);
    assert(size == 0x4000 && unmaps < successful);
    assert(va == (uintptr_t)memory[successful - ++unmaps]);
}
static unsigned region_index(uintptr_t address) {
    assert(maps == 9 && !fail_map && !unmaps);
    for (unsigned i = 0; i < 9; i++) if (address == (uintptr_t)memory[i]) return i;
    assert(0); return 99; /* No config/BAR/ATU/SysMMU/CMU-other mappings. */
}
static void powered(unsigned index) {
    if (index < 4) return;
    assert(now_us >= 201000);
    assert(memory[0][0x71c/4] & 1);
    assert(((memory[1][0x80/4] >> 20) & 15) == 1 && (memory[1][0x84/4] & 32));
    assert(((memory[2][0x20/4] >> 8) & 15) == 1);
    if (index == 8) assert(memory[6][0xec/4] & 8); /* Every DBI read/write ignores RX idle. */
}
static int valid_link_setup(void) {
    for (unsigned i = 0; i < sizeof(b2l_wifi_cmn); i++)
        if (memory[5][i] != b2l_wifi_cmn[i]) return 0;
    for (unsigned i = 0; i < sizeof(b2l_wifi_trsv); i++)
        if (memory[5][0x30+i] != b2l_wifi_trsv[i]) return 0;
    return (memory[2][0x24/4] & 4) && (memory[2][0x20/4] & 15) == 2 &&
        (memory[2][0x28/4] & 15) == 3 && (memory[7][0x1044/4] & 2) &&
        (memory[7][0x1050/4] & 0x10fe) == 0x100c &&
        memory[4][0x1d0/4] == 1 && memory[4][0x288/4] == 1 &&
        memory[4][0x28c/4] == 1 && memory[4][0x290/4] == 1 &&
        !(memory[4][0x0c/4] | memory[4][0x10/4] | memory[4][0x14/4]) &&
        memory[6][0xf8/4] == 0x70 && memory[6][0x100/4] == 0x87 &&
        memory[6][0x104/4] == 0x50 && !(memory[6][0xd0/4] & 0xc0) &&
        !(memory[8][0x04/4] & 0x145) && (memory[8][0xa0/4] & 15) == 1 &&
        (memory[8][0x710/4] & 0x3f0000) == 0x10000 &&
        (memory[8][0x80c/4] & 0x1f00) == 0x100 && memory[8][0xb40/4] == 26;
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
                    off == 0xa0 || off == 0x710 || off == 0x80c || off == 0x8bc || off == 0xb40));
    assert(allowed);
    if (i >= 5) gen1_reads++;
    if (i == 4 && off == 0x74 && (memory[4][0x2c/4] & 1)) {
        link_polls++;
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
    return memory[i][off/4];
}
static void host_write(uintptr_t address, unsigned off, uint32_t value) {
    unsigned i = region_index(address); assert(!(off & 3) && off < 0x4000); powered(i);
    uint32_t before = memory[i][off/4], mask = 0;
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
        if (off == 4 || off == 0x44 || off == 0x80 || off == 0xa0) {
            assert(!(value >> 16));
            if (off == 4) assert(!(value & 0x145));
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
    if (writes != drop_write) memory[i][off/4] = value;
}
'''

post = r'''
static void reset(unsigned which, unsigned failure) {
    maps = unmaps = writes = drop_write = link_polls = 0;
    trained = gen1_reads = log_size = 0;
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
    memory[8][0] = 0x123416c3;
    memory[8][4/4] = 0xdead0144; /* BME, parity and SERR must be cleared before training. */
    memory[8][0x44/4] = 0xbeef8003; /* PME_Status must not be acknowledged by copying it. */
}
static void verify_quiet(void) {
    assert(!(memory[0][0x71c/4] & 1));
    assert(memory[1][0x80/4] == 0 && memory[1][0x84/4] == 1);
    assert(!memory[2][0x20/4] && !memory[2][0x24/4] && !memory[2][0x28/4]);
    assert(!(memory[4][0x2c/4] & 1));
    assert(memory[7][0x1044/4] == 0x8040 && memory[7][0x1050/4] == 0x80000101);
    assert(memory[4][0x18/4] == 0x12 && memory[4][0xf4/4] == 0x20);
    assert(memory[4][0x1b8/4] == 0x80 && memory[4][0x2c8/4] == 0xaaaa0555);
    assert(strstr(logs, "power probe restore verified"));
    if (strstr(logs, "Gen1 probe begin:"))
        assert(!(memory[4][0xc/4] | memory[4][0x10/4] | memory[4][0x14/4]));
    assert(unmaps == 9 && now_us < 1000000 && link_polls <= 500);
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
    for (unsigned fault = 6; fault <= 7; fault++) {
        reset(fault, 0); b2l_wifi_diag_start();
        assert(strstr(logs, "Gen1 quiesce FAILED") && strstr(logs, "power probe restore verified"));
        assert(!(memory[0][0x71c/4] & 1) && !(memory[1][0x84/4] & 32));
        assert(!memory[2][0x20/4] && !(memory[2][0x24/4] & 4));
        assert(now_us < 1000000 && unmaps == 9 && writes < normal_writes + 20);
    }
    puts("Gen1: exact MMIO boundaries, PHY profile, reset/power ordering, no BME/ATU/endpoint access, deadline, dropped writes and quiescence passed");
}
'''

with tempfile.TemporaryDirectory(prefix='b2l-wifi-gen1-') as temporary:
    path = Path(temporary) / 'gen1.c'
    binary = Path(temporary) / 'gen1-test'
    path.write_text(pre + source + post)
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) + [
        '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
        '-fno-omit-frame-pointer', '-I', str(repo / 'xnu-overlay/osfmk/arm64'),
        str(path), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
