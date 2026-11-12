/* SPDX-License-Identifier: BSD-2-Clause
 * Mali-G76 (Bifrost v7, TNOx r0p0) hardware layer shared by the kernel driver
 * (B2LMali.cpp), the recovery probe (tools/b2l-mali-probe.c) and the host test
 * (tests/test_mali_core.py). Free of kernel types so all three build it.
 *
 * Three parts: the descriptors of a fragment job that clears a surface, a
 * 4 KiB four-level GPU page table, and bring-up through a register interface.
 * Register offsets and descriptor layouts are the hardware's. Power domain,
 * clock and voltage are outside this file: bring-up needs the GPU block
 * already powered and clocked and never writes the PMU or CMU.
 */
#ifndef _B2L_MALI_CORE_H
#define _B2L_MALI_CORE_H

#include <stdint.h>
#include <stddef.h>

/* GPU_ID of this silicon: arch 7.2.1, product 1, r0p0. */
#define B2L_MALI_GPU_ID            0x72110000u

#define B2L_MALI_ID                0x000
#define B2L_MALI_L2_FEATURES       0x004
#define B2L_MALI_CORE_FEATURES     0x008
#define B2L_MALI_TILER_FEATURES    0x00c
#define B2L_MALI_MEM_FEATURES      0x010
#define B2L_MALI_MMU_FEATURES      0x014
#define B2L_MALI_AS_PRESENT        0x018
#define B2L_MALI_JS_PRESENT        0x01c
#define B2L_MALI_GPU_IRQ_RAWSTAT   0x020
#define B2L_MALI_GPU_IRQ_CLEAR     0x024
#define B2L_MALI_GPU_IRQ_MASK      0x028
#define B2L_MALI_GPU_COMMAND       0x030
#define B2L_MALI_GPU_STATUS        0x034
#define B2L_MALI_GPU_FAULTSTATUS   0x03c
#define B2L_MALI_GPU_FAULTADDRESS  0x040
#define B2L_MALI_CYCLE_COUNT_LO    0x090
#define B2L_MALI_CYCLE_COUNT_HI    0x094
#define B2L_MALI_SHADER_PRESENT    0x100
#define B2L_MALI_TILER_PRESENT     0x110
#define B2L_MALI_L2_PRESENT        0x120
#define B2L_MALI_SHADER_READY      0x140
#define B2L_MALI_TILER_READY       0x150
#define B2L_MALI_L2_READY          0x160
#define B2L_MALI_SHADER_PWRON      0x180
#define B2L_MALI_TILER_PWRON       0x190
#define B2L_MALI_L2_PWRON          0x1a0
#define B2L_MALI_SHADER_PWROFF     0x1c0
#define B2L_MALI_TILER_PWROFF      0x1d0
#define B2L_MALI_L2_PWROFF         0x1e0
#define B2L_MALI_SHADER_PWRTRANS   0x200
#define B2L_MALI_TILER_PWRTRANS    0x210
#define B2L_MALI_L2_PWRTRANS       0x220
#define B2L_MALI_COHERENCY         0x300
#define B2L_MALI_JM_CONFIG         0xf00
#define B2L_MALI_SHADER_CONFIG     0xf04

#define B2L_MALI_CMD_SOFT_RESET    0x01
#define B2L_MALI_CMD_CYCLES_START  0x05
#define B2L_MALI_CMD_CYCLES_STOP   0x06
#define B2L_MALI_IRQ_RESET_DONE    (1u << 8)
#define B2L_MALI_IDVS_GROUP_DEFAULT (0xfu << 16)
#define B2L_MALI_SC_TLS_HASH       (1u << 17)

