/* SPDX-License-Identifier: BSD-2-Clause
 * Client for ACPM, the firmware on the APM core that owns clocks, voltages and
 * temperature sensors on the Exynos 9820. Free of kernel types so the host test
 * can run it against a simulated firmware.
 *
 * Channels are queues of fixed-size messages in SRAM, described by a table the
 * firmware leaves there. A request carries a sequence number; the reply carries
 * it back. Requests name a frequency: ACPM chooses the voltage from its own
 * table, so this client never handles one. A reply that does not arrive is
 * reported and nothing else happens.
 */
#ifndef _B2L_ACPM_H
#define _B2L_ACPM_H

#include <stdint.h>

#define B2L_ACPM_MAILBOX        0x15900000ull
#define B2L_ACPM_MAILBOX_BYTES  0x1000
#define B2L_ACPM_SRAM           0x02039000ull
#define B2L_ACPM_SRAM_BYTES     0x30000
#define B2L_ACPM_INITDATA       0x7000

#define B2L_ACPM_INTGR0         0x08    /* ring the APM */
#define B2L_ACPM_INTCR1         0x20    /* clear our pending bit */
#define B2L_ACPM_INTGR1         0x1c    /* set it again */

#define B2L_ACPM_FW_CHANNELS    0x08    /* acpm_framework.ipc_channels */
#define B2L_ACPM_FW_AP_MAX      0x18    /* acpm_framework.ipc_ap_max */
#define B2L_ACPM_CHANNEL_BYTES  72      /* struct ipc_channel */
#define B2L_ACPM_CH_ID          0
#define B2L_ACPM_CH_TX_REAR     16      /* AP to APM: the firmware's rx queue */
#define B2L_ACPM_CH_TX_FRONT    20
#define B2L_ACPM_CH_TX_BASE     24
#define B2L_ACPM_CH_RX_REAR     40      /* APM to AP: the firmware's tx queue */
#define B2L_ACPM_CH_RX_FRONT    44
#define B2L_ACPM_CH_RX_BASE     48
#define B2L_ACPM_CH_LEN         52
#define B2L_ACPM_CH_ELEM        56

#define B2L_ACPM_SEQ_SHIFT      16
#define B2L_ACPM_SEQ_MASK       0x3fu
#define B2L_ACPM_TIMEOUT_US     50000
#define B2L_ACPM_MAX_ELEM       32

/* Channel 5, named acpm_dvfs in the device tree, only tells the AP about MIF. */
#define B2L_ACPM_CHANNEL_DVFS   0
#define B2L_ACPM_CHANNEL_TMU    9
#define B2L_ACPM_FREQ_SET       0
#define B2L_ACPM_FREQ_GET       1
#define B2L_ACPM_TMU_INIT       1
#define B2L_ACPM_TMU_READ       2

/* dvfs_* in order; the stock device tree's dfs ids end in these indices. */
enum {
	B2L_DVFS_MIF, B2L_DVFS_INT, B2L_DVFS_CPUCL0, B2L_DVFS_CPUCL1, B2L_DVFS_CPUCL2, B2L_DVFS_NPU,
	B2L_DVFS_DISP, B2L_DVFS_SCORE, B2L_DVFS_AUD, B2L_DVFS_CP, B2L_DVFS_G3D, B2L_DVFS_COUNT = 15
};
enum { B2L_TMU_BIG, B2L_TMU_MID, B2L_TMU_LITTLE, B2L_TMU_G3D, B2L_TMU_ISP, B2L_TMU_COUNT };

struct b2l_acpm_io {
	void *ctx;
	uint32_t (*sram_read)(void *ctx, uint32_t offset);
	void (*sram_write)(void *ctx, uint32_t offset, uint32_t value);
	void (*mailbox_write)(void *ctx, uint32_t offset, uint32_t value);
	void (*delay_us)(void *ctx, uint32_t us);
};

struct b2l_acpm_channel {
	uint32_t id, tx_rear, tx_front, tx_base, rx_rear, rx_front, rx_base, len, elem, seq;
};

