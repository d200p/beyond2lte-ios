/* SPDX-License-Identifier: BSD-2-Clause
 * Power-off and restart on the Exynos 9820, free of kernel types so the host
 * test can run them. The bootloader reads the reason from two PMU registers:
 * INFORM2 bits 23:16 tell it how the system went down, INFORM3 what to boot.
 *
 * Off is the PMIC's: dropping PS_HOLD removes the SoC's power. A held power key
 * would turn the phone straight back on, so it is released first.
 */
#ifndef _B2L_POWER_H
#define _B2L_POWER_H

#include <stdint.h>

#define B2L_PMU_RST_STAT        0x0404
#define B2L_PMU_PS_HOLD         0x030c
#define B2L_PMU_PS_HOLD_OUT     (1u << 8)
#define B2L_PMU_INFORM2         0x0808
#define B2L_PMU_INFORM3         0x080c
#define B2L_PMU_SWRESET         0x3a00
#define B2L_PMU_SWRESET_TRIGGER (1u << 1)
#define B2L_PMU_BYTES           0x4000

#define B2L_REBOOT_SHIFT        16
#define B2L_REBOOT_MASK         (0xffu << B2L_REBOOT_SHIFT)
#define B2L_REBOOT_NORMAL       0x4eu
#define B2L_REBOOT_POWER_OFF    0x70u
#define B2L_RESET_UNKNOWN       0x12345670u
#define B2L_RESET_RECOVERY      0x12345674u

#define B2L_POWER_RELEASE_MS    10000
#define B2L_POWER_SETTLE_MS     5000

struct b2l_power_io {
	void *ctx;
	uint32_t (*read)(void *ctx, uint32_t offset);
	void (*write)(void *ctx, uint32_t offset, uint32_t value);
	int (*power_key_down)(void *ctx);
	void (*delay_ms)(void *ctx, uint32_t ms);
};

static inline void
b2l_power_set_reason(const struct b2l_power_io *io, uint32_t magic, uint32_t reset)
{
	uint32_t inform = io->read(io->ctx, B2L_PMU_INFORM2);

	io->write(io->ctx, B2L_PMU_INFORM2, (inform & ~B2L_REBOOT_MASK) | (magic << B2L_REBOOT_SHIFT));
	io->write(io->ctx, B2L_PMU_INFORM3, reset);
}

static inline void
b2l_power_reset(const struct b2l_power_io *io)
{
	io->write(io->ctx, B2L_PMU_RST_STAT, 0);
	io->write(io->ctx, B2L_PMU_SWRESET, B2L_PMU_SWRESET_TRIGGER);
}

static inline void
b2l_power_restart(const struct b2l_power_io *io, int recovery)
{
	b2l_power_set_reason(io, B2L_REBOOT_NORMAL, recovery ? B2L_RESET_RECOVERY : B2L_RESET_UNKNOWN);
	b2l_power_reset(io);
}

/*
 * Returns only if power stayed on, which a connected charger can cause; the
 * caller then restarts. A key still held after the limit is not waited for.
 */
static inline void
b2l_power_off(const struct b2l_power_io *io)
{
	uint32_t waited;

	b2l_power_set_reason(io, B2L_REBOOT_POWER_OFF, B2L_RESET_UNKNOWN);
	for (waited = 0; waited < B2L_POWER_RELEASE_MS && io->power_key_down(io->ctx); waited += 100) {
		io->delay_ms(io->ctx, 100);
	}
	io->write(io->ctx, B2L_PMU_PS_HOLD, io->read(io->ctx, B2L_PMU_PS_HOLD) & ~B2L_PMU_PS_HOLD_OUT);
	io->delay_ms(io->ctx, B2L_POWER_SETTLE_MS);
}

#endif /* _B2L_POWER_H */