#define B2L_MALI_JOB_IRQ_RAWSTAT   0x1000
#define B2L_MALI_JOB_IRQ_CLEAR     0x1004
#define B2L_MALI_JOB_IRQ_MASK      0x1008
#define B2L_MALI_JS0               0x1800
#define B2L_MALI_JS_STRIDE         0x80
#define B2L_MALI_SLOT_FRAGMENT     0
#define B2L_MALI_SLOT_TILER        1
#define B2L_MALI_JS_HEAD_LO        0x00
#define B2L_MALI_JS_COMMAND        0x20
#define B2L_MALI_JS_STATUS         0x24
#define B2L_MALI_JS_HEAD_NEXT_LO   0x40
#define B2L_MALI_JS_HEAD_NEXT_HI   0x44
#define B2L_MALI_JS_AFFINITY_NEXT_LO 0x50
#define B2L_MALI_JS_AFFINITY_NEXT_HI 0x54
#define B2L_MALI_JS_CONFIG_NEXT    0x58
#define B2L_MALI_JS_COMMAND_NEXT   0x60
#define B2L_MALI_JS_START          1u
#define B2L_MALI_JS_HARD_STOP      3u
#define B2L_MALI_JS_CONFIG \
	((3u << 8) | (1u << 10) | (3u << 12) | (8u << 16)) /* flush, MMU, priority */
#define B2L_MALI_JOB_DONE(js)      (1u << (js))
#define B2L_MALI_JOB_FAILED(js)    (1u << ((js) + 16))

#define B2L_MALI_MMU_IRQ_RAWSTAT   0x2000
#define B2L_MALI_MMU_IRQ_CLEAR     0x2004
#define B2L_MALI_MMU_IRQ_MASK      0x2008
#define B2L_MALI_AS0               0x2400
#define B2L_MALI_AS_TRANSTAB_LO    0x00
#define B2L_MALI_AS_TRANSTAB_HI    0x04
#define B2L_MALI_AS_MEMATTR_LO     0x08
#define B2L_MALI_AS_MEMATTR_HI     0x0c
#define B2L_MALI_AS_COMMAND        0x18
#define B2L_MALI_AS_FAULTSTATUS    0x1c
#define B2L_MALI_AS_FAULTADDRESS_LO 0x20
#define B2L_MALI_AS_FAULTADDRESS_HI 0x24
#define B2L_MALI_AS_STATUS         0x28
#define B2L_MALI_AS_TRANSCFG_LO    0x30
#define B2L_MALI_AS_TRANSCFG_HI    0x34
#define B2L_MALI_AS_LOCKADDR_LO    0x10
#define B2L_MALI_AS_LOCKADDR_HI    0x14
#define B2L_MALI_AS_CMD_UPDATE     1u
#define B2L_MALI_AS_CMD_LOCK       2u
#define B2L_MALI_AS_CMD_FLUSH_PT   4u
#define B2L_MALI_AS_ACTIVE         1u
/* AArch64 4 KiB walks, cached page-table reads with read-allocate. */
#define B2L_MALI_AS_TRANSCFG       (6ull | (2ull << 24) | (1ull << 30))
#define B2L_MALI_AS_MEMATTR \
	(0x88ull | (0x8full << 8) | (0x8dull << 16) | (0x88ull << 24) | \
	(0x8dull << 32) | (0x4cull << 40))

/* Descriptors, one 4 KiB page: job header + payload, FBD, render target. */
#define B2L_MALI_JOB_BYTES         0x400
#define B2L_MALI_FBD_OFFSET        0x100
#define B2L_MALI_RT_OFFSET         (B2L_MALI_FBD_OFFSET + 128)
#define B2L_MALI_SAMPLES_OFFSET    0x200
#define B2L_MALI_ROW_ALIGN         64u
#define B2L_MALI_JOB_FRAGMENT      9u