/* Read a channel's descriptor; 0 if it is not what the firmware should leave. */
static inline int
b2l_acpm_open(const struct b2l_acpm_io *io, uint32_t id, struct b2l_acpm_channel *c)
{
	uint32_t fw = B2L_ACPM_INITDATA;
	uint32_t table = io->sram_read(io->ctx, fw + B2L_ACPM_FW_CHANNELS);
	uint32_t count = io->sram_read(io->ctx, fw + B2L_ACPM_FW_AP_MAX);
	uint32_t at = table + id * B2L_ACPM_CHANNEL_BYTES;
	uint32_t limit = B2L_ACPM_SRAM_BYTES - 4;

	if (id >= count || count > 32 || table >= limit || at + B2L_ACPM_CHANNEL_BYTES > limit) {
		return 0;
	}
	c->id = io->sram_read(io->ctx, at + B2L_ACPM_CH_ID);
	c->tx_rear = io->sram_read(io->ctx, at + B2L_ACPM_CH_TX_REAR);
	c->tx_front = io->sram_read(io->ctx, at + B2L_ACPM_CH_TX_FRONT);
	c->tx_base = io->sram_read(io->ctx, at + B2L_ACPM_CH_TX_BASE);
	c->rx_rear = io->sram_read(io->ctx, at + B2L_ACPM_CH_RX_REAR);
	c->rx_front = io->sram_read(io->ctx, at + B2L_ACPM_CH_RX_FRONT);
	c->rx_base = io->sram_read(io->ctx, at + B2L_ACPM_CH_RX_BASE);
	c->len = io->sram_read(io->ctx, at + B2L_ACPM_CH_LEN);
	c->elem = io->sram_read(io->ctx, at + B2L_ACPM_CH_ELEM);
	c->seq = 0;
	if (c->id != id || c->len == 0 || c->len > 64 || c->elem < 16 || c->elem > B2L_ACPM_MAX_ELEM || (c->elem & 3) ||
	    c->tx_rear >= limit || c->tx_front >= limit || c->rx_rear >= limit || c->rx_front >= limit ||
	    c->tx_base + c->len * c->elem > limit || c->rx_base + c->len * c->elem > limit) {
		return 0;
	}
	return 1;
}

/* Send four words and wait for the reply to them. 1 with cmd holding the reply. */
static inline int
b2l_acpm_call(const struct b2l_acpm_io *io, struct b2l_acpm_channel *c, uint32_t *cmd)
{
	uint32_t waited, i, front, rear, next;

	front = io->sram_read(io->ctx, c->tx_front);
	next = (front + 1) % c->len;
	if (front >= c->len || next == io->sram_read(io->ctx, c->tx_rear)) {
		return 0;               /* full, or not a queue at all */
	}
	c->seq = c->seq % 63 + 1;
	cmd[0] = (cmd[0] & ~(B2L_ACPM_SEQ_MASK << B2L_ACPM_SEQ_SHIFT)) | (c->seq << B2L_ACPM_SEQ_SHIFT);
	for (i = 0; i < c->elem / 4; i++) {
		io->sram_write(io->ctx, c->tx_base + front * c->elem + i * 4, i < 4 ? cmd[i] : 0);
	}
	io->sram_write(io->ctx, c->tx_front, next);
	io->mailbox_write(io->ctx, B2L_ACPM_INTGR0, (1u << c->id) << 16);

	for (waited = 0; waited <= B2L_ACPM_TIMEOUT_US; waited += 10) {
		rear = io->sram_read(io->ctx, c->rx_rear);
		front = io->sram_read(io->ctx, c->rx_front);
		if (rear >= c->len || front >= c->len) {
			return 0;
		}
		for (i = rear; i != front; i = (i + 1) % c->len) {
			uint32_t at = c->rx_base + i * c->elem, word = io->sram_read(io->ctx, at), k;

			if (((word >> B2L_ACPM_SEQ_SHIFT) & B2L_ACPM_SEQ_MASK) != c->seq) {
				continue;
			}
			for (k = 0; k < 4; k++) {
				cmd[k] = io->sram_read(io->ctx, at + k * 4);
			}
			/* Take it out: a reply left ahead of it moves into its place. */
			if (i != rear) {
				uint32_t from = c->rx_base + rear * c->elem;
				for (k = 0; k < c->elem / 4; k++) {
					io->sram_write(io->ctx, at + k * 4, io->sram_read(io->ctx, from + k * 4));
				}
			}
			rear = (rear + 1) % c->len;
			io->sram_write(io->ctx, c->rx_rear, rear);
			if (rear == io->sram_read(io->ctx, c->rx_front)) {
				io->mailbox_write(io->ctx, B2L_ACPM_INTCR1, 1u << c->id);
				if (rear != io->sram_read(io->ctx, c->rx_front)) {
					io->mailbox_write(io->ctx, B2L_ACPM_INTGR1, 1u << c->id);
				}
			}
			return 1;
		}
		io->delay_us(io->ctx, 10);
	}
	return 0;
}

