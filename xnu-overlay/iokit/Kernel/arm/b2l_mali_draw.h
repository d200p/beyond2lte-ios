/* SPDX-License-Identifier: BSD-2-Clause
 * One render pass on the Mali-G76: a chain of tiler jobs, one per quad, then a
 * fragment job that resolves them into a surface.
 *
 * There is no vertex shader. The CPU writes window-space positions and
 * varyings straight into the buffers the tiler and fragment stages read, so
 * the only GPU programs are the fragment shaders in b2l_mali_shaders.h.
 * A textured shader runs through the RSD message preload: the hardware loads
 * the varying and the texel into r0..r3 before the first clause.
 *
 * Coordinates and colours are IEEE 754 bit patterns rather than floats, so the
 * kernel, which has no floating point, can build the same passes.
 */
#ifndef _B2L_MALI_DRAW_H
#define _B2L_MALI_DRAW_H

#include <stdint.h>
#include "b2l_mali_core.h"
#include "b2l_mali_shaders.h"

/*
 * A pass is a page-aligned block: shared state, the fragment job, then one
 * record per draw. Draw records hold everything that differs between layers.
 */
#define B2L_MALI_PASS_FRAGMENT  0x1000  /* fragment job, FBD and render target */
#define B2L_MALI_PASS_DRAWS     0x2000
#define B2L_MALI_DRAW_BYTES     0x400
#define B2L_MALI_PASS_BYTES(n)  (B2L_MALI_PASS_DRAWS + (n) * B2L_MALI_DRAW_BYTES)
#define B2L_F_VIEWPORT          0x100
#define B2L_F_TLS               0x140
#define B2L_F_TILER             0x280
#define B2L_F_HEAP              0x380
/* Offsets inside a draw record. */
#define B2L_D_POSITIONS         0x100
#define B2L_D_PUSH              0x140
#define B2L_D_RSD               0x180
#define B2L_D_BLEND             0x1c0
#define B2L_D_VARYING           0x200
#define B2L_D_VARYING_BUF       0x240
#define B2L_D_UV                0x280
#define B2L_D_TEXTURE           0x2c0
#define B2L_D_SURFACE           0x300
#define B2L_D_SAMPLER           0x340

#define B2L_JOB_TILER           7u
#define B2L_DRAW_STRIP          10u
#define B2L_FUNC_ALWAYS         7u
#define B2L_FORMAT_RGBA8_UNORM  (187u << 12)
#define B2L_FORMAT_BGRA8_UNORM  ((187u << 12) | 4u)
#define B2L_FORMAT_RGBA8_UNORM  (187u << 12)
#define B2L_FORMAT_RGBA16_FLOAT (223u << 12)
#define B2L_FORMAT_R8_UNORM     (163u << 12)
#define B2L_FORMAT_RG32F        ((175u << 12) | 16u)
#define B2L_FORMAT_RGBA8_TB     (237u << 12)
#define B2L_WRAP_CLAMP_EDGE     9u
/* (A op B) * C equations: A + B * C, for the colour and alpha channels. */
#define B2L_EQ_COPY             0xf0132132u     /* src */
#define B2L_EQ_SRC_OVER         0xf0d32d32u     /* src + dest * (1 - src.a) */
#define B2L_EQ_ADD              0xf0932932u     /* src + dest */

struct b2l_mali_quad {
	uint32_t x0, y0, x1, y1;        /* window pixels */
	uint32_t u0, v0, u1, v1;        /* normalized texture coordinates */
	uint32_t colour[4];             /* push uniforms */
	uint64_t shader_va, texture_va;
	uint32_t texture_width, texture_height, texture_stride, texture_format;
	uint32_t equation;
	uint32_t fau_slots;             /* 64-bit push uniform slots the shader reads */
	uint32_t first_clause_blends;   /* the shader is only the BLEND clause */
	uint32_t linear;                /* filter texels, for scaled layers */
	uint32_t texture_mask;          /* one channel, read into all four */
};

struct b2l_mali_target {
	uint64_t va;
	uint32_t width, height, stride, clear, swizzle;
	uint64_t heap_va;
	uint32_t heap_bytes;
};

/* Render target swizzles: the inverse of the memory component order. */
#define B2L_RT_RGBA             0x688u
#define B2L_RT_BGRA             0x60au

struct b2l_mali_pass {
	uint8_t *cpu;
	uint64_t va;
	unsigned draws;
};

static inline void
b2l_put64(uint32_t *w, uint64_t v)
{
	w[0] = (uint32_t)v;
	w[1] = (uint32_t)(v >> 32);
}

#define B2L_FLOAT_ONE           0x3f800000u
#define B2L_FLOAT_HALF          0x3f000000u
#define B2L_FLOAT_INF           0x7f800000u
#define B2L_FLOAT_MINUS_INF     0xff800000u

