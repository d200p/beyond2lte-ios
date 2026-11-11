/* SPDX-License-Identifier: BSD-2-Clause
 * MAX77705 fuel gauge / charger readings -> iOS power-source state.
 *
 * Register meanings follow the MAX77705 (ModelGauge m5) register map used by
 * the S10+ (stock device tree: max77705-fuelgauge, fuelgauge,fg_resistor = 2,
 * i.e. a 5 mOhm sense resistor). Only reads are ever performed.
 *   fuel gauge (I2C 0x36): RepCap 0x05, RepSOC 0x06, Temp 0x08, VCell 0x09,
 *     Current 0x0A, AvgCurrent 0x0B, FullCapRep 0x10, TTE 0x11, Cycles 0x17,
 *     DesignCap 0x18, TTF 0x20; 16-bit little endian.
 *   charger (I2C 0x69): CHG_INT_OK 0xB2 (CHGIN_OK bit 6, WCIN_OK bit 5),
 *     CHG_DETAILS_00 0xB3 (CHGIN_DTLS [6:5], WCIN_DTLS [4:3], 3 = valid),
 *     CHG_DETAILS_01 0xB4 (CHG_DTLS [3:0]: 0 prequal, 1 fast CC, 2 fast CV,
 *     3 top-off, 4 done, other values not charging).
 * Kernel and host tests share this header.
 */
#ifndef B2L_BATTERY_MODEL_H
#define B2L_BATTERY_MODEL_H
#include <stdbool.h>
#include <stdint.h>

#define B2L_FG_ADDR          0x36
#define B2L_CHG_ADDR         0x69
#define B2L_FG_REPCAP        0x05
#define B2L_FG_REPSOC        0x06
#define B2L_FG_TEMP          0x08
#define B2L_FG_VCELL         0x09
#define B2L_FG_CURRENT       0x0A
#define B2L_FG_AVGCURRENT    0x0B
#define B2L_FG_FULLCAPREP    0x10
#define B2L_FG_TTE           0x11
#define B2L_FG_CYCLES        0x17
#define B2L_FG_DESIGNCAP     0x18
#define B2L_FG_TTF           0x20
#define B2L_CHG_INT_OK       0xB2
#define B2L_CHG_DETAILS_00   0xB3
#define B2L_CHG_DETAILS_01   0xB4
#define B2L_FG_RESISTOR      2      /* stock fuelgauge,fg_resistor */

struct b2l_battery_raw {
    uint16_t repcap, repsoc, vcell, fullcaprep, designcap, cycles, tte, ttf;
    int16_t current, avgcurrent, temp;
    bool charger_ok;                   /* charger registers were read */
    uint8_t int_ok, details00, details01;
};

struct b2l_battery_state {
    bool valid;                        /* plausible fuel-gauge reading */
    unsigned percent;                  /* 0..100 */
    unsigned millivolts;
    int milliamps, avg_milliamps;      /* positive: charging */
    unsigned remaining_mah, full_mah, design_mah, cycles;
    bool temp_valid;
    int temp_centi_c;
    bool external, charging, full;
    int minutes_to_empty, minutes_to_full;   /* -1: unknown */
};

/* Fuel-gauge time registers: 5.625 s per LSB; 0xFFFF means unknown. */
static inline int
b2l_fg_minutes(uint16_t raw)
{
    if (raw == 0xFFFF || raw == 0) return -1;
    return (int)(((uint32_t)raw * 5625u) / 60000u);
}

/* Current: 1.5625 uV across the sense resistor per LSB (156.25 uA at
 * 10 mOhm), scaled by fg_resistor. Rounded toward zero. */
static inline int
b2l_fg_milliamps(int16_t raw)
{
    return (int)(((int64_t)raw * 15625 * B2L_FG_RESISTOR) / 100000);
}

/* Capacity: 0.5 mAh per LSB at 10 mOhm, scaled by fg_resistor. */
static inline unsigned
b2l_fg_mah(uint16_t raw)
{
    return (unsigned)(((uint32_t)raw * B2L_FG_RESISTOR) / 2u);
}

static inline void
b2l_battery_decode(const struct b2l_battery_raw *r, struct b2l_battery_state *s)
{
    *s = (struct b2l_battery_state){ .minutes_to_empty = -1, .minutes_to_full = -1 };
    unsigned percent = r->repsoc >> 8;                 /* 1/256 % per LSB */
    if ((r->repsoc & 0xFF) >= 0x80 && percent < 100) percent++;
    if (percent > 100) percent = 100;
    s->percent = percent;
    s->millivolts = (unsigned)(((uint32_t)r->vcell * 625u) / 8000u);   /* 78.125 uV */
    s->milliamps = b2l_fg_milliamps(r->current);
    s->avg_milliamps = b2l_fg_milliamps(r->avgcurrent);
    s->remaining_mah = b2l_fg_mah(r->repcap);
    s->full_mah = b2l_fg_mah(r->fullcaprep);
    s->design_mah = b2l_fg_mah(r->designcap);
    s->cycles = r->cycles / 100u;                      /* 1% of a cycle per LSB */
    s->temp_centi_c = (int)(((int32_t)r->temp * 100) / 256);
    s->temp_valid = s->temp_centi_c > -2000 && s->temp_centi_c < 8000;
    s->valid = s->millivolts >= 2500 && s->millivolts <= 4700 && r->repsoc <= (101u << 8);

    if (r->charger_ok) {
        unsigned chgin = (r->details00 >> 5) & 3u, wcin = (r->details00 >> 3) & 3u;
        s->external = (r->int_ok & 0x60u) != 0 || chgin == 3u || wcin == 3u;
        unsigned dtls = r->details01 & 0x0Fu;
        s->full = s->external && (dtls == 4u || s->percent >= 100);
        s->charging = s->external && dtls <= 3u && !s->full;
    } else {
        /* No charger reading: infer from the averaged current. */
        s->external = s->avg_milliamps > 20;
        s->full = false;
        s->charging = s->external;
    }
    if (s->charging) s->minutes_to_full = b2l_fg_minutes(r->ttf);
    else if (!s->external) s->minutes_to_empty = b2l_fg_minutes(r->tte);
}

/* Exynos HSI2C fast-mode (400 kHz) timing for a given input clock, exactly as
 * the platform UEFI HsI2cDxe computes it: fs3[23:16] = div, fs2[7:0] = t,
 * fs1[23:16] = t_minus_1. */
static inline void
b2l_hsi2c_fast_timing(uint32_t clock, uint32_t *div, uint32_t *t, uint32_t *t_minus_1)
{
    uint32_t d = (clock / (400000u * 15u)) & 0xFFu;
    uint32_t base = ((9u * clock) / 1000000u) / ((d + 1u) * 10u);
    *div = d;
    *t = base >= 32 ? 0 : ((0xFFFFFFFFu >> base) << base) & 0xFFu;
    *t_minus_1 = (base == 0 || base > 32) ? 0 : ((0xFFFFFFFFu >> (base - 1)) << (base - 1)) & 0xFFu;
}
#endif
