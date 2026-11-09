/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "b2l_acpm_layout.h"

/* Independently laid out firmware fixture. Neither offset uses parser macros. */
struct fixture_header {
    uint8_t type, levels, members, pll, mux, div;
    uint16_t gear;
    uint8_t init, gates, reserved[2];
    uint16_t blocks[3], members_offset, rates_offset, tables_offset;
};
_Static_assert(sizeof(struct fixture_header) == 24, "firmware header size");
_Static_assert(offsetof(struct fixture_header, members_offset) == 18, "members ABI");
_Static_assert(offsetof(struct fixture_header, rates_offset) == 20, "rates ABI");

static uint8_t memory[B2L_ACPM_SRAM_BYTES];
static unsigned reads, map_reads, fail_at;
static const uint32_t plugins = 0x7100, name = 0x7311;
static const uint32_t plugin_base = 0x8000, map_base = 0x12400;

static void put32(uint32_t at, uint32_t value)
{
    assert(at <= sizeof(memory) - 4);
    for (unsigned i = 0; i < 4; i++) memory[at + i] = (uint8_t)(value >> (8 * i));
}
static void put16(uint32_t at, uint16_t value)
{
    memory[at] = (uint8_t)value; memory[at + 1] = (uint8_t)(value >> 8);
}
static int read32(void *context, uint32_t at, uint32_t *value)
{
    assert(context == memory);
    assert(!(at & 3) && at <= sizeof(memory) - 4);
    /* No accesses to arbitrary SRAM, including a misleading old-location map. */
    assert((at >= 0x7000 && at < 0x7400) ||
        (at >= map_base && at < map_base + 0x2000) ||
        at == sizeof(memory) - 4);
    reads++;
    if (at >= map_base && at < map_base + 0x2000) map_reads++;
    if (at == fail_at) return -1;
    *value = (uint32_t)memory[at] | ((uint32_t)memory[at + 1] << 8) |
        ((uint32_t)memory[at + 2] << 16) | ((uint32_t)memory[at + 3] << 24);
    return 0;
}
static const struct b2l_acpm_reader reader = { memory, read32 };

static void fixture(void)
{
    memset(memory, 0, sizeof(memory));
    reads = map_reads = 0; fail_at = UINT32_MAX;
    put32(0x7000, plugins); put32(0x7004, 2);
    /* The first plugin is detached. The DVFS plugin is second, not its ID. */
    uint32_t d = plugins + 48;
    put32(d, 7); put32(d + 4, plugin_base | 1);
    put32(d + 32, 1); put32(d + 40, 1); put32(d + 44, name);
    memcpy(memory + name, "exynos-DVFS.bin", 16);
    for (unsigned domain = 2; domain <= 4; domain++) {
        uint32_t h = map_base + domain * 24, table = 0x400 + domain * 0x100;
        memory[h] = (uint8_t)domain; memory[h + 1] = 4; memory[h + 8] = 2;
        /* The old decoder misread this deliberately unaligned member offset. */
        put16(h + 18, 0x181); put16(h + 20, (uint16_t)table);
        for (unsigned l = 0; l < 4; l++) {
            put32(map_base + table + l * 8, 2500000 - l * 500000);
            put32(map_base + table + l * 8 + 4, 900000 - l * 50000);
        }
    }
}

static void no_map(void)
{
    uint32_t map = 123;
    assert(b2l_acpm_find_fvmap(&reader, &map) != 0 && map == 0 && !map_reads);
}
static void bad_rates(void)
{
    struct b2l_acpm_cpu_rates out;
    memset(&out, 0xff, sizeof(out));
    assert(b2l_acpm_read_cpu_rates(&reader, map_base, &out) != 0);
    struct b2l_acpm_cpu_rates zero = {0};
    assert(!memcmp(&out, &zero, sizeof(out)));
}

int main(void)
{
    fixture();
    uint32_t map;
    assert(!b2l_acpm_find_fvmap(&reader, &map) && map == map_base && !map_reads);
    struct b2l_acpm_cpu_rates out;
    assert(!b2l_acpm_read_cpu_rates(&reader, map, &out));
    for (unsigned c = 0; c < 3; c++) {
        assert(out.count[c] == 4);
        for (unsigned l = 0; l < 4; l++) assert(out.khz[c][l] == 1000000 + l * 500000);
    }
    assert(reads < 200); /* Descriptor lookup does not walk the SRAM. */

    fixture(); put32(0x7000, 0); no_map();
    fixture(); put32(0x7000, plugins + 1); no_map();
    fixture(); put32(0x7000, UINT32_MAX - 3); no_map();
    fixture(); put32(0x7000, sizeof(memory) - 4); no_map();
    fixture(); put32(0x7004, 0); no_map();
    fixture(); put32(0x7004, UINT32_MAX); no_map();
    fixture(); fail_at = 0x7000; no_map();
    fixture(); fail_at = plugins + 48 + 4; no_map();
    fixture(); put32(plugins + 48 + 4, 0xfffffffdu); no_map();
    fixture(); put32(plugins + 48 + 4, 0x8003); no_map();
    fixture(); put32(plugins + 48 + 4, 0x28001); no_map();
    fixture(); put32(plugins + 48 + 32, 0); no_map();
    fixture(); put32(plugins + 48 + 40, 0); no_map();
    fixture(); put32(plugins + 48 + 44, UINT32_MAX); no_map();
    fixture(); memset(memory + name, 'x', 64); memcpy(memory + name, "dvfs", 4); no_map();
    fixture(); memory[name] = 1; no_map();
    fixture(); put32(plugins + 48 + 44, sizeof(memory) - 4);
    memcpy(memory + sizeof(memory) - 4, "dvfs", 4); no_map(); /* no terminator */
    fixture(); memcpy(memory + plugins, memory + plugins + 48, 48); no_map(); /* ambiguous */

    for (unsigned offset = 0; offset < 0x10000; offset++) {
        fixture(); put16(map_base + 2 * 24 + 20, (uint16_t)offset);
        if ((offset & 3) || offset < 120 || offset > 0x2000 - 32) bad_rates();
    }
    for (unsigned levels = 0; levels < 256; levels++) {
        if (levels >= 1 && levels <= 24) continue;
        fixture(); memory[map_base + 2 * 24 + 1] = (uint8_t)levels; bad_rates();
    }
    fixture(); memory[map_base + 3 * 24] = 2; bad_rates();
    fixture(); memory[map_base + 4 * 24 + 8] = 4; bad_rates();
    /* A failure in the last domain must discard previously decoded domains. */
    fixture(); put32(map_base + 0x800, 0); bad_rates();
    fixture(); put32(map_base + 0x800 + 8, 2500000); bad_rates();
    fixture(); put32(map_base + 0x800, 4001000); bad_rates();
    fixture(); put32(map_base + 0x800, 2500001); bad_rates();
    fixture(); fail_at = map_base + 0x800; bad_rates();
    fixture(); unsigned before = reads;
    assert(b2l_acpm_read_cpu_rates(&reader, map_base + 1, &out) != 0 && reads == before);
    assert(b2l_acpm_read_cpu_rates(&reader, UINT32_MAX - 3, &out) != 0 && reads == before);
    assert(b2l_acpm_word(&reader, UINT32_MAX, &map) != 0 && reads == before);
    puts("ACPM layout: descriptor-only lookup, independent offset fixture, aligned bounded reads, malformed tables and atomic publication passed");
}