/* An integer below 2^24 as a float's bit pattern. */
static inline uint32_t
b2l_int_float(uint32_t v)
{
	uint32_t exponent = 23;

	if (!v) {
		return 0;
	}
	while (!(v >> exponent)) {
		exponent--;
	}
	return ((127 + exponent) << 23) | ((v << (23 - exponent)) & 0x7fffffu);
}

static inline void
b2l_mali_write_rsd(uint32_t *rsd, uint32_t *blend, const struct b2l_mali_quad *q)
{
	int textured = q->texture_va != 0;
	uint32_t reads_dest = q->equation != B2L_EQ_COPY;

	b2l_put64(rsd, q->shader_va);
	rsd[2] = textured ? (1u | (1u << 16)) : 0;      /* one sampler, one texture */
	rsd[3] = textured ? (1u << 16) : 0;             /* one varying */
	rsd[4] = (2u << 8) | (2u << 12) | (1u << 20);   /* FF depth, 32 regs, killable */
	if (q->first_clause_blends) {
		rsd[4] |= (1u << 30) | (1u << 31);      /* wait on the tile buffer */
	}
	rsd[8] = 0xffffu | (B2L_FUNC_ALWAYS << 24);     /* sample mask, depth func */
	rsd[9] = 0xffffu | (B2L_FUNC_ALWAYS << 21);     /* stencil masks, alpha test */
	rsd[10] = rsd[11] = 0xffu << 8 | (B2L_FUNC_ALWAYS << 16);
	rsd[12] = (1u << 7) | (q->fau_slots << 15);     /* coverage, push uniforms */
	if (textured) {
		rsd[15] = 2u | (1u << 15);              /* VAR_TEX v0/t0 F32, LOD 0 */
	}

	blend[0] = (1u << 9) | reads_dest;              /* enable, load destination */
	blend[1] = q->equation;
	blend[2] = 2u | (3u << 3);                      /* fixed function, 4 comps */
	blend[3] = B2L_FORMAT_RGBA8_TB | (1u << 24);    /* F32 registers */
}

static inline void
b2l_mali_write_texture(uint8_t *f, uint64_t va, const struct b2l_mali_quad *q)
{
	uint32_t *t = (uint32_t *)(f + B2L_D_TEXTURE);
	uint32_t *surface = (uint32_t *)(f + B2L_D_SURFACE);
	uint32_t *s = (uint32_t *)(f + B2L_D_SAMPLER);
	uint32_t *attribute = (uint32_t *)(f + B2L_D_VARYING);
	uint32_t *buffer = (uint32_t *)(f + B2L_D_VARYING_BUF);
	uint32_t *uv = (uint32_t *)(f + B2L_D_UV);

	t[0] = 2u | (2u << 4) | (q->texture_format << 10);
	t[1] = (q->texture_width - 1) | ((q->texture_height - 1) << 16);
	t[2] = (q->texture_mask ? 0u : 0x688u) | (2u << 12);    /* RRRR or identity, linear */
	b2l_put64(t + 4, va + B2L_D_SURFACE);
	b2l_put64(surface, q->texture_va);
	surface[2] = q->texture_stride;
	surface[3] = q->texture_stride * q->texture_height;

	/* Clamp all axes, normalized; nearest copies pixels one to one. */
	s[0] = 1u | (B2L_WRAP_CLAMP_EDGE << 8) | (B2L_WRAP_CLAMP_EDGE << 12) |
	    (B2L_WRAP_CLAMP_EDGE << 16) | (1u << 21) | (1u << 23) | (1u << 25) |
	    (1u << 26);
	if (!q->linear) {
		s[0] |= (1u << 27) | (1u << 28);
	}

	attribute[0] = (1u << 9) | (B2L_FORMAT_RG32F << 10);
	b2l_put64(buffer, (va + B2L_D_UV) | 1u);        /* 1D */
	buffer[2] = 2 * sizeof(uint32_t);
	buffer[3] = 8 * sizeof(uint32_t);
	uv[0] = q->u0; uv[1] = q->v0;
	uv[2] = q->u1; uv[3] = q->v0;
	uv[4] = q->u0; uv[5] = q->v1;
	uv[6] = q->u1; uv[7] = q->v1;
}