static inline void
b2l_mali_clear_job(void *page, uint64_t page_va, uint64_t rt_va, uint32_t width,
    uint32_t height, uint32_t stride, uint32_t colour)
{
	uint32_t *job = (uint32_t *)page;
	uint32_t *fbd = job + B2L_MALI_FBD_OFFSET / 4;
	uint32_t *rtd = job + B2L_MALI_RT_OFFSET / 4;
	uint16_t *samples = (uint16_t *)page + B2L_MALI_SAMPLES_OFFSET / 2;
	/* Bit 0 marks a Bifrost FBD; one render target, no ZS or CRC extension. */
	uint64_t pointer = (page_va + B2L_MALI_FBD_OFFSET) | 1;
	uint64_t samples_va = page_va + B2L_MALI_SAMPLES_OFFSET;
	unsigned i;

	for (i = 0; i < B2L_MALI_JOB_BYTES / 4; i++) {
		job[i] = 0;
	}
	samples[0] = samples[1] = 128;  /* one sample at the pixel centre */
	samples[64] = samples[65] = 128;

	job[4] = 1 | (B2L_MALI_JOB_FRAGMENT << 1) | (1u << 16); /* 64-bit, index 1 */
	job[9] = ((width - 1) >> 4) | (((height - 1) >> 4) << 16); /* tiles */
	job[10] = (uint32_t)pointer;
	job[11] = (uint32_t)(pointer >> 32);

	fbd[4] = (uint32_t)samples_va;
	fbd[5] = (uint32_t)(samples_va >> 32);
	fbd[8] = (width - 1) | ((height - 1) << 16);
	fbd[10] = (width - 1) | ((height - 1) << 16);
	fbd[11] = (2u << 6) | (8u << 9) | (1u << 24); /* tie break, 16x16, 1 KiB */

	rtd[0] = 1u << 26;              /* R8G8B8A8 tile buffer */
	rtd[1] = 1 | (19u << 3) | (2u << 8) | (0x688u << 16) | (1u << 31);
	rtd[8] = (uint32_t)rt_va;
	rtd[9] = (uint32_t)(rt_va >> 32);
	rtd[10] = stride;
	rtd[11] = stride * height;
	for (i = 12; i < 16; i++) {
		rtd[i] = colour;
	}
}

/* GPU page table: `capacity` contiguous 4 KiB tables, the first is the root. */
#define B2L_MALI_PT_ENTRIES        512u
#define B2L_MALI_PT_TABLE          3ull
#define B2L_MALI_PT_PAGE \
	(3ull | (1ull << 6) | (1ull << 10) | (1ull << 54)) /* RW, AF, no execute */
#define B2L_MALI_PT_EXECUTE        (1ull << 54)       /* clear this for shaders */
#define B2L_MALI_PT_ADDRESS        0x0000fffffffff000ull

struct b2l_mali_pt {
	uint64_t *tables;
	uint64_t tables_pa;
	unsigned used, capacity;
};

static inline int
b2l_mali_pt_init(struct b2l_mali_pt *pt, uint64_t *tables, uint64_t tables_pa,
    unsigned capacity)
{
	unsigned i;

	if (!capacity || (tables_pa & 0xfff)) {
		return 0;
	}
	for (i = 0; i < capacity * B2L_MALI_PT_ENTRIES; i++) {
		tables[i] = 0;
	}
	pt->tables = tables;
	pt->tables_pa = tables_pa;
	pt->used = 1;
	pt->capacity = capacity;
	return 1;
}

static inline int
b2l_mali_pt_map_flags(struct b2l_mali_pt *pt, uint64_t va, uint64_t pa, uint64_t flags)
{
	uint64_t *table = pt->tables;
	int level;

	if ((va | pa) & 0xfff || va >> 48 || pa & ~B2L_MALI_PT_ADDRESS) {
		return 0;
	}
	for (level = 0; level < 3; level++) {
		unsigned index = (unsigned)(va >> (39 - 9 * level)) & (B2L_MALI_PT_ENTRIES - 1);
		uint64_t entry = table[index];

		if (!entry) {
			if (pt->used == pt->capacity) {
				return 0;
			}
			entry = (pt->tables_pa + (uint64_t)pt->used * 4096) | B2L_MALI_PT_TABLE;
			pt->used++;
			table[index] = entry;
		}
		table = pt->tables +
		    ((entry & B2L_MALI_PT_ADDRESS) - pt->tables_pa) / sizeof(uint64_t);
	}
	table[(va >> 12) & (B2L_MALI_PT_ENTRIES - 1)] = pa | flags;
	return 1;
}