/*
 * Ask a DVFS domain for its rate. 1 on a reply, which holds the rate in kHz in
 * reply[1]. Android never asks, so 0 there can be a real answer.
 */
static inline int
b2l_acpm_get_rate(const struct b2l_acpm_io *io, struct b2l_acpm_channel *dvfs, uint32_t domain, uint32_t *reply)
{
	reply[0] = domain;
	reply[1] = 0;
	reply[2] = B2L_ACPM_FREQ_GET;
	reply[3] = 0;
	return b2l_acpm_call(io, dvfs, reply);
}

/* Ask for a rate in kHz. ACPM selects the matching voltage. */
static inline int
b2l_acpm_set_rate(const struct b2l_acpm_io *io, struct b2l_acpm_channel *dvfs, uint32_t domain, uint32_t khz)
{
	uint32_t cmd[4] = { domain, khz, B2L_ACPM_FREQ_SET, 0 };

	return b2l_acpm_call(io, dvfs, cmd);
}

/* Hold at the boot clocks from hot, and let go only at cool: 1 while holding. */
static inline int
b2l_acpm_thermal_hold(int holding, int hottest, int hot, int cool)
{
	if (hottest >= hot) {
		return 1;
	}
	return hottest <= cool ? 0 : holding;
}

/* Rise at once on load, and fall only after a run of quiet samples. */
struct b2l_acpm_load {
	uint32_t quiet;
	int boosted;
};

static inline void
b2l_acpm_load_step(struct b2l_acpm_load *load, uint32_t percent, uint32_t up, uint32_t down, uint32_t quiet_limit)
{
	if (percent >= up) {
		load->boosted = 1;
		load->quiet = 0;
	} else if (percent >= down) {
		load->quiet = 0;
	} else if (++load->quiet >= quiet_limit) {
		load->boosted = 0;
		load->quiet = quiet_limit;
	}
}

/*
 * The big cluster's rate for its temperature: one step per 4 C from 76 C, and a
 * step back up only 3 C below the mark that took it down, so it does not flap.
 * Returns the index into a table that starts with the fastest rate.
 */
static inline unsigned
b2l_acpm_big_step(unsigned current, int temp, int first_c, int per_c, unsigned steps)
{
	unsigned down = 0;

	while (down + 1 < steps && temp >= first_c + (int)down * per_c) {
		down++;
	}
	if (down > current) {
		return down;
	}
	while (current > 0 && temp <= first_c + (int)(current - 1) * per_c - 3) {
		current--;
	}
	return current;
}

/* Android's first TMU request: the firmware starts its sensors. 1 on a reply. */
static inline int
b2l_acpm_tmu_init(const struct b2l_acpm_io *io, struct b2l_acpm_channel *tmu)
{
	uint32_t cmd[4] = { 0, B2L_ACPM_TMU_INIT, 0, 0 };

	return b2l_acpm_call(io, tmu, cmd);
}

/* Sensor temperature in degrees C, or -1. */
static inline int
b2l_acpm_read_temp(const struct b2l_acpm_io *io, struct b2l_acpm_channel *tmu, uint32_t sensor)
{
	uint32_t cmd[4] = { 0, B2L_ACPM_TMU_READ | (sensor << 16), 0, 0 };

	if (!b2l_acpm_call(io, tmu, cmd) || ((cmd[1] >> 8) & 0xff)) {
		return -1;              /* no reply, or the firmware's ret is not 0 */
	}
	return (int)(cmd[1] >> 24);
}

#endif /* _B2L_ACPM_H */
