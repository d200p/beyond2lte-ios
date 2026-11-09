/* SPDX-License-Identifier: BSD-2-Clause
 * Bounded decoding of the Exynos 9820 ACPM SRAM ABI, using aligned words only.
 * Layout references and limitations: docs/acpm-diagnostics.md.
 */
#ifndef B2L_ACPM_LAYOUT_H
#define B2L_ACPM_LAYOUT_H

#include <stdint.h>

#define B2L_ACPM_SRAM_BYTES 0x30000u
#define B2L_ACPM_INITDATA 0x7000u
#define B2L_ACPM_FVMAP_OFFSET 0xa400u
#define B2L_ACPM_FVMAP_BYTES 0x2000u
#define B2L_ACPM_MAX_RATES 24u
#define B2L_ACPM_PLUGIN_BYTES 48u

struct b2l_acpm_reader {
    void *context;
    /* Offset from SRAM, 4-byte aligned and bounded before every callback.
     * Return zero and a little-endian word on success. No writes are needed. */
    int (*read32)(void *, uint32_t, uint32_t *);
};

struct b2l_acpm_cpu_rates {
    unsigned count[3];
    uint32_t khz[3][B2L_ACPM_MAX_RATES]; /* ascending, domains 2, 3, 4 */
};

static inline int
b2l_acpm_range(uint32_t offset, uint32_t bytes)
{
    return offset <= B2L_ACPM_SRAM_BYTES && bytes <= B2L_ACPM_SRAM_BYTES - offset;
}

static inline int
b2l_acpm_word(const struct b2l_acpm_reader *r, uint32_t offset, uint32_t *word)
{
    if ((offset & 3) || !b2l_acpm_range(offset, 4)) return -1;
    return r->read32(r->context, offset, word);
}

/* Firmware names are byte strings, but the AP accesses SRAM as aligned words.
 * Require a printable, terminated name within 64 bytes, as well as "dvfs". */
static inline int
b2l_acpm_dvfs_name(const struct b2l_acpm_reader *r, uint32_t name)
{
    uint32_t tail = 0, word;
    unsigned found = 0;
    if (!name || !b2l_acpm_range(name, 1)) return 0;
    for (unsigned i = 0; i < 64; i++) {
        if (b2l_acpm_word(r, (name + i) & ~3u, &word)) return 0;
        unsigned c = (word >> (((name + i) & 3) * 8)) & 0xff;
        if (!c) return (int)found;
        if (c < 0x20 || c > 0x7e) return 0;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        tail = (tail << 8) | c;
        if (tail == 0x64766673u) found = 1; /* dvfs */
    }
    return 0;
}

/* Follow the framework's attached DVFS plugin, as Samsung's acpm.c does.
 * No address search and no fallback to SRAM+fvmap_offset. Ambiguity fails. */
static inline int
b2l_acpm_find_fvmap(const struct b2l_acpm_reader *r, uint32_t *map)
{
    uint32_t plugins, count, candidate = 0;
    unsigned found = 0;
    *map = 0;
    if (b2l_acpm_word(r, B2L_ACPM_INITDATA, &plugins) ||
        b2l_acpm_word(r, B2L_ACPM_INITDATA + 4, &count) ||
        !plugins || (plugins & 3) || !count || count > 32 ||
        !b2l_acpm_range(plugins, count * B2L_ACPM_PLUGIN_BYTES)) return -1;
    for (unsigned i = 0; i < count; i++) {
        uint32_t descriptor = plugins + i * B2L_ACPM_PLUGIN_BYTES;
        uint32_t attached, stay, base, name;
        if (b2l_acpm_word(r, descriptor + 32, &attached) ||
            b2l_acpm_word(r, descriptor + 40, &stay)) return -1;
        if ((attached & 0xff) != 1 || (stay & 0xff) != 1) continue;
        if (b2l_acpm_word(r, descriptor + 44, &name)) return -1;
        if (!b2l_acpm_dvfs_name(r, name)) continue;
        if (b2l_acpm_word(r, descriptor + 4, &base)) return -1;
        base &= ~1u; /* Firmware entrypoint has the Thumb bit set. */
        if (!base || (base & 3) || !b2l_acpm_range(base, B2L_ACPM_FVMAP_OFFSET) ||
            !b2l_acpm_range(base + B2L_ACPM_FVMAP_OFFSET, B2L_ACPM_FVMAP_BYTES)) return -1;
        candidate = base + B2L_ACPM_FVMAP_OFFSET;
        if (++found > 1) return -1;
    }
    if (!found) return -1;
    *map = candidate;
    return 0;
}

/* 24-byte FVMAP headers: o_members is at 18, o_ratevolt at 20.
 * Validate all three CPU tables before publishing any frequencies. */
static inline int
b2l_acpm_read_cpu_rates(const struct b2l_acpm_reader *r, uint32_t map,
    struct b2l_acpm_cpu_rates *out)
{
    struct b2l_acpm_cpu_rates decoded = {0};
    *out = decoded;
    if ((map & 3) || !b2l_acpm_range(map, B2L_ACPM_FVMAP_BYTES)) return -1;
    for (unsigned cpu = 0; cpu < 3; cpu++) {
        unsigned domain = cpu + 2;
        uint32_t header = map + domain * 24, first, init, offsets;
        if (b2l_acpm_word(r, header, &first) ||
            b2l_acpm_word(r, header + 8, &init) ||
            b2l_acpm_word(r, header + 20, &offsets)) return -1;
        unsigned levels = (first >> 8) & 0xff;
        uint32_t ratevolt = offsets & 0xffff;
        if ((first & 0xff) != domain || !levels || levels > B2L_ACPM_MAX_RATES ||
            (init & 0xff) >= levels || (ratevolt & 3) || ratevolt < 5 * 24 ||
            ratevolt > B2L_ACPM_FVMAP_BYTES - levels * 8) return -1;
        uint32_t previous = UINT32_MAX;
        for (unsigned l = 0; l < levels; l++) {
            uint32_t rate;
            if (b2l_acpm_word(r, map + ratevolt + l * 8, &rate) ||
                !rate || rate > 4000000 || rate >= previous || rate % 1000) return -1;
            decoded.khz[cpu][levels - 1 - l] = rate;
            previous = rate;
        }
        decoded.count[cpu] = levels;
    }
    *out = decoded;
    return 0;
}

#endif