/* Clear a page entry; the walk tables stay, so the same VA reuses them. */
static inline void
b2l_mali_pt_unmap(struct b2l_mali_pt *pt, uint64_t va)
{
	uint64_t *table = pt->tables;
	int level;

	for (level = 0; level < 3; level++) {
		uint64_t entry = table[(va >> (39 - 9 * level)) & (B2L_MALI_PT_ENTRIES - 1)];

		if (!entry) {
			return;
		}
		table = pt->tables +
		    ((entry & B2L_MALI_PT_ADDRESS) - pt->tables_pa) / sizeof(uint64_t);
	}
	table[(va >> 12) & (B2L_MALI_PT_ENTRIES - 1)] = 0;
}

static inline int
b2l_mali_pt_map(struct b2l_mali_pt *pt, uint64_t va, uint64_t pa)
{
	return b2l_mali_pt_map_flags(pt, va, pa, B2L_MALI_PT_PAGE);
}

static inline int
b2l_mali_pt_map_code(struct b2l_mali_pt *pt, uint64_t va, uint64_t pa)
{
	return b2l_mali_pt_map_flags(pt, va, pa, B2L_MALI_PT_PAGE & ~B2L_MALI_PT_EXECUTE);
}

/* Register access and bounded wait, supplied by the caller. */
struct b2l_mali_io {
	void *ctx;
	uint32_t (*read)(void *ctx, uint32_t offset);
	void (*write)(void *ctx, uint32_t offset, uint32_t value);
	void (*delay_us)(void *ctx, uint32_t us);
};

static inline uint32_t
b2l_mali_r(const struct b2l_mali_io *io, uint32_t offset)
{
	return io->read(io->ctx, offset);
}

static inline void
b2l_mali_w(const struct b2l_mali_io *io, uint32_t offset, uint32_t value)
{
	io->write(io->ctx, offset, value);
}

/* Wait until (register & mask) == value; 0 on timeout. */
static inline int
b2l_mali_wait(const struct b2l_mali_io *io, uint32_t offset, uint32_t mask,
    uint32_t value, uint32_t timeout_us)
{
	uint32_t waited;

	for (waited = 0; waited <= timeout_us; waited += 10) {
		if ((b2l_mali_r(io, offset) & mask) == value) {
			return 1;
		}
		io->delay_us(io->ctx, 10);
	}
	return 0;
}

static inline void
b2l_mali_quiet_irqs(const struct b2l_mali_io *io)
{
	b2l_mali_w(io, B2L_MALI_GPU_IRQ_MASK, 0);
	b2l_mali_w(io, B2L_MALI_JOB_IRQ_MASK, 0);
	b2l_mali_w(io, B2L_MALI_MMU_IRQ_MASK, 0);
	b2l_mali_w(io, B2L_MALI_GPU_IRQ_CLEAR, 0xffffffffu);
	b2l_mali_w(io, B2L_MALI_JOB_IRQ_CLEAR, 0xffffffffu);
	b2l_mali_w(io, B2L_MALI_MMU_IRQ_CLEAR, 0xffffffffu);
}

static inline int
b2l_mali_soft_reset(const struct b2l_mali_io *io)
{
	b2l_mali_quiet_irqs(io);
	b2l_mali_w(io, B2L_MALI_GPU_COMMAND, B2L_MALI_CMD_SOFT_RESET);
	if (!b2l_mali_wait(io, B2L_MALI_GPU_IRQ_RAWSTAT, B2L_MALI_IRQ_RESET_DONE,
	    B2L_MALI_IRQ_RESET_DONE, 100000)) {
		return 0;
	}
	b2l_mali_w(io, B2L_MALI_GPU_IRQ_CLEAR, 0xffffffffu);
	/* The same settings the Android driver applies to tNOx after reset. */
	b2l_mali_w(io, B2L_MALI_SHADER_CONFIG,
	    b2l_mali_r(io, B2L_MALI_SHADER_CONFIG) | B2L_MALI_SC_TLS_HASH);
	b2l_mali_w(io, B2L_MALI_JM_CONFIG,
	    b2l_mali_r(io, B2L_MALI_JM_CONFIG) | B2L_MALI_IDVS_GROUP_DEFAULT);
	return 1;
}