/* Start a pass: shared state and the fragment job. */
static inline void
b2l_mali_pass_begin(struct b2l_mali_pass *pass, void *memory, uint64_t va,
    const struct b2l_mali_target *rt)
{
	uint8_t *f = (uint8_t *)memory;
	uint32_t *fbd = (uint32_t *)(f + B2L_MALI_PASS_FRAGMENT + B2L_MALI_FBD_OFFSET);
	uint32_t *rtd = (uint32_t *)(f + B2L_MALI_PASS_FRAGMENT + B2L_MALI_RT_OFFSET);
	uint32_t *viewport = (uint32_t *)(f + B2L_F_VIEWPORT);
	uint32_t *tiler = (uint32_t *)(f + B2L_F_TILER);
	uint32_t *heap = (uint32_t *)(f + B2L_F_HEAP);
	unsigned i;

	for (i = 0; i < B2L_MALI_PASS_FRAGMENT / 4; i++) {
		((uint32_t *)f)[i] = 0;
	}
	b2l_mali_clear_job(f + B2L_MALI_PASS_FRAGMENT, va + B2L_MALI_PASS_FRAGMENT, rt->va,
	    rt->width, rt->height, rt->stride, rt->clear);
	b2l_put64(fbd + 14, va + B2L_F_TILER);
	rtd[1] = (rtd[1] & ~(0xfffu << 16)) | (rt->swizzle << 16);

	viewport[0] = viewport[1] = B2L_FLOAT_MINUS_INF;
	viewport[2] = viewport[3] = B2L_FLOAT_INF;
	viewport[5] = B2L_FLOAT_ONE;
	viewport[7] = (rt->width - 1) | ((rt->height - 1) << 16);
	((uint32_t *)(f + B2L_F_TLS))[1] = 31;          /* no workgroup memory */

	tiler[2] = 0xff;                                /* hierarchy mask */
	tiler[3] = (rt->width - 1) | ((rt->height - 1) << 16);
	b2l_put64(tiler + 6, va + B2L_F_HEAP);
	heap[1] = rt->heap_bytes;
	b2l_put64(heap + 2, rt->heap_va);
	b2l_put64(heap + 4, rt->heap_va);
	b2l_put64(heap + 6, rt->heap_va + rt->heap_bytes);

	pass->cpu = f;
	pass->va = va;
	pass->draws = 0;
}

/* Limit the fragment job to these tiles; pixels outside them are not touched. */
static inline void
b2l_mali_pass_bounds(struct b2l_mali_pass *pass, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
	uint32_t *job = (uint32_t *)(pass->cpu + B2L_MALI_PASS_FRAGMENT);

	job[8] = (x0 >> 4) | ((y0 >> 4) << 16);
	job[9] = ((x1 - 1) >> 4) | (((y1 - 1) >> 4) << 16);
}

/* Append a draw. Tiler jobs must run in order, so each depends on the last. */
static inline void
b2l_mali_pass_add(struct b2l_mali_pass *pass, const struct b2l_mali_quad *q)
{
	unsigned index = pass->draws++;
	uint8_t *f = pass->cpu + B2L_MALI_PASS_DRAWS + index * B2L_MALI_DRAW_BYTES;
	uint64_t va = pass->va + B2L_MALI_PASS_DRAWS + index * (uint64_t)B2L_MALI_DRAW_BYTES;
	uint32_t *job = (uint32_t *)f;
	uint32_t *draw = job + 32;
	uint32_t *position = (uint32_t *)(f + B2L_D_POSITIONS);
	unsigned i;

	for (i = 0; i < B2L_MALI_DRAW_BYTES / 4; i++) {
		job[i] = 0;
	}
	job[4] = 1 | (B2L_JOB_TILER << 1) | ((index + 1) << 16);
	job[5] = index;                                 /* previous job, 0 for none */
	job[8] = 3;                                     /* four vertices */
	job[9] = (32u << 22) | (2u << 28);
	job[10] = B2L_DRAW_STRIP | (1u << 15) | (3u << 16) | (6u << 26);
	job[13] = 3;
	job[16] = B2L_FLOAT_ONE;                        /* line width */
	b2l_put64(job + 18, pass->va + B2L_F_TILER);

	draw[0] = 3;                                    /* vec4 positions, 64-bit */
	b2l_put64(draw + 4, va + B2L_D_POSITIONS);
	b2l_put64(draw + 12, va + B2L_D_PUSH);
	b2l_put64(draw + 14, va + B2L_D_RSD);
	b2l_put64(draw + 24, pass->va + B2L_F_VIEWPORT);
	b2l_put64(draw + 28, pass->va + B2L_F_TLS);
	if (q->texture_va) {
		b2l_put64(draw + 8, va + B2L_D_TEXTURE);
		b2l_put64(draw + 10, va + B2L_D_SAMPLER);
		b2l_put64(draw + 20, va + B2L_D_VARYING_BUF);
		b2l_put64(draw + 22, va + B2L_D_VARYING);
		b2l_mali_write_texture(f, va, q);
	}

	position[0] = q->x0; position[1] = q->y0;
	position[4] = q->x1; position[5] = q->y0;
	position[8] = q->x0; position[9] = q->y1;
	position[12] = q->x1; position[13] = q->y1;
	for (i = 0; i < 4; i++) {
		position[i * 4 + 2] = B2L_FLOAT_HALF;
		position[i * 4 + 3] = B2L_FLOAT_ONE;
		((uint32_t *)(f + B2L_D_PUSH))[i] = q->colour[i];
	}
	b2l_mali_write_rsd((uint32_t *)(f + B2L_D_RSD), (uint32_t *)(f + B2L_D_BLEND), q);

	if (index) {
		b2l_put64((uint32_t *)(f - B2L_MALI_DRAW_BYTES) + 6, va);
	}
}

#endif /* _B2L_MALI_DRAW_H */
