#!/usr/bin/env python3
"""Run the SRAM parser and actual driver control flow with host MMIO boundaries."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
source = (repo / 'xnu-overlay/osfmk/arm64/b2l_acpm_dvfs.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
start = source.index('static inline uint32_t rd(')
end = source.index('\n/* The descriptor', start)
source = source[:start] + r'''
static inline uint32_t rd(volatile uint8_t *at) {
    uintptr_t address = (uintptr_t)at, low = (uintptr_t)sram_memory;
    assert(!(address & 3));
    assert((address >= low && address + 4 <= low + sizeof(sram_memory)) ||
        (address >= (uintptr_t)intr_memory && address + 4 <= (uintptr_t)intr_memory + sizeof(intr_memory)));
    host_reads++;
    return *(volatile uint32_t *)at;
}
static inline void wr(volatile uint8_t *at, uint32_t value) {
    (void)rd(at); host_writes++; *(volatile uint32_t *)at = value;
}
''' + source[end:]
start = source.index('static uint64_t\nmhz_on_cpu(')
end = source.index('\n/* The parser', start)
source = source[:start] + '''static uint64_t mhz_on_cpu(unsigned cpu) {
    assert(cpu < 8); clock_measures++; return 1377;
}
''' + source[end:]
pre = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "b2l_acpm_layout.h"
#define __unused __attribute__((unused))
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_SEC 1000000000ull
#define KERN_SUCCESS 0
#define PROCESSOR_NULL NULL
#define PROCESSOR_CPU_LOAD_INFO 1
#define PROCESSOR_CPU_LOAD_INFO_COUNT 4
#define CPU_STATE_USER 0
#define CPU_STATE_SYSTEM 1
#define CPU_STATE_NICE 2
#define CPU_STATE_IDLE 3
typedef int kern_return_t, processor_flavor_t, wait_result_t;
typedef void *processor_t;
typedef void *host_t;
typedef int *processor_info_t;
typedef unsigned mach_msg_type_number_t;
typedef struct { uint32_t cpu_ticks[4]; } processor_cpu_load_info_data_t;
typedef void *thread_t;
unsigned real_ncpus = 8, processor_avail_count = 8;
static processor_t processor_array[8];
static uint32_t sram_memory[B2L_ACPM_SRAM_BYTES / 4], intr_memory[0x1000 / 4];
static unsigned host_reads, host_writes, sram_maps, intr_maps, clock_measures, launches;
static int boot_mode = -1;
static uint64_t clock_now;
static void b2l_early_trace(const char *fmt, ...) { (void)fmt; }
static void delay(unsigned n) { clock_now += n * 1000; }
static void delay_for_interval(unsigned n, uint64_t unit) { clock_now += n * unit; }
static uint64_t mach_absolute_time(void) { return ++clock_now; }
static void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *ticks) { *ticks = ns; }
static void clock_interval_to_deadline(unsigned n, uint64_t unit, uint64_t *at) { *at = clock_now + n * unit; }
static uintptr_t ml_io_map(uint64_t address, unsigned size) {
    if (address == 0x02039000) { assert(size == sizeof(sram_memory)); sram_maps++; return (uintptr_t)sram_memory; }
    assert(address == 0x15900000 && size == sizeof(intr_memory));
    intr_maps++; return (uintptr_t)intr_memory;
}
static int PE_parse_boot_argn(const char *name, void *value, unsigned size) {
    assert(size == 4);
    if (strcmp(name, "b2l_dvfs") || boot_mode < 0) return 0;
    *(uint32_t *)value = (unsigned)boot_mode; return 1;
}
kern_return_t processor_info(processor_t p, processor_flavor_t f, host_t *h,
    processor_info_t info, mach_msg_type_number_t *n) {
    (void)p; (void)f; (void)h; (void)info; (void)n; return -1;
}
static int kernel_thread_start(void (*fn)(void *, wait_result_t), void *arg, thread_t *thread) {
    *thread = NULL; launches++; fn(arg, 0); return KERN_SUCCESS;
}
static void thread_deallocate(thread_t thread) { (void)thread; }
'''
post = r'''
static void reset(int requested) {
    memset(sram_memory, 0, sizeof(sram_memory)); memset(intr_memory, 0, sizeof(intr_memory));
    host_reads = host_writes = sram_maps = intr_maps = clock_measures = launches = 0;
    boot_mode = requested; intr = sram = NULL;
}
static void put(uint32_t offset, uint32_t value) { sram_memory[offset / 4] = value; }
static void valid_map(void) {
    put(0x7000, 0x7100); put(0x7004, 1);
    put(0x7104, 0x8001); put(0x7120, 1); put(0x7128, 1); put(0x712c, 0x7300);
    memcpy((uint8_t *)sram_memory + 0x7300, "dvfs", 5);
    for (unsigned domain = 2; domain <= 4; domain++) {
        uint32_t h = 0x12400 + domain * 24;
        put(h, domain | 2 << 8); put(h + 8, 0); put(h + 20, 0x400 + domain * 0x100);
        put(0x12400 + 0x400 + domain * 0x100, 2000000);
        put(0x12400 + 0x408 + domain * 0x100, 1000000);
    }
}
static uint32_t valid_channel(void) {
    sram = (volatile uint8_t *)sram_memory;
    put(0x7008, 0x7400); put(0x7018, 11);
    uint32_t d = 0x7400 + 5 * 72;
    put(d, 5); put(d + 12, 2); put(d + 52, 1); put(d + 56, 16);
    put(d + 16, 0x7900); put(d + 20, 0x7904); put(d + 24, 0x7910);
    put(d + 40, 0x7940); put(d + 44, 0x7944); put(d + 48, 0x7950);
    return d;
}
int main(void) {
    for (int requested = -1; requested <= 3; requested++) {
        if (requested == 1 || requested == 2) continue;
        reset(requested); b2l_acpm_dvfs_start();
        assert(!launches && !host_reads && !host_writes && !sram_maps && !intr_maps);
    }
    for (unsigned valid = 0; valid < 2; valid++) {
        reset(1); if (valid) valid_map(); b2l_acpm_dvfs_start();
        assert(launches == 1 && sram_maps == 1 && host_reads && clock_measures == 3);
        assert(!intr_maps && !host_writes);
        if (valid) for (unsigned c = 0; c < 3; c++) assert(clusters[c].nrates == 2);
    }
    reset(2); b2l_acpm_dvfs_start(); /* invalid tables must stop before mailbox mapping */
    assert(!intr_maps && !host_writes && clock_measures == 3);
    reset(2); valid_map(); b2l_acpm_dvfs_start(); /* missing channel must stop before writes */
    assert(intr_maps == 1 && !host_writes);

    reset(0); uint32_t d = valid_channel();
    struct channel c;
    assert(!find_channel(5, &c));
    put(d + 20, 0x7905); assert(find_channel(5, &c) != 0);
    put(d + 20, sizeof(sram_memory) - 2); assert(find_channel(5, &c) != 0);
    put(d + 20, 0x7904); put(d, 16); assert(find_channel(5, &c) != 0);
    put(d, 5); put(d + 56, 17); assert(find_channel(5, &c) != 0);
    put(d + 56, 16); put(0x7904, 1); assert(find_channel(5, &c) != 0);
    put(0x7904, 0); assert(!find_channel(5, &c));
    uint32_t cmd[4] = {0};
    put(0x7904, UINT32_MAX); assert(ipc(&c, cmd) < 0 && !host_writes);
    put(0x7904, 0); put(0x7900, 1); assert(ipc(&c, cmd) < 0 && !host_writes);
    puts("Actual ACPM driver: disabled path has no MMIO; SRAM-only mode has no mailbox access; malformed tables/channels/indices cannot write");
}
'''
assert 'b2l_dvfs=0 ' in (repo / 'uefi/XnuLoader/XnuLoader.c').read_text()
with tempfile.TemporaryDirectory(prefix='b2l-acpm-') as temporary:
    root = Path(temporary)
    c = root / 'driver.c'
    c.write_text(pre + source + post)
    compiler = shlex.split(os.environ.get('CC', 'clang'))
    for fixture in (repo / 'tests/acpm_layout_test.c', c):
        exe = root / fixture.stem
        subprocess.run(compiler + ['-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                                   '-fsanitize=address,undefined',
                                   '-I' + str(repo / 'xnu-overlay/osfmk/arm64'),
                                   str(fixture), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