/* L2 first and off last, as the hardware requires. 0 names the block that failed. */
static inline const char *
b2l_mali_power_on(const struct b2l_mali_io *io)
{
	uint32_t l2 = b2l_mali_r(io, B2L_MALI_L2_PRESENT);
	uint32_t tiler = b2l_mali_r(io, B2L_MALI_TILER_PRESENT);
	uint32_t shader = b2l_mali_r(io, B2L_MALI_SHADER_PRESENT);

	b2l_mali_w(io, B2L_MALI_L2_PWRON, l2);
	if (!b2l_mali_wait(io, B2L_MALI_L2_READY, l2, l2, 100000)) {
		return "l2";
	}
	b2l_mali_w(io, B2L_MALI_TILER_PWRON, tiler);
	if (!b2l_mali_wait(io, B2L_MALI_TILER_READY, tiler, tiler, 100000)) {
		return "tiler";
	}
	b2l_mali_w(io, B2L_MALI_SHADER_PWRON, shader);
	if (!b2l_mali_wait(io, B2L_MALI_SHADER_READY, shader, shader, 100000)) {
		return "shader";
	}
	return NULL;
}

static inline void
b2l_mali_power_off(const struct b2l_mali_io *io)
{
	uint32_t shader = b2l_mali_r(io, B2L_MALI_SHADER_READY);
	uint32_t tiler = b2l_mali_r(io, B2L_MALI_TILER_READY);
	uint32_t l2 = b2l_mali_r(io, B2L_MALI_L2_READY);

	b2l_mali_w(io, B2L_MALI_SHADER_PWROFF, shader);
	b2l_mali_wait(io, B2L_MALI_SHADER_PWRTRANS, 0xffffffffu, 0, 100000);
	b2l_mali_w(io, B2L_MALI_TILER_PWROFF, tiler);
	b2l_mali_wait(io, B2L_MALI_TILER_PWRTRANS, 0xffffffffu, 0, 100000);
	b2l_mali_w(io, B2L_MALI_L2_PWROFF, l2);
	b2l_mali_wait(io, B2L_MALI_L2_PWRTRANS, 0xffffffffu, 0, 100000);
}

static inline int
b2l_mali_set_as0(const struct b2l_mali_io *io, uint64_t root_pa)
{
	b2l_mali_w(io, B2L_MALI_AS0 + B2L_MALI_AS_TRANSCFG_LO, (uint32_t)B2L_MALI_AS_TRANSCFG);
	b2l_mali_w(io, B2L_MALI_AS0 + B2L_MALI_AS_TRANSCFG_HI, (uint32_t)(B2L_MALI_AS_TRANSCFG >> 32));
	b2l_mali_w(io, B2L_MALI_AS0 + B2L_MALI_AS_TRANSTAB_LO, (uint32_t)root_pa);
	b2l_mali_w(io, B2L_MALI_AS0 + B2L_MALI_AS_TRANSTAB_HI, (uint32_t)(root_pa >> 32));
	b2l_mali_w(io, B2L_MALI_AS0 + B2L_MALI_AS_MEMATTR_LO, (uint32_t)B2L_MALI_AS_MEMATTR);
	b2l_mali_w(io, B2L_MALI_AS0 + B2L_MALI_AS_MEMATTR_HI, (uint32_t)(B2L_MALI_AS_MEMATTR >> 32));
	b2l_mali_w(io, B2L_MALI_AS0 + B2L_MALI_AS_COMMAND, B2L_MALI_AS_CMD_UPDATE);
	return b2l_mali_wait(io, B2L_MALI_AS0 + B2L_MALI_AS_STATUS, B2L_MALI_AS_ACTIVE, 0, 100000);
}

