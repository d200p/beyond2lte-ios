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
    if (at == intr + INTCR1) intr_memory[INTSR1 / 4] &= ~value;
    if (at == intr + INTGR0) host_mailbox_command(value);
}
''' + source[end:]
start = source.index('static uint64_t\nmhz_on_cpu(')
end = source.index('\n/* The parser', start)
source = source[:start] + '''static uint64_t mhz_on_cpu(unsigned cpu) {
    assert(cpu < 8); clock_measures++; return host_mhz[cpu < 4 ? 0 : cpu < 6 ? 1 : 2];
}
''' + source[end:]
pre = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include "b2l_acpm_layout.h"
#ifndef __unused
#define __unused __attribute__((unused))
#endif
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
static uint64_t host_mhz[3];
static uint32_t host_caps[3];
static int host_temp[4], host_hold_clock, host_drop_after, host_hot_after;
static unsigned host_runtime_ticks, host_stop_after, host_request_count;
static struct { unsigned domain; uint32_t khz; } host_requests[32];
static jmp_buf host_stop;
static void host_mailbox_command(uint32_t mask);
static void b2l_early_trace(const char *fmt, ...) { (void)fmt; }
static void delay(unsigned n) { clock_now += n * 1000; }
static void delay_for_interval(unsigned n, uint64_t unit) {
    clock_now += n * unit;
    if (n == 200 && unit == NSEC_PER_MSEC && ++host_runtime_ticks > host_stop_after)
        longjmp(host_stop, 1);
}
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
    const char *caps[] = {"b2l_dvfs_cl0", "b2l_dvfs_cl1", "b2l_dvfs_cl2"};
    for (unsigned c = 0; c < 3; c++) if (!strcmp(name, caps[c])) {
        *(uint32_t *)value = host_caps[c]; return 1;
    }
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
    host_mhz[0] = 1050; host_mhz[1] = 1221; host_mhz[2] = 1376;
    for (unsigned c = 0; c < 3; c++) {
        host_caps[c] = 0; clusters[c].nrates = clusters[c].attempted = clusters[c].sent = 0;
    }
    for (unsigned z = 0; z < 4; z++) host_temp[z] = 50;
    host_hold_clock = host_drop_after = host_hot_after = -1;
    host_runtime_ticks = host_request_count = 0; host_stop_after = 2;
}
static void put(uint32_t offset, uint32_t value) { sram_memory[offset / 4] = value; }
static void valid_map(void) {
    put(0x7000, 0x7100); put(0x7004, 1);
    put(0x7104, 0x8001); put(0x7120, 1); put(0x7128, 1); put(0x712c, 0x7300);
    memcpy((uint8_t *)sram_memory + 0x7300, "dvfs", 5);
    for (unsigned domain = 2; domain <= 4; domain++) {
        uint32_t h = 0x12400 + domain * 24;
        put(h, domain | 2 << 8); put(h + 8, 0); put(h + 20, 0x400 + domain * 0x100);
        const uint32_t tops[] = {1950000, 2002000, 2080000}, baselines[] = {1053000, 1222000, 1378000};
        put(0x12400 + 0x400 + domain * 0x100, tops[domain - 2]);
        put(0x12400 + 0x408 + domain * 0x100, baselines[domain - 2]);
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
static void valid_channels(void) {
    valid_channel();
    uint32_t d = 0x7400 + 9 * 72;
    put(d, 9); put(d + 12, 1); put(d + 52, 2); put(d + 56, 16);
    put(d + 16, 0x7a00); put(d + 20, 0x7a04); put(d + 24, 0x7a10);
    put(d + 40, 0x7a40); put(d + 44, 0x7a44); put(d + 48, 0x7a50);
}
static void host_mailbox_command(uint32_t mask) {
    if (mask == (1u << 5) << 16) {
        volatile uint32_t *tx = (volatile uint32_t *)ch_dvfs.tx_base;
        unsigned c = (tx[0] & 0xffff) - 2;
        assert(c < 3 && host_request_count < 32);
        host_requests[host_request_count].domain = c + 2;
        host_requests[host_request_count++].khz = tx[1];
        if ((int)c != host_hold_clock) host_mhz[c] = tx[1] / 1000;
        ((volatile uint32_t *)ch_dvfs.rx_base)[0] = tx[0];
        intr_memory[INTSR1 / 4] |= 1u << 5;
    } else {
        assert(mask == (1u << 9) << 16);
        uint32_t front = *(volatile uint32_t *)ch_tmu.tx_front;
        uint32_t previous = front ? front - 1 : ch_tmu.len - 1;
        volatile uint32_t *tx = (volatile uint32_t *)(ch_tmu.tx_base + previous * ch_tmu.elem);
        uint32_t rx_front = *(volatile uint32_t *)ch_tmu.rx_front;
        volatile uint32_t *rx = (volatile uint32_t *)(ch_tmu.rx_base + rx_front * ch_tmu.elem);
        rx[0] = tx[0]; rx[1] = 0;
        if ((tx[1] & 0xff) == TMU_IPC_READ_TEMP) {
            unsigned z = tx[1] >> 16; assert(z < 4);
            int temperature = host_temp[z];
            if (host_drop_after >= 0 && host_runtime_ticks >= (unsigned)host_drop_after && z == 1) temperature = -1;
            if (host_hot_after >= 0 && host_runtime_ticks >= (unsigned)host_hot_after) temperature = 90;
            rx[1] = temperature < 0 ? 0xff00 : (uint32_t)temperature << 24;
        }
        *(volatile uint32_t *)ch_tmu.tx_rear = front;
        *(volatile uint32_t *)ch_tmu.rx_front = (rx_front + 1) % ch_tmu.len;
        intr_memory[INTSR1 / 4] |= 1u << 9;
    }
}
static void run_boot(void) { if (!setjmp(host_stop)) b2l_acpm_dvfs_start(); }
static void full_fixture(void) { reset(2); valid_map(); valid_channels(); }
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

    full_fixture(); run_boot();
    assert(mode == 2 && host_request_count == 3);
    const uint32_t target[] = {1950000, 2002000, 2080000};
    const uint32_t baseline[] = {1053000, 1222000, 1378000};
    for (unsigned c = 0; c < 3; c++) {
        assert(host_requests[c].domain == c + 2 && host_requests[c].khz == target[c]);
        assert(clusters[c].sent == target[c]);
        for (unsigned r = 0; r < clusters[c].nrungs; r++)
            assert(clusters[c].rung[r] >= baseline[c] && clusters[c].rung[r] <= target[c]);
    }
    full_fixture(); host_temp[1] = -1; run_boot(); assert(!host_request_count && mode == 1);
    full_fixture(); host_temp[0] = 80; run_boot(); assert(!host_request_count && mode == 1);
    full_fixture(); host_hold_clock = 1; run_boot();
    assert(host_request_count == 4 && mode == 1);
    assert(host_requests[2].domain == 3 && host_requests[2].khz == baseline[1]);
    assert(host_requests[3].domain == 2 && host_requests[3].khz == baseline[0]);
    full_fixture(); host_drop_after = 1; run_boot(); assert(mode == 1 && host_request_count == 6);
    for (unsigned c = 0; c < 3; c++) assert(clusters[c].sent == baseline[c]);
    full_fixture(); host_hot_after = 1; run_boot(); assert(mode == 2 && host_request_count == 6);
    for (unsigned c = 0; c < 3; c++) assert(clusters[c].level == 2 && clusters[c].sent == baseline[c]);
    assert(thermal_level(80, 0) == 1 && thermal_level(90, 0) == 2);
    assert(thermal_level(84, 2) == 2 && thermal_level(83, 2) == 1);
    assert(thermal_level(74, 1) == 1 && thermal_level(73, 1) == 0);
    full_fixture(); assert(!read_fvmap());
    for (unsigned c = 0; c < 3; c++) {
        struct cluster *k = &clusters[c]; k->mhz_before = host_mhz[c];
        k->rates[k->nrates++] = c == 2 ? 3016000 : 2400000;
        host_caps[c] = UINT32_MAX; assert(!plan(k) && k->target == target[c]);
        k->mhz_before = 0; assert(plan(k) != 0);
    }
    assert(rate_matches(2080000, 2080) && !rate_matches(2080000, 1377) && !rate_matches(2080000, 0));
    puts("Actual ACPM driver: disabled path has no MMIO; SRAM-only mode has no mailbox access; malformed tables/channels/indices cannot write");
    puts("ACPM control: board ceilings, observed boot floors, measured requests, startup heat/sensor rejection, reverse restoration and runtime thermal failure passed");
}
'''
loader = (repo / 'uefi/XnuLoader/XnuLoader.c').read_text()
# S10 GPU build: B2LMali owns the ACPM mailbox, so this governor never writes
# it: mode 1 (tables and measured clocks only) at most.
assert 'b2l_dvfs=1 ' in loader and 'b2l_dvfs=2' not in loader and 'b2l_dvfs_cl' not in loader
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