/*
 * Drop GPU TLB entries for [va, va + 2^log2_bytes), a naturally aligned region,
 * after its page entries changed. The GPU must be idle.
 */
static inline int
b2l_mali_flush_region(const struct b2l_mali_io *io, uint64_t va, unsigned log2_bytes)
{
	uint64_t lock = (va & ~((1ull << log2_bytes) - 1)) | (log2_bytes - 1);

	b2l_mali_w(io, B2L_MALI_AS0 + B2L_MALI_AS_LOCKADDR_LO, (uint32_t)lock);
	b2l_mali_w(io, B2L_MALI_AS0 + B2L_MALI_AS_LOCKADDR_HI, (uint32_t)(lock >> 32));
	b2l_mali_w(io, B2L_MALI_AS0 + B2L_MALI_AS_COMMAND, B2L_MALI_AS_CMD_LOCK);
	if (!b2l_mali_wait(io, B2L_MALI_AS0 + B2L_MALI_AS_STATUS, B2L_MALI_AS_ACTIVE, 0, 100000)) {
		return 0;
	}
	b2l_mali_w(io, B2L_MALI_AS0 + B2L_MALI_AS_COMMAND, B2L_MALI_AS_CMD_FLUSH_PT);
	return b2l_mali_wait(io, B2L_MALI_AS0 + B2L_MALI_AS_STATUS, B2L_MALI_AS_ACTIVE, 0, 100000);
}

/*
 * Run one job chain on a slot in address space 0 and wait for it. Returns the
 * JS_STATUS exception code (1 is done), or 0 after a timeout, in which case the
 * job has been hard-stopped.
 */
static inline uint32_t
b2l_mali_run_job(const struct b2l_mali_io *io, unsigned slot, uint64_t job_va, uint32_t timeout_us)
{
	const uint32_t js = B2L_MALI_JS0 + slot * B2L_MALI_JS_STRIDE;
	const uint32_t finished = B2L_MALI_JOB_DONE(slot) | B2L_MALI_JOB_FAILED(slot);
	uint32_t shader = b2l_mali_r(io, B2L_MALI_SHADER_READY);
	uint32_t status;

	b2l_mali_w(io, B2L_MALI_JOB_IRQ_CLEAR, 0xffffffffu);
	b2l_mali_w(io, js + B2L_MALI_JS_HEAD_NEXT_LO, (uint32_t)job_va);
	b2l_mali_w(io, js + B2L_MALI_JS_HEAD_NEXT_HI, (uint32_t)(job_va >> 32));
	b2l_mali_w(io, js + B2L_MALI_JS_AFFINITY_NEXT_LO, shader);
	b2l_mali_w(io, js + B2L_MALI_JS_AFFINITY_NEXT_HI, 0);
	b2l_mali_w(io, js + B2L_MALI_JS_CONFIG_NEXT, B2L_MALI_JS_CONFIG);
	b2l_mali_w(io, js + B2L_MALI_JS_COMMAND_NEXT, B2L_MALI_JS_START);
	if (b2l_mali_wait(io, B2L_MALI_JOB_IRQ_RAWSTAT, finished, B2L_MALI_JOB_DONE(slot), timeout_us)) {
		return 1;
	}
	if (b2l_mali_r(io, B2L_MALI_JOB_IRQ_RAWSTAT) & finished) {
		status = b2l_mali_r(io, js + B2L_MALI_JS_STATUS);
		return status ? status : 1;
	}
	b2l_mali_w(io, js + B2L_MALI_JS_COMMAND, B2L_MALI_JS_HARD_STOP);
	b2l_mali_wait(io, B2L_MALI_JOB_IRQ_RAWSTAT, finished, finished, 100000);
	return 0;
}

#endif /* _B2L_MALI_CORE_H */
