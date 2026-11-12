// SPDX-License-Identifier: BSD-2-Clause
/*
 * Core Animation translation layer, loaded into backboardd only.
 *
 * It hooks CA::OGL::SWContext::draw_elements. Draws that can be translated are
 * sampled in shadow mode: the GPU draws into a private copy of the destination,
 * the CPU draws the real pixels, and the two are compared and timed per kind of
 * draw. Only the kinds this has proven, listed in live_kind(), then run on the
 * GPU for real, and only where it beats the CPU. Those kinds keep being sampled,
 * and any failure falls back to the CPU draw.
 *
 * All addresses are for the iOS 26.6.2 (23G90) shared cache, checked by cache
 * UUID and by the bytes being replaced; on any mismatch nothing is patched.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "b2l_flush_rect.h"
#include "b2l_mali_draw.h"
#include "b2l_mali_uapi.h"

typedef mach_port_t io_object_t, io_service_t, io_connect_t;
extern const mach_port_t kIOMainPortDefault;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t port, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallMethod(io_connect_t c, uint32_t selector, const uint64_t *in, uint32_t in_count,
    const void *struct_in, size_t struct_in_size, uint64_t *out, uint32_t *out_count, void *struct_out, size_t *struct_out_size);
extern kern_return_t IOConnectMapMemory64(io_connect_t c, uint32_t type, task_port_t task, mach_vm_address_t *address,
    mach_vm_size_t *size, uint32_t options);
extern bool _dyld_get_shared_cache_uuid(uuid_t uuid);

#define CACHE_UUID "\xcc\x67\x0b\x30\x8f\xdb\x34\xda\xaf\xab\x7a\xee\x12\x51\x38\x4b"
#define QC_BASE    0x183ed7000ull
#define DRAW_ENTRY 0x18410de88ull
#define PREPARE_DESTINATION 0x183f0e788ull      /* CA::OGL::Context::prepare_destination_() */
#define MARKED_DESTINATION  0x183f07bdcull      /* CA::OGL::Context::marked_destination() */

/* CA::OGL::Context */
#define CX_STATE        0x010   /* current state */
#define CX_SCISSOR      0x020   /* int x, y, w, h */
#define CX_SCISSOR_ON   0x040   /* byte */
#define CX_DEST_FLAGS   0x578   /* u16; bit 9: detached layers pending */
#define CX_DEST_LEVEL   0x2ac
#define CX_DRAW_COUNTS  0x4f8   /* two u32: draws, indices */
#define CX_DEST_LIST    0x2b0   /* differing pointers: destination needs preparing */
#define CX_TRANSFORM    0x568   /* vertices pass through this when set */
/* What the rasterizer draws into: its bounds and the pixel at their corner. */
#define CX_CUR_BASE     0x9a8
#define CX_CUR_PLANE2   0x9b0   /* a separate second plane, unsupported */
#define CX_CUR_STRIDE   0x9b8
#define CX_CUR_BITS     0x9c8
#define CX_CUR_X0       0x9d0
#define CX_CUR_X1       0x9d4
#define CX_CUR_Y0       0x9d8
#define CX_CUR_Y1       0x9dc
/* Current state */
#define ST_KIND    0x10         /* byte: function, selects the texture units */
#define ST_IMAGE   0x11         /* byte per unit: image function */
#define ST_UNIT    0x90         /* u16 per unit: 1, 2 image; 3 surface */
#define ST_BIND    0xb0         /* pointer per unit */
#define ST_BLEND   0x1f0        /* index into SW::Blend */
#define ST_BLENDON 0x1f1        /* bit 0: blending on; SW ignores the others */
/* CA::OGL::Surface, unit type 3 */
#define SF_SIZE    0x38         /* int w, h */
#define SF_STRIDE  0x98         /* signed */
#define SF_DATA    0xa0         /* first row in sampling order */
#define SF_FILTER  0xc0
/* CA::OGL::Image, unit types 1 and 2, and the texture behind it */
#define IM_TEXTURE 0x08
#define IM_VALID   0x70         /* byte: 1 when the texture is usable */
#define TX_SIZE    0x10         /* int w, h */
#define TX_DATA    0x70
#define TX_FORMAT  0xa8         /* byte */
#define TX_STRIDE  0xb0

#define BLEND_COPY  1
#define BLEND_SOVER 3
#define BLEND_ADD 12
#define IMAGE_FORMAT_MASK 9
#define IMAGE_FORMAT_RGBA8 11
#define IMAGE_FORMAT_RGBA16F 13
#define IMAGE_IDENTITY 0
#define UNIT_SURFACE 3
#define VERTEX_WORDS 12         /* position vec4, texture vec4, RGBA half floats */
#define MIN_SHADOW_PIXELS (64 * 64)
#define SHADOW_PER_WINDOW 200   /* compared draws in one 5 s window */
#define SHADOW_PER_KEY 12       /* so one busy kind cannot use them all */
#define WRONG_PER_KEY 3
#define LIVE_MIN_PIXELS 100000  /* below this the CPU is as fast */
#define LIVE_SAMPLES 1          /* live kinds still compared in each 5 s */
#define SHADOW_MAX_W 1440
#define SHADOW_MAX_H 3040
#define SHADOW_BYTES ((uint64_t)SHADOW_MAX_W * 4 * SHADOW_MAX_H)
#define STAGE_BYTES (16ull * 1024 * 1024) /* images whose rows the GPU cannot read in place */
#define MAX_QUADS 48
#define PASS_DRAWS (MAX_QUADS + 1)
#define TOLERANCE 2

static const uint32_t ENTRY_WORDS[4] = { 0xd503201f, 0x6db923e9, 0xa9016ffc, 0xa90267fa };

/* Displaced entry instructions, then back into the original body. */
void b2l_draw_original(void *ctx, uint32_t mode, uint32_t count, const uint16_t *indices,
    const void *vertices, uint32_t a, uint32_t b, void *clip);
uint64_t b2l_draw_resume;
__asm__(".text\n.p2align 2\n.globl _b2l_draw_original\n_b2l_draw_original:\n"
    " nop\n stp d9, d8, [sp, #-0x70]!\n stp x28, x27, [sp, #0x10]\n stp x26, x25, [sp, #0x20]\n"
    " adrp x16, _b2l_draw_resume@PAGE\n ldr x16, [x16, _b2l_draw_resume@PAGEOFF]\n br x16\n");

/*
 * Places where Core Animation reads or writes a destination on the CPU outside
 * draw_elements. Each gets a hook that finishes any pending GPU batch and then
 * runs the displaced entry instructions, as draw_elements does. Every argument
 * register is saved around the call, since the signatures differ.
 */
#define PREHOOK(name, index, address, w1, w2, w3, i1, i2, i3) \
	uint64_t b2l_resume_##name; \
	void b2l_prehook_##name(void); \
	static const uint32_t name##_WORDS[4] = { 0xd503201f, w1, w2, w3 }; \
	__asm__(".text\n.p2align 2\n.globl _b2l_prehook_" #name "\n_b2l_prehook_" #name ":\n" \
	    " stp x29, x30, [sp, #-0xe0]!\n stp x0, x1, [sp, #0x10]\n stp x2, x3, [sp, #0x20]\n" \
	    " stp x4, x5, [sp, #0x30]\n stp x6, x7, [sp, #0x40]\n str x8, [sp, #0x50]\n" \
	    " stp q0, q1, [sp, #0x60]\n stp q2, q3, [sp, #0x80]\n stp q4, q5, [sp, #0xa0]\n stp q6, q7, [sp, #0xc0]\n" \
	    " mov x0, #" #index "\n bl _b2l_batch_commit_hook\n" \
	    " ldp q6, q7, [sp, #0xc0]\n ldp q4, q5, [sp, #0xa0]\n ldp q2, q3, [sp, #0x80]\n ldp q0, q1, [sp, #0x60]\n" \
	    " ldr x8, [sp, #0x50]\n ldp x6, x7, [sp, #0x40]\n ldp x4, x5, [sp, #0x30]\n ldp x2, x3, [sp, #0x20]\n" \
	    " ldp x0, x1, [sp, #0x10]\n ldp x29, x30, [sp], #0xe0\n" \
	    " nop\n " i1 "\n " i2 "\n " i3 "\n" \
	    " adrp x16, _b2l_resume_" #name "@PAGE\n ldr x16, [x16, _b2l_resume_" #name "@PAGEOFF]\n br x16\n");

void b2l_batch_commit_hook(uint64_t which);
PREHOOK(clear_region, 5, 0x18410c740ull, 0xd10283ff, 0x6d0423e9, 0xa90567fa,
    "sub sp, sp, #0xa0", "stp d9, d8, [sp, #0x40]", "stp x26, x25, [sp, #0x50]")
PREHOOK(blur_surface, 6, 0x18410c974ull, 0xd10503ff, 0x6d0d2beb, 0x6d0e23e9,
    "sub sp, sp, #0x140", "stp d11, d10, [sp, #0xd0]", "stp d9, d8, [sp, #0xe0]")
PREHOOK(copy_destination, 7, 0x18410d0e4ull, 0xa9bb67fa, 0xa9015ff8, 0xa90257f6,
    "stp x26, x25, [sp, #-0x50]!", "stp x24, x23, [sp, #0x10]", "stp x22, x21, [sp, #0x20]")
PREHOOK(make_buffer_current, 8, 0x18410c420ull, 0xd10243ff, 0xa9055ff8, 0xa90657f6,
    "sub sp, sp, #0x90", "stp x24, x23, [sp, #0x50]", "stp x22, x21, [sp, #0x60]")
PREHOOK(finalize_surface, 9, 0x18410cf60ull, 0xa9be4ff4, 0xa9017bfd, 0x910043fd,
    "stp x20, x19, [sp, #-0x20]!", "stp x29, x30, [sp, #0x10]", "add x29, sp, #0x10")
PREHOOK(swap_begin, 10, 0x183f291fcull, 0xa9be4ff4, 0xa9017bfd, 0x910043fd,
    "stp x20, x19, [sp, #-0x20]!", "stp x29, x30, [sp, #0x10]", "add x29, sp, #0x10")

static const struct {
	const char *name;
	uint64_t address;
	const uint32_t *words;
	void (*stub)(void);
	uint64_t *resume;
} prehooks[] = {
	{ "clear_region", 0x18410c740ull, clear_region_WORDS, b2l_prehook_clear_region, &b2l_resume_clear_region },
	{ "blur_surface", 0x18410c974ull, blur_surface_WORDS, b2l_prehook_blur_surface, &b2l_resume_blur_surface },
	{ "copy_destination", 0x18410d0e4ull, copy_destination_WORDS, b2l_prehook_copy_destination,
	    &b2l_resume_copy_destination },
	{ "make_buffer_current", 0x18410c420ull, make_buffer_current_WORDS, b2l_prehook_make_buffer_current,
	    &b2l_resume_make_buffer_current },
	{ "finalize_surface", 0x18410cf60ull, finalize_surface_WORDS, b2l_prehook_finalize_surface,
	    &b2l_resume_finalize_surface },
	{ "swap_begin", 0x183f291fcull, swap_begin_WORDS, b2l_prehook_swap_begin, &b2l_resume_swap_begin },
};

static io_connect_t connection;

static void
log_line(const char *format, ...)
{
	char text[B2L_MALI_MAX_LOG - 1];
	va_list ap;
	va_start(ap, format);
	int n = vsnprintf(text, sizeof(text), format, ap);
	va_end(ap);
	if (connection != MACH_PORT_NULL && n > 0) {
		IOConnectCallMethod(connection, kB2LMaliLog, NULL, 0, text,
		    (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1, NULL, NULL, NULL, NULL);
	}
}

enum reject {
	R_NONE, R_SHAPE_CALL, R_BLEND, R_ALIGNMENT, R_DETACHED, R_BITS, R_TRANSFORM, R_KIND, R_NOT_QUAD,
	R_GEOMETRY, R_MANY_QUADS, R_FRACTIONAL, R_SMALL, R_TEXTURE, R_TEXTURE_STRIDE, R_TINT, R_ROTATED, R_COUNT
};
static uint32_t rejects[R_COUNT], reject_samples[R_COUNT];

#define MAX_KEYS 96
struct draw_key {
	uint8_t kind, blend, blend_on, image, format, padding;
	uint16_t unit0, unit1;
};
struct draw_stat {
	struct draw_key key;
	uint32_t wrong_logged, why;
	uint32_t draws, compared, exact, close, wrong, failed, live, live_failed;
	uint64_t ticks, gpu_ticks, cpu_ticks, pixels, live_ticks, live_pixels;
};
static struct draw_stat stats[MAX_KEYS];
static unsigned stat_count, compared_in_window;
static uint64_t window_start, ticks_in_draws, draws_total, overflow, thread_busy_us;
static mach_timebase_info_data_t timebase;
static int shadow_ready, shadow_disabled, shadow_busy;
static uint8_t *pass_cpu, *shadow_cpu, *stage_cpu;
static uint64_t pass_va, shadow_va, stage_va, code_va, heap_va;
static uint32_t pass_type, heap_bytes;

#define SHADER_TEXTURED  0x000
#define SHADER_SOLID     0x100
#define SHADER_MODULATED 0x200
#define SHADER_TINTED    0x300

static uint64_t
to_us(uint64_t ticks)
{
	return ticks * timebase.numer / timebase.denom / 1000;
}

static struct draw_stat *
stat_for(const struct draw_key *key)
{
	for (unsigned i = 0; i < stat_count; i++) {
		if (!memcmp(&stats[i].key, key, sizeof(*key))) {
			return &stats[i];
		}
	}
	if (stat_count == MAX_KEYS) {
		overflow++;
		return NULL;
	}
	stats[stat_count].key = *key;
	return &stats[stat_count++];
}

/* Where live draws spend their time, for one report window. */
static struct {
	uint64_t draws, map_ticks, flush_ticks, submit_ticks, drop_ticks, tiler_ns, fragment_ns, commits, commit_ticks;
	/* Why batches ended: cpu draw, other dest, full, self-sample, texture failed, then one per pre-hook. */
	uint64_t why[12];
} live_split;

static void
report(uint64_t now)
{
	/* This thread's CPU time: the draws' share of what the renderer actually did. */
	thread_basic_info_data_t info;
	mach_msg_type_number_t info_count = THREAD_BASIC_INFO_COUNT;
	uint64_t busy_us = 0;
	if (thread_info(mach_thread_self(), THREAD_BASIC_INFO, (thread_info_t)&info, &info_count) == KERN_SUCCESS) {
		busy_us = (uint64_t)info.user_time.seconds * 1000000 + info.user_time.microseconds +
		    (uint64_t)info.system_time.seconds * 1000000 + info.system_time.microseconds;
	}
	log_line("W window_us=%llu draw_us=%llu busy_us=%llu draws=%llu keys=%u overflow=%llu", to_us(now - window_start),
	    to_us(ticks_in_draws), busy_us - thread_busy_us, draws_total, stat_count, overflow);
	thread_busy_us = busy_us;
	if (live_split.draws) {
		log_line("P draws=%llu commits=%llu commit_us=%llu map_us=%llu flush_us=%llu submit_us=%llu drop_us=%llu tiler_us=%llu fragment_us=%llu",
		    live_split.draws, live_split.commits, to_us(live_split.commit_ticks), to_us(live_split.map_ticks),
		    to_us(live_split.flush_ticks), to_us(live_split.submit_ticks), to_us(live_split.drop_ticks),
		    live_split.tiler_ns / 1000, live_split.fragment_ns / 1000);
		log_line("B cpu=%llu dest=%llu full=%llu self=%llu tex=%llu clear=%llu blur=%llu copy=%llu current=%llu finalize=%llu swap=%llu",
		    live_split.why[0], live_split.why[1], live_split.why[2], live_split.why[3], live_split.why[4], live_split.why[5],
		    live_split.why[6], live_split.why[7], live_split.why[8], live_split.why[9], live_split.why[10]);
		memset(&live_split, 0, sizeof(live_split));
	}
	for (unsigned i = 0; i < stat_count; i++) {
		struct draw_stat *s = &stats[i];
		if (s->ticks) {
			log_line("K kind=%u blend=%u/%u image=%u fmt=%u units=%u/%u draws=%u us=%llu cmp=%u exact=%u close=%u wrong=%u fail=%u gpu_us=%llu cpu_us=%llu kpix=%llu why=%u live=%u live_fail=%u live_us=%llu live_kpix=%llu",
			    s->key.kind, s->key.blend, s->key.blend_on, s->key.image, s->key.format, s->key.unit0, s->key.unit1, s->draws,
			    to_us(s->ticks), s->compared, s->exact, s->close, s->wrong, s->failed, to_us(s->gpu_ticks),
			    to_us(s->cpu_ticks), s->pixels / 1000, s->why, s->live, s->live_failed, to_us(s->live_ticks),
			    s->live_pixels / 1000);
		}
		memset(&s->draws, 0, sizeof(*s) - offsetof(struct draw_stat, draws));
	}
	log_line("R %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u", rejects[1], rejects[2], rejects[3], rejects[4], rejects[5],
	    rejects[6], rejects[7], rejects[8], rejects[9], rejects[10], rejects[11], rejects[12], rejects[13], rejects[14],
	    rejects[15], rejects[16]);
	memset(rejects, 0, sizeof(rejects));
	ticks_in_draws = draws_total = 0;
	compared_in_window = 0;
	window_start = now;
}

#define FIELD(base, offset, type) (*(type *)((uintptr_t)(base) + (offset)))

static float
half_to_float(uint16_t h)
{
	return (float)(h >> 15 ? -1 : 1) * ((h & 0x7c00) ? (float)((h & 0x3ff) | 0x400) / 1024.0f *
	    ((h & 0x7c00) >> 10 >= 15 ? (float)(1u << (((h & 0x7c00) >> 10) - 15)) :
	    1.0f / (float)(1u << (15 - ((h & 0x7c00) >> 10)))) : 0.0f);
}

/* An axis-aligned quad as four corners, with one colour. */
struct quad_shape {
	float x0, y0, x1, y1;           /* context coordinates */
	float u[4], v[4];               /* at (x0,y0) (x1,y0) (x0,y1) (x1,y1) */
	uint16_t colour[4];
};

static void (*prepare_destination_function)(void *ctx);
static void (*marked_destination_function)(void *ctx);

static int
read_shape(const uint16_t *indices, const uint32_t *vertices, struct quad_shape *shape)
{
	const float *first = (const float *)(vertices + indices[0] * VERTEX_WORDS);
	float x[4], y[4];
	unsigned i, corner;

	shape->x0 = shape->x1 = first[0];
	shape->y0 = shape->y1 = first[1];
	memcpy(shape->colour, first + 8, sizeof(shape->colour));
	for (i = 0; i < 4; i++) {
		const float *v = (const float *)(vertices + indices[i] * VERTEX_WORDS);
		if (v[2] != 0.0f || v[3] != 1.0f || memcmp(v + 8, shape->colour, sizeof(shape->colour))) {
			return 0;
		}
		x[i] = v[0];
		y[i] = v[1];
		shape->x0 = x[i] < shape->x0 ? x[i] : shape->x0;
		shape->x1 = x[i] > shape->x1 ? x[i] : shape->x1;
		shape->y0 = y[i] < shape->y0 ? y[i] : shape->y0;
		shape->y1 = y[i] > shape->y1 ? y[i] : shape->y1;
	}
	if (shape->x1 <= shape->x0 || shape->y1 <= shape->y0) {
		return 0;
	}
	unsigned seen = 0;
	for (i = 0; i < 4; i++) {
		const float *v = (const float *)(vertices + indices[i] * VERTEX_WORDS);
		if ((x[i] != shape->x0 && x[i] != shape->x1) || (y[i] != shape->y0 && y[i] != shape->y1)) {
			return 0;       /* not axis aligned */
		}
		corner = (x[i] == shape->x1 ? 1u : 0u) | (y[i] == shape->y1 ? 2u : 0u);
		shape->u[corner] = v[4];
		shape->v[corner] = v[5];
		seen |= 1u << corner;
	}
	return seen == 15;
}

/* Clean and invalidate to memory: the GPU is not coherent with the CPU. */
/* A rectangle of a surface to clean and invalidate. */
struct flush_rect {
	const uint8_t *base;
	size_t stride, row_bytes;
	unsigned rows;
};

static void
cache_flush(const struct flush_rect *r)
{
	for (unsigned row = 0; row < r->rows; row++) {
		uintptr_t begin = (uintptr_t)r->base + row * r->stride;
		uintptr_t line = begin & ~(uintptr_t)63, end = begin + r->row_bytes;

		for (; line < end; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r"(line));
		}
	}
	__asm__ volatile ("dsb sy");
}

static uint64_t
map_range(const void *address, uint64_t bytes)
{
	uint64_t in[2] = { (uint64_t)address, bytes }, va = 0;
	uint32_t count = 1;

	return IOConnectCallMethod(connection, kB2LMaliMapRange, in, 2, NULL, 0, &va, &count, NULL, NULL) ? 0 : va;
}

struct translation {
	struct b2l_mali_quad quads[MAX_QUADS];
	unsigned quad_count;
	const uint8_t *dest;            /* lowest address of the destination */
	uint32_t stride, width, height;
	int dest_bottom_up;
	int x0, y0, x1, y1;             /* covered pixels, memory rows */
	const uint8_t *texture;
	size_t texture_bytes, texture_source_stride;
	int staged;                     /* sampled through stage_cpu, whose rows are aligned */
	unsigned texel_bytes;
	int tinted;                     /* a colour, or a one-channel mask */
	float opacity;
};

/* The texture a draw samples, and what the coordinates need to reach it. */
struct source {
	int surface;
	uint64_t magnitude;
	int64_t stride;
	int width, height, linear, staged;
	unsigned texel_bytes;
	float opacity;
};

/* Exactly the test draw_elements makes before it calls prepare_destination_. */
static void
prepare_destination(void *ctx)
{
	void *top = FIELD(ctx, CX_DEST_LIST, void *), *bound = FIELD(ctx, CX_DEST_LIST + 8, void *);
	uint32_t level = top != NULL ? (FIELD(top, 0x90, uint32_t) >> 1) & 31 : 0;

	if (top != bound || FIELD(ctx, CX_DEST_LEVEL, uint32_t) != level) {
		prepare_destination_function(ctx);
	}
}

/*
 * One quad of the draw, clipped to the rasterizer's bounds. (dx, dy) is the pixel
 * at the destination's first row; rows in memory run the other way if bottom-up.
 */
static enum reject
translate_quad(const uint16_t *indices, const void *vertices, const struct source *src, int solid, int dx, int dy,
    int width, int height, int bottom_up, int cx0, int cy0, int cx1, int cy1, uint16_t *colour,
    struct b2l_mali_quad *g, int *box)
{
	struct quad_shape q;

	if (!read_shape(indices, vertices, &q)) {
		if (reject_samples[R_NOT_QUAD]++ < 4) {
			const uint32_t *v = vertices;
			log_line("Q %u,%u,%u,%u %08x %08x %08x %08x c=%08x %08x / %08x %08x %08x %08x c=%08x %08x", indices[0], indices[1],
			    indices[2], indices[3], v[0], v[1], v[2], v[3], v[8], v[9], v[12], v[13], v[14], v[15], v[20], v[21]);
		}
		return R_NOT_QUAD;
	}
	memcpy(colour, q.colour, sizeof(q.colour));
	/* Only whole pixels translate: partial edges are antialiased on the CPU. */
	int qx0 = (int)q.x0, qx1 = (int)q.x1, qy0 = (int)q.y0, qy1 = (int)q.y1;
	if ((float)qx0 != q.x0 || (float)qx1 != q.x1 || (float)qy0 != q.y0 || (float)qy1 != q.y1) {
		return R_FRACTIONAL;
	}
	int rx0 = qx0 > cx0 ? qx0 : cx0, rx1 = qx1 < cx1 ? qx1 : cx1;
	int ry0 = qy0 > cy0 ? qy0 : cy0, ry1 = qy1 < cy1 ? qy1 : cy1;
	if (rx1 <= rx0 || ry1 <= ry0) {
		g->x0 = g->x1 = 0;      /* fully clipped: nothing to draw */
		return R_NONE;
	}
	int x0 = rx0 - dx, x1 = rx1 - dx, y0 = ry0 - dy, y1 = ry1 - dy;
	float gy0 = bottom_up ? (float)(height - y0) : (float)y0;
	float gy1 = bottom_up ? (float)(height - y1) : (float)y1;
	(void)width;
	g->x0 = b2l_int_float((uint32_t)x0);
	g->x1 = b2l_int_float((uint32_t)x1);
	memcpy(&g->y0, &gy0, 4);
	memcpy(&g->y1, &gy1, 4);
	int my0 = bottom_up ? height - y1 : y0, my1 = bottom_up ? height - y0 : y1;
	box[0] = x0 < box[0] ? x0 : box[0];
	box[1] = my0 < box[1] ? my0 : box[1];
	box[2] = x1 > box[2] ? x1 : box[2];
	box[3] = my1 > box[3] ? my1 : box[3];
	box[4] += (x1 - x0) * (y1 - y0);
	if (solid) {
		return R_NONE;
	}

	/* Coordinates are pixels or normalized; their range tells which. */
	int pixels = 0;
	for (int i = 0; i < 4; i++) {
		pixels |= q.u[i] > 1.001f || q.v[i] > 1.001f;
	}
	float su = pixels ? 1.0f / (float)src->width : 1.0f, sv = pixels ? 1.0f / (float)src->height : 1.0f;
	float cu[4], cv[4];
	for (int i = 0; i < 4; i++) {
		cu[i] = q.u[i] * su;
		cv[i] = q.v[i] * sv;
		if (src->stride < 0) {
			cv[i] = 1.0f - cv[i];
		}
	}
	/* The pass builder takes a rectangle: reject rotated or sheared mappings. */
	if (cu[0] != cu[2] || cu[1] != cu[3] || cv[0] != cv[1] || cv[2] != cv[3]) {
		return R_ROTATED;
	}
	float du = (cu[1] - cu[0]) / (q.x1 - q.x0), dv = (cv[2] - cv[0]) / (q.y1 - q.y0);
	float u0 = cu[0] + du * ((float)rx0 - q.x0), u1 = cu[0] + du * ((float)rx1 - q.x0);
	float v0 = cv[0] + dv * ((float)ry0 - q.y0), v1 = cv[0] + dv * ((float)ry1 - q.y0);
	memcpy(&g->u0, &u0, 4);
	memcpy(&g->u1, &u1, 4);
	memcpy(&g->v0, &v0, 4);
	memcpy(&g->v1, &v1, 4);
	return R_NONE;
}

/* Decide, without changing any pixels, whether this draw can run on the GPU. */
static enum reject
translate(void *ctx, uint32_t mode, uint32_t count, const uint16_t *indices, const void *vertices,
    void *clip, const struct draw_key *key, struct translation *t)
{
	const uint8_t *state = FIELD(ctx, CX_STATE, const uint8_t *);

	if (mode != 0 || count == 0 || (count & 3) || indices == NULL || vertices == NULL || clip != NULL) {
		return R_SHAPE_CALL;
	}
	if (count / 4 > MAX_QUADS) {
		return R_MANY_QUADS;
	}
	if ((key->blend_on & 1) && key->blend != BLEND_SOVER && key->blend != BLEND_COPY && key->blend != BLEND_ADD) {
		return R_BLEND;
	}
	if (FIELD(ctx, CX_DEST_FLAGS, uint16_t) & 0x200) {
		return R_DETACHED;
	}
	int solid = key->kind == 0 && key->unit0 == 0;
	int surface = key->unit0 == UNIT_SURFACE;
	int image = key->unit0 == 1 || key->unit0 == 2;
	int textured = (key->kind == 1 || key->kind == 3 || key->kind == 52) && (surface || image) &&
	    key->image == IMAGE_IDENTITY;
	if (!solid && !textured) {
		return R_KIND;
	}
	prepare_destination(ctx);
	if (FIELD(ctx, CX_CUR_BITS, uint32_t) != 32) {
		return R_BITS;
	}
	if (FIELD(ctx, CX_TRANSFORM, void *) != NULL) {
		if (!reject_samples[R_TRANSFORM]++) {
			const double *m = FIELD(ctx, CX_TRANSFORM, const double *);
			log_line("T %d %d %d %d %d %d %d %d", (int)(m[0] * 1000), (int)(m[1] * 1000), (int)(m[2] * 1000),
			    (int)(m[3] * 1000), (int)(m[4] * 1000), (int)(m[5] * 1000), (int)(m[6] * 1000), (int)(m[7] * 1000));
		}
		return R_TRANSFORM;
	}

	/* The rasterizer's window is the whole destination as far as the GPU goes. */
	const uint8_t *base = FIELD(ctx, CX_CUR_BASE, const uint8_t *);
	const uint8_t *plane2 = FIELD(ctx, CX_CUR_PLANE2, const uint8_t *);
	int64_t stride = FIELD(ctx, CX_CUR_STRIDE, int64_t);
	int dx = FIELD(ctx, CX_CUR_X0, int32_t), dy = FIELD(ctx, CX_CUR_Y0, int32_t);
	int cx1 = FIELD(ctx, CX_CUR_X1, int32_t), cy1 = FIELD(ctx, CX_CUR_Y1, int32_t);
	int cx0 = dx, cy0 = dy, width = cx1 - dx, height = cy1 - dy;
	uint64_t magnitude = stride < 0 ? (uint64_t)-stride : (uint64_t)stride;

	if (base == NULL || width <= 0 || height <= 0 || width > SHADOW_MAX_W || height > SHADOW_MAX_H ||
	    magnitude < (uint64_t)width * 4 || (magnitude & 63) || magnitude * (uint64_t)height > SHADOW_BYTES ||
	    (plane2 != NULL && plane2 != base)) {
		if (reject_samples[R_GEOMETRY]++ < 4) {
			log_line("G cur=%p p2=%p stride=%lld bounds=%d,%d-%d,%d", base, plane2, stride, cx0, cy0, cx1, cy1);
		}
		return R_GEOMETRY;
	}
	const uint8_t *lowest = stride < 0 ? base + (int64_t)(height - 1) * stride : base;
	if ((uintptr_t)lowest & 63) {
		return R_ALIGNMENT;
	}
	if (FIELD(ctx, CX_SCISSOR_ON, uint8_t) == 1) {
		int sx = FIELD(ctx, CX_SCISSOR, int32_t), sy = FIELD(ctx, CX_SCISSOR + 4, int32_t);
		int sw = FIELD(ctx, CX_SCISSOR + 8, int32_t), sh = FIELD(ctx, CX_SCISSOR + 12, int32_t);
		cx0 = sx > cx0 ? sx : cx0;
		cy0 = sy > cy0 ? sy : cy0;
		cx1 = sx + sw < cx1 ? sx + sw : cx1;
		cy1 = sy + sh < cy1 ? sy + sh : cy1;
	}

	struct source src;
	const uint8_t *data = NULL;
	memset(&src, 0, sizeof(src));
	if (textured) {
		const uint8_t *unit = FIELD(state, ST_BIND, const uint8_t *);
		if (surface) {
			data = FIELD(unit, SF_DATA, const uint8_t *);
			src.stride = FIELD(unit, SF_STRIDE, int64_t);
			src.width = FIELD(unit, SF_SIZE, int32_t);
			src.height = FIELD(unit, SF_SIZE + 4, int32_t);
			src.linear = FIELD(unit, SF_FILTER, uint8_t) != 0;
		} else {
			const uint8_t *texture = FIELD(unit, IM_TEXTURE, const uint8_t *);
			if (texture == NULL || FIELD(unit, IM_VALID, uint8_t) != 1) {
				return R_TEXTURE;
			}
			data = FIELD(texture, TX_DATA, const uint8_t *);
			src.stride = FIELD(texture, TX_STRIDE, int64_t);
			src.width = FIELD(texture, TX_SIZE, int32_t);
			src.height = FIELD(texture, TX_SIZE + 4, int32_t);
			src.linear = (FIELD(unit, 0x34, uint64_t) & 0xff0000) != 0;
		}
		src.magnitude = src.stride < 0 ? (uint64_t)-src.stride : (uint64_t)src.stride;
		src.texel_bytes = key->format == IMAGE_FORMAT_RGBA16F ? 8 : key->format == IMAGE_FORMAT_MASK ? 1 : 4;
		if (data == NULL || src.width <= 0 || src.height <= 0 || src.width > 16384 || src.height > 16384) {
			return R_TEXTURE;
		}
		if ((src.magnitude & 63) || src.magnitude < (uint64_t)src.width * src.texel_bytes) {
			static uint32_t formats_seen;
			reject_samples[R_TEXTURE_STRIDE]++;
			if (key->format < 32 && !(formats_seen & (1u << key->format))) {
				formats_seen |= 1u << key->format;
				log_line("S kind=%u fmt=%u %dx%d stride=%llu", key->kind, key->format, src.width, src.height,
				    src.magnitude);
			}
			uint64_t row = (uint64_t)src.width * src.texel_bytes;
			src.staged = src.magnitude >= row && ((row + 63) & ~63ull) * (uint64_t)src.height <= STAGE_BYTES;
			if (!src.staged) {
				return R_TEXTURE_STRIDE;
			}
		}
	}

	memset(t, 0, sizeof(*t));
	t->stride = (uint32_t)magnitude;
	t->width = (uint32_t)width;
	t->height = (uint32_t)height;
	t->dest_bottom_up = stride < 0;
	t->dest = lowest;
	int box[5] = { width, height, 0, 0, 0 };
	uint16_t colour[4] = { 0, 0, 0, 0 }, first[4];
	unsigned quads = count / 4;
	for (unsigned i = 0; i < quads; i++) {
		enum reject why = translate_quad(indices + i * 4, vertices, &src, solid, dx, dy, width, height, stride < 0,
		    cx0, cy0, cx1, cy1, colour, &t->quads[i], box);
		if (why != R_NONE) {
			return why;
		}
		if (i == 0) {
			memcpy(first, colour, sizeof(first));
		} else if (memcmp(first, colour, sizeof(first))) {
			return R_NOT_QUAD;
		}
	}
	if (box[4] < MIN_SHADOW_PIXELS) {
		return R_SMALL;
	}
	t->x0 = box[0];
	t->y0 = box[1];
	t->x1 = box[2];
	t->y1 = box[3];

	float r = half_to_float(first[0]), green = half_to_float(first[1]);
	float b = half_to_float(first[2]), a = half_to_float(first[3]);
	struct b2l_mali_quad common;
	memset(&common, 0, sizeof(common));
	common.equation = !(key->blend_on & 1) ? B2L_EQ_COPY : key->blend == BLEND_SOVER ? B2L_EQ_SRC_OVER :
	    key->blend == BLEND_ADD ? B2L_EQ_ADD : B2L_EQ_COPY;
	if (solid) {
		float value[4] = { r, green, b, a };
		memcpy(common.colour, value, sizeof(value));
		common.shader_va = code_va + SHADER_SOLID;
		common.fau_slots = 2;
	} else {
		/* Kind 1 samples the texture alone: the CPU ignores vertex colour there. */
		if (key->kind == 1) {
			r = green = b = a = 1.0f;
		}
		int tinted = r != green || green != b || b != a;
		t->texture = src.stride < 0 ? data + (int64_t)(src.height - 1) * src.stride : data;
		t->texture_bytes = (size_t)src.magnitude * (size_t)src.height;
		t->texture_source_stride = (size_t)src.magnitude;
		t->staged = src.staged;
		common.texture_width = (uint32_t)src.width;
		common.texture_height = (uint32_t)src.height;
		common.texture_stride = src.staged ? (uint32_t)(((uint64_t)src.width * src.texel_bytes + 63) & ~63ull) :
		    (uint32_t)src.magnitude;
		common.texture_format = key->format == IMAGE_FORMAT_RGBA16F ? B2L_FORMAT_RGBA16_FLOAT :
		    key->format == IMAGE_FORMAT_RGBA8 ? B2L_FORMAT_RGBA8_UNORM :
		    key->format == IMAGE_FORMAT_MASK ? B2L_FORMAT_R8_UNORM : B2L_FORMAT_BGRA8_UNORM;
		common.texture_mask = key->format == IMAGE_FORMAT_MASK;
		t->texel_bytes = src.texel_bytes;
		common.linear = (uint32_t)src.linear;
		t->opacity = a;
		t->tinted = tinted || common.texture_mask;
		if (tinted) {
			float value[4] = { r, green, b, a };
			memcpy(common.colour, value, sizeof(value));
			common.shader_va = code_va + SHADER_TINTED;
			common.fau_slots = 2;
		} else if (a == 1.0f) {
			common.shader_va = code_va + SHADER_TEXTURED;
			common.first_clause_blends = 1;
		} else {
			float opacity[4] = { a, 0.0f, 0.0f, 0.0f };
			memcpy(common.colour, opacity, sizeof(opacity));
			common.shader_va = code_va + SHADER_MODULATED;
			common.fau_slots = 1;
		}
	}
	for (unsigned i = 0; i < quads; i++) {
		struct b2l_mali_quad *g = &t->quads[i];
		if (g->x0 == 0 && g->x1 == 0) {
			continue;       /* clipped away */
		}
		uint32_t x0 = g->x0, x1 = g->x1, y0 = g->y0, y1 = g->y1;
		uint32_t u0 = g->u0, u1 = g->u1, v0 = g->v0, v1 = g->v1;
		*g = common;
		g->x0 = x0; g->x1 = x1; g->y0 = y0; g->y1 = y1;
		g->u0 = u0; g->u1 = u1; g->v0 = v0; g->v1 = v1;
		t->quad_count = i + 1;
	}
	return R_NONE;
}

/* Stage, clean and map the texture a translation samples; 0 if it cannot be. */
static int
prepare_texture(struct translation *t)
{
	if (t->texture != NULL) {
		uint32_t tx0 = t->quads[0].texture_width, tx1 = 0, ty0 = t->quads[0].texture_height, ty1 = 0;
		for (unsigned i = 0; i < t->quad_count; i++) {
			const struct b2l_mali_quad *q = &t->quads[i];
			if (q->shader_va != 0 && !(q->x0 == 0 && q->x1 == 0)) {
				float u0, u1, v0, v1;
				memcpy(&u0, &q->u0, 4);
				memcpy(&u1, &q->u1, 4);
				memcpy(&v0, &q->v0, 4);
				memcpy(&v1, &q->v1, 4);
				b2l_flush_span(u0, u1, q->texture_width, &tx0, &tx1);
				b2l_flush_span(v0, v1, q->texture_height, &ty0, &ty1);
			}
		}
		size_t stride = t->quads[0].texture_stride;
		const uint8_t *sampled_base = t->staged ? stage_cpu : t->texture;
		if (t->staged) {
			for (uint32_t y = ty0; y < ty1; y++) {
				memcpy(stage_cpu + y * stride + (size_t)tx0 * t->texel_bytes,
				    t->texture + y * t->texture_source_stride + (size_t)tx0 * t->texel_bytes,
				    (size_t)(tx1 - tx0) * t->texel_bytes);
			}
		}
		if (tx1 > tx0 && ty1 > ty0) {
			struct flush_rect sampled = { sampled_base + ty0 * stride + (size_t)tx0 * t->texel_bytes, stride,
				(size_t)(tx1 - tx0) * t->texel_bytes, ty1 - ty0 };
			cache_flush(&sampled);
		}
		uint64_t texture_va = t->staged ? stage_va : map_range(t->texture, t->texture_bytes);
		if (texture_va == 0) {
			return 0;
		}
		for (unsigned i = 0; i < t->quad_count; i++) {
			t->quads[i].texture_va = texture_va;
		}
	}
	return 1;
}

/*
 * Consecutive live draws into one destination form a batch: one GPU pass, one
 * clean of the destination before and one invalidate after, instead of a pass per
 * draw. The batch ends when a draw cannot join it, when anything else touches a
 * destination on the CPU (the pre-hooks above), or when the pass is full. While a
 * batch is open, nothing on the CPU reads or writes its destination or any
 * surface it samples: every path that could does so through draw_elements or a
 * hooked function, and both commit first.
 */
static struct {
	int active;
	const uint8_t *dest;
	uint8_t *target;
	uint64_t target_va;
	uint32_t stride, width, height;
	int bx0, by0, bx1, by1;
	struct b2l_mali_pass pass;
	const void *textures[PASS_DRAWS];
	unsigned texture_count, draws;
} batch;

static void
batch_keep(void)
{
	float u0 = (float)batch.bx0 / (float)batch.width, u1 = (float)batch.bx1 / (float)batch.width;
	float v0 = (float)batch.by0 / (float)batch.height, v1 = (float)batch.by1 / (float)batch.height;
	struct b2l_mali_quad keep = {
		b2l_int_float((uint32_t)batch.bx0), b2l_int_float((uint32_t)batch.by0), b2l_int_float((uint32_t)batch.bx1),
		b2l_int_float((uint32_t)batch.by1), 0, 0, 0, 0, { 0, 0, 0, 0 }, code_va + SHADER_TEXTURED, batch.target_va,
		batch.width, batch.height, batch.stride, B2L_FORMAT_BGRA8_UNORM, B2L_EQ_COPY, 0, 1, 0, 0,
	};
	memcpy(&keep.u0, &u0, 4);
	memcpy(&keep.u1, &u1, 4);
	memcpy(&keep.v0, &v0, 4);
	memcpy(&keep.v1, &v1, 4);
	/* Slot 0 again, with the final bounds; then the link to slot 1 it zeroed. */
	unsigned draws = batch.pass.draws;
	batch.pass.draws = 0;
	b2l_mali_pass_add(&batch.pass, &keep);
	batch.pass.draws = draws;
	if (draws > 1) {
		b2l_put64((uint32_t *)(batch.pass.cpu + B2L_MALI_PASS_DRAWS) + 6, batch.pass.va + B2L_MALI_PASS_DRAWS + B2L_MALI_DRAW_BYTES);
	}
}

/* Run the open batch, if any, noting why. Caller holds shadow_busy. */
static void
batch_commit_why(unsigned why)
{
	if (!batch.active) {
		return;
	}
	live_split.why[why < 12 ? why : 11]++;
	uint64_t begin = mach_absolute_time();
	batch.active = 0;
	batch_keep();
	b2l_mali_pass_bounds(&batch.pass, (uint32_t)batch.bx0, (uint32_t)batch.by0, (uint32_t)batch.bx1, (uint32_t)batch.by1);
	struct flush_rect drawn = { batch.target + (size_t)batch.by0 * batch.stride + (size_t)batch.bx0 * 4, batch.stride,
		(size_t)(batch.bx1 - batch.bx0) * 4, (unsigned)(batch.by1 - batch.by0) };
	uint64_t mapped = mach_absolute_time();
	cache_flush(&drawn);
	struct b2l_mali_submit request = { batch.pass.va + B2L_MALI_PASS_DRAWS, batch.pass.va + B2L_MALI_PASS_FRAGMENT,
		pass_type, B2L_MALI_PASS_BYTES(batch.pass.draws), 0, 0, { 0 } };
	uint64_t result[3] = { 0, 0, 0 };
	uint32_t out_count = 3;
	uint64_t submitted = mach_absolute_time();
	kern_return_t kr = IOConnectCallMethod(connection, kB2LMaliSubmit, NULL, 0, &request, sizeof(request),
	    result, &out_count, NULL, NULL);
	uint64_t returned = mach_absolute_time();
	cache_flush(&drawn);
	uint64_t end = mach_absolute_time();
	live_split.map_ticks += mapped - begin;
	live_split.flush_ticks += submitted - mapped;
	live_split.submit_ticks += returned - submitted;
	live_split.drop_ticks += end - returned;
	live_split.tiler_ns += result[1];
	live_split.fragment_ns += result[2];
	live_split.commits++;
	live_split.commit_ticks += end - begin;
	ticks_in_draws += end - begin;
	if (kr || result[0] != B2L_MALI_STATUS_DONE) {
		log_line("gpu: batch kr=0x%x status=%llu draws=%u, stopping", kr, result[0], batch.draws);
		shadow_disabled = 1;
	}
}

#define batch_commit() batch_commit_why(0)

/* From the pre-hooks: any thread, no lock held. */
void
b2l_batch_commit_hook(uint64_t which)
{
	if (!__atomic_load_n(&batch.active, __ATOMIC_ACQUIRE)) {
		return;
	}
	while (__atomic_exchange_n(&shadow_busy, 1, __ATOMIC_ACQUIRE)) {
	}
	batch_commit_why((unsigned)which);
	__atomic_store_n(&shadow_busy, 0, __ATOMIC_RELEASE);
}

/* Add a live draw to the batch, opening one for its destination if needed. */
static uint64_t
batch_append(struct translation *t)
{
	int bx0 = t->x0 & ~15, by0 = t->y0 & ~15;
	int bx1 = (t->x1 + 15) & ~15, by1 = (t->y1 + 15) & ~15;
	bx1 = bx1 > (int)t->width ? (int)t->width : bx1;
	by1 = by1 > (int)t->height ? (int)t->height : by1;
	uint64_t begin = mach_absolute_time();

	if (batch.active) {
		if (batch.dest != t->dest || batch.stride != t->stride || batch.width != t->width || batch.height != t->height) {
			batch_commit_why(1);
		} else if (batch.pass.draws + t->quad_count >= PASS_DRAWS) {
			batch_commit_why(2);
		} else if (t->texture == t->dest) {
			batch_commit_why(3);
		}
	}
	if (!batch.active) {
		uint64_t target_va = map_range(t->dest, (uint64_t)t->stride * t->height);
		if (target_va == 0 || (target_va & 63)) {
			return 0;
		}
		batch.dest = t->dest;
		batch.target = (uint8_t *)(uintptr_t)t->dest;
		batch.target_va = target_va;
		batch.stride = t->stride;
		batch.width = t->width;
		batch.height = t->height;
		batch.bx0 = bx0; batch.by0 = by0; batch.bx1 = bx1; batch.by1 = by1;
		batch.texture_count = 0;
		batch.draws = 0;
		struct b2l_mali_target rt = { target_va, t->width, t->height, t->stride, 0, B2L_RT_BGRA, heap_va, heap_bytes };
		b2l_mali_pass_begin(&batch.pass, pass_cpu, pass_va, &rt);
		struct b2l_mali_quad placeholder;
		memset(&placeholder, 0, sizeof(placeholder));
		placeholder.shader_va = code_va + SHADER_TEXTURED;
		b2l_mali_pass_add(&batch.pass, &placeholder);
		batch.active = 1;
	}
	if (!prepare_texture(t)) {
		batch_commit_why(4);
		return 0;
	}
	batch.bx0 = bx0 < batch.bx0 ? bx0 : batch.bx0;
	batch.by0 = by0 < batch.by0 ? by0 : batch.by0;
	batch.bx1 = bx1 > batch.bx1 ? bx1 : batch.bx1;
	batch.by1 = by1 > batch.by1 ? by1 : batch.by1;
	for (unsigned i = 0; i < t->quad_count; i++) {
		if (t->quads[i].shader_va != 0 && !(t->quads[i].x0 == 0 && t->quads[i].x1 == 0)) {
			b2l_mali_pass_add(&batch.pass, &t->quads[i]);
		}
	}
	if (t->texture != NULL && batch.texture_count < PASS_DRAWS) {
		batch.textures[batch.texture_count++] = t->texture;
	}
	batch.draws++;
	live_split.draws++;
	return mach_absolute_time() - begin + 1;
}

/*
 * Run the draw on the GPU, into the real destination or the shadow copy of it.
 * A tile starts empty, so a first draw brings back what the quads do not cover.
 * Returns the time taken, or 0 if nothing was drawn.
 */
static uint64_t
gpu_draw(struct translation *t, int live)
{
	int bx0 = t->x0 & ~15, by0 = t->y0 & ~15;
	int bx1 = (t->x1 + 15) & ~15, by1 = (t->y1 + 15) & ~15;
	bx1 = bx1 > (int)t->width ? (int)t->width : bx1;
	by1 = by1 > (int)t->height ? (int)t->height : by1;
	uint64_t begin = mach_absolute_time();
	uint8_t *target = live ? (uint8_t *)(uintptr_t)t->dest : shadow_cpu;
	uint64_t target_va = live ? map_range(t->dest, (uint64_t)t->stride * t->height) : shadow_va;

	if (target_va == 0 || (target_va & 63)) {
		return 0;
	}
	if (!live) {
		for (int y = by0; y < by1; y++) {
			memcpy(shadow_cpu + (size_t)y * t->stride + (size_t)bx0 * 4,
			    t->dest + (size_t)y * t->stride + (size_t)bx0 * 4, (size_t)(bx1 - bx0) * 4);
		}
		begin = mach_absolute_time();
	}
	struct flush_rect drawn = { target + (size_t)by0 * t->stride + (size_t)bx0 * 4, t->stride,
		(size_t)(bx1 - bx0) * 4, (unsigned)(by1 - by0) };
	uint64_t mapped = mach_absolute_time();
	cache_flush(&drawn);

	if (!prepare_texture(t)) {
		return 0;
	}

	struct b2l_mali_target rt = { target_va, t->width, t->height, t->stride, 0, B2L_RT_BGRA, heap_va, heap_bytes };
	struct b2l_mali_pass pass;
	b2l_mali_pass_begin(&pass, pass_cpu, pass_va, &rt);
	b2l_mali_pass_bounds(&pass, (uint32_t)bx0, (uint32_t)by0, (uint32_t)bx1, (uint32_t)by1);

	float u0 = (float)bx0 / (float)t->width, u1 = (float)bx1 / (float)t->width;
	float v0 = (float)by0 / (float)t->height, v1 = (float)by1 / (float)t->height;
	struct b2l_mali_quad keep = {
		b2l_int_float((uint32_t)bx0), b2l_int_float((uint32_t)by0), b2l_int_float((uint32_t)bx1),
		b2l_int_float((uint32_t)by1), 0, 0, 0, 0, { 0, 0, 0, 0 }, code_va + SHADER_TEXTURED, target_va,
		t->width, t->height, t->stride, B2L_FORMAT_BGRA8_UNORM, B2L_EQ_COPY, 0, 1, 0, 0,
	};
	memcpy(&keep.u0, &u0, 4);
	memcpy(&keep.u1, &u1, 4);
	memcpy(&keep.v0, &v0, 4);
	memcpy(&keep.v1, &v1, 4);
	b2l_mali_pass_add(&pass, &keep);
	for (unsigned i = 0; i < t->quad_count; i++) {
		if (t->quads[i].shader_va != 0 && !(t->quads[i].x0 == 0 && t->quads[i].x1 == 0)) {
			b2l_mali_pass_add(&pass, &t->quads[i]);
		}
	}

	struct b2l_mali_submit request = { pass_va + B2L_MALI_PASS_DRAWS, pass_va + B2L_MALI_PASS_FRAGMENT,
		pass_type, B2L_MALI_PASS_BYTES(pass.draws), 0, 0, { 0 } };
	uint64_t result[3] = { 0, 0, 0 };
	uint32_t out_count = 3;
	uint64_t submitted = mach_absolute_time();
	kern_return_t kr = IOConnectCallMethod(connection, kB2LMaliSubmit, NULL, 0, &request, sizeof(request),
	    result, &out_count, NULL, NULL);
	uint64_t returned = mach_absolute_time();
	cache_flush(&drawn);                    /* drop lines from before the GPU wrote */
	if (live) {
		live_split.draws++;
		live_split.map_ticks += mapped - begin;
		live_split.flush_ticks += submitted - mapped;
		live_split.submit_ticks += returned - submitted;
		live_split.drop_ticks += mach_absolute_time() - returned;
		live_split.tiler_ns += result[1];
		live_split.fragment_ns += result[2];
	}
	if (kr || result[0] != B2L_MALI_STATUS_DONE) {
		log_line("gpu: submit kr=0x%x status=%llu live=%d, stopping", kr, result[0], live);
		shadow_disabled = 1;
		return 0;
	}
	return mach_absolute_time() - begin + 1;
}

/*
 * Kinds shadow mode has shown to match. Across 0130 and 0131: kind 3 Sover, 97
 * of 97 within tolerance. Kind 1 Sover and Copy, 1 of 62 and 1 of 104 over by a
 * single level, in filtered pixels. Plain copies and fills are not here: the
 * CPU does those faster.
 */
static int
live_kind(const struct draw_key *key)
{
	/* Image formats 1, 11, 13: no wrong pixels in 0143. Format 9 masks: 65 of 66 in 0144. */
	int image = (key->unit0 == 1 || key->unit0 == 2) && (key->format == 1 || key->format == IMAGE_FORMAT_RGBA8 ||
	    key->format == IMAGE_FORMAT_RGBA16F || key->format == IMAGE_FORMAT_MASK);
	int source = key->unit0 == UNIT_SURFACE || image;
	if (!source || key->unit1 != 0 || key->image != IMAGE_IDENTITY) {
		return 0;
	}
	if ((key->kind == 3 || key->kind == 1) && key->blend == BLEND_SOVER && (key->blend_on & 1)) {
		return 1;
	}
	if (key->kind == 1 && key->blend == BLEND_COPY && !(key->blend_on & 1)) {
		return 1;
	}
	/* Masks arrive with the Sover blend and other flag bits set. */
	return key->format == IMAGE_FORMAT_MASK && key->blend == BLEND_SOVER && (key->blend_on & 1);
}

/* A plain copy is only worth the GPU when it has to filter. */
static int
live_worthwhile(const struct draw_key *key, const struct translation *t)
{
	return (key->blend_on & 1) || t->quads[0].linear;
}

/* Every pixel of the covered tiles, so anything drawn out of bounds shows too. */
static void
compare(const struct translation *t, struct draw_stat *stat)
{
	size_t differs = 0, far = 0, outside = 0;
	int bad_x = 0, bad_y = 0;
	uint32_t got = 0, want = 0;
	int bx0 = t->x0 & ~15, by0 = t->y0 & ~15;
	int bx1 = (t->x1 + 15) & ~15, by1 = (t->y1 + 15) & ~15;
	bx1 = bx1 > (int)t->width ? (int)t->width : bx1;
	by1 = by1 > (int)t->height ? (int)t->height : by1;

	for (int y = by0; y < by1; y++) {
		const uint32_t *gpu = (const uint32_t *)(shadow_cpu + (size_t)y * t->stride);
		const uint32_t *cpu = (const uint32_t *)(t->dest + (size_t)y * t->stride);
		for (int x = bx0; x < bx1; x++) {
			if (gpu[x] == cpu[x]) {
				continue;
			}
			differs++;
			for (int shift = 0; shift < 32; shift += 8) {
				int a = (int)((gpu[x] >> shift) & 255), b = (int)((cpu[x] >> shift) & 255);
				if (a - b > TOLERANCE || b - a > TOLERANCE) {
					if (x < t->x0 || x >= t->x1 || y < t->y0 || y >= t->y1) {
						outside++;
					}
					if (!far++) {
						bad_x = x; bad_y = y; got = gpu[x]; want = cpu[x];
					}
					break;
				}
			}
		}
	}
	if (far) {
		stat->wrong++;
		if (stat->wrong_logged < WRONG_PER_KEY) {
			stat->wrong_logged++;
			float u0, v0, u1, v1;
			memcpy(&u0, &t->quads[0].u0, 4);
			memcpy(&v0, &t->quads[0].v0, 4);
			memcpy(&u1, &t->quads[0].u1, 4);
			memcpy(&v1, &t->quads[0].v1, 4);
			log_line("X kind=%u blend=%u/%u fmt=%u quads=%u rect=%d,%d-%d,%d of %ux%u up=%d tex=%ux%u stride=%u lin=%u uv=%d,%d-%d,%d op=%d self=%d off=%zu/%d out=%zu at %d,%d gpu=%08x cpu=%08x",
			    stat->key.kind, stat->key.blend, stat->key.blend_on, stat->key.format, t->quad_count, t->x0, t->y0, t->x1, t->y1,
			    t->width, t->height, t->dest_bottom_up, t->quads[0].texture_width, t->quads[0].texture_height,
			    t->quads[0].texture_stride, t->quads[0].linear, (int)(u0 * 1000), (int)(v0 * 1000),
			    (int)(u1 * 1000), (int)(v1 * 1000), (int)(t->opacity * 100), t->texture == t->dest, far,
			    (t->x1 - t->x0) * (t->y1 - t->y0), outside, bad_x, bad_y, got, want);
		}
	} else if (differs) {
		stat->close++;
	} else {
		stat->exact++;
	}
}

static void
draw_hook(void *ctx, uint32_t mode, uint32_t count, const uint16_t *indices, const void *vertices,
    uint32_t a, uint32_t b, void *clip)
{
	const uint8_t *state = FIELD(ctx, CX_STATE, const uint8_t *);
	struct draw_key key = { state[ST_KIND], state[ST_BLEND], state[ST_BLENDON], state[ST_IMAGE], 0, 0,
		FIELD(state, ST_UNIT, uint16_t), FIELD(state, ST_UNIT + 2, uint16_t) };
	if (key.unit0 == 1 || key.unit0 == 2) {
		const uint8_t *unit = FIELD(state, ST_BIND, const uint8_t *);
		const uint8_t *texture = unit != NULL ? FIELD(unit, IM_TEXTURE, const uint8_t *) : NULL;
		key.format = texture != NULL ? FIELD(texture, TX_FORMAT, uint8_t) : 255;
	}
	struct draw_stat *stat = stat_for(&key);
	struct translation t;
	uint64_t gpu = 0;
	int sampled = 0, owner = 0;

	/* Render threads share the pass and shadow buffers: one at a time. */
	int want_shadow = stat != NULL && compared_in_window < SHADOW_PER_WINDOW && stat->compared < SHADOW_PER_KEY;
	int may_go_live = stat != NULL && live_kind(&key) && stat->compared >= LIVE_SAMPLES && stat->wrong == 0;
	int pending = __atomic_load_n(&batch.active, __ATOMIC_ACQUIRE);
	if (pending) {
		/* A batch is open: this draw either joins it or ends it before it touches memory. */
		while (__atomic_exchange_n(&shadow_busy, 1, __ATOMIC_ACQUIRE)) {
		}
		owner = 1;
	}
	/*
	 * In 0147, 445 of 523 batches ended at a CPU draw between two GPU draws. A
	 * draw that is exact on the GPU but cheaper on the CPU alone, a solid fill or a
	 * small draw, still joins an open batch to its own destination: inside a pass
	 * it costs no flush, and it keeps the batch alive.
	 */
	int may_join = pending && stat != NULL && stat->compared >= LIVE_SAMPLES && stat->wrong == 0 &&
	    (live_kind(&key) || (key.kind == 0 && key.unit0 == 0 && key.blend == BLEND_SOVER));
	if (shadow_ready && !shadow_disabled && (want_shadow || may_go_live || may_join) &&
	    (owner || !__atomic_exchange_n(&shadow_busy, 1, __ATOMIC_ACQUIRE))) {
		owner = 1;
		enum reject why = translate(ctx, mode, count, indices, vertices, clip, &key, &t);
		int joins = may_join && why == R_NONE && batch.active && batch.dest == t.dest && batch.stride == t.stride &&
		    batch.width == t.width && batch.height == t.height && t.texture != t.dest &&
		    batch.pass.draws + t.quad_count < PASS_DRAWS;
		if (why != R_NONE) {
			rejects[why]++;
			stat->why = why;
			batch_commit();
		} else if (joins || (may_go_live && (t.x1 - t.x0) * (t.y1 - t.y0) >= LIVE_MIN_PIXELS && live_worthwhile(&key, &t))) {
			uint64_t ticks = batch_append(&t);
			if (ticks) {
				/* What the CPU path does after drawing. */
				marked_destination_function(ctx);
				FIELD(ctx, CX_DRAW_COUNTS, uint32_t) += 1;
				FIELD(ctx, CX_DRAW_COUNTS + 4, uint32_t) += count;
				stat->live++;
				stat->live_ticks += ticks;
				stat->live_pixels += (uint64_t)(t.x1 - t.x0) * (uint64_t)(t.y1 - t.y0);
				stat->draws++;
				ticks_in_draws += ticks;
				draws_total++;
				stat->ticks += ticks;
				__atomic_store_n(&shadow_busy, 0, __ATOMIC_RELEASE);
				return;
			}
			stat->live_failed++;
			batch_commit();
		} else {
			batch_commit();
			if (want_shadow) {
				sampled = 1;
				gpu = gpu_draw(&t, 0);
			}
		}
	} else if (owner) {
		batch_commit();
	}
	uint64_t begin = mach_absolute_time();
	b2l_draw_original(ctx, mode, count, indices, vertices, a, b, clip);
	uint64_t end = mach_absolute_time();
	ticks_in_draws += end - begin;
	draws_total++;
	if (stat != NULL) {
		stat->draws++;
		stat->ticks += end - begin;
		if (sampled) {
			compared_in_window++;
			if (gpu) {
				stat->compared++;
				stat->gpu_ticks += gpu;
				stat->cpu_ticks += end - begin;
				stat->pixels += (uint64_t)(t.x1 - t.x0) * (uint64_t)(t.y1 - t.y0);
				compare(&t, stat);
			} else {
				stat->failed++;
			}
		}
	}
	if (owner) {
		__atomic_store_n(&shadow_busy, 0, __ATOMIC_RELEASE);
	}
	if (end - window_start > 5ull * 1000000000ull * timebase.denom / timebase.numer) {
		report(mach_absolute_time());
	}
}

static uint64_t
quartzcore_slide(void)
{
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		if (strstr(_dyld_get_image_name(i), "/QuartzCore.framework/QuartzCore")) {
			return (uint64_t)_dyld_get_image_header(i) - QC_BASE;
		}
	}
	return 0;
}

static int
install_hook(void)
{
	uuid_t uuid;
	uint64_t slide = quartzcore_slide();
	if (!slide || !_dyld_get_shared_cache_uuid(uuid) || memcmp(uuid, CACHE_UUID, 16)) {
		log_line("hook: not cache 23G90");
		return 0;
	}
	uint32_t *target = (uint32_t *)(DRAW_ENTRY + slide);
	if (memcmp(target, ENTRY_WORDS, sizeof(ENTRY_WORDS))) {
		log_line("hook: entry bytes differ %08x %08x", target[0], target[1]);
		return 0;
	}
	b2l_draw_resume = (uint64_t)(target + 4);
	prepare_destination_function = (void (*)(void *))(PREPARE_DESTINATION + slide);
	marked_destination_function = (void (*)(void *))(MARKED_DESTINATION + slide);
	vm_address_t page = (vm_address_t)target & ~(vm_address_t)(vm_page_size - 1);
	kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr) {
		log_line("hook: vm_protect write failed %d", kr);
		return 0;
	}
	/* ldr x16, #8; br x16; .quad hook */
	struct { uint32_t ldr, br; uint64_t address; } patch = { 0x58000050, 0xd61f0200, (uint64_t)draw_hook };
	memcpy(target, &patch, sizeof(patch));
	sys_icache_invalidate(target, sizeof(patch));
	kr = vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	log_line("hook: installed at %p, protect=%d", target, kr);
	if (kr) {
		return 0;
	}
	for (unsigned i = 0; i < sizeof(prehooks) / sizeof(prehooks[0]); i++) {
		uint32_t *at = (uint32_t *)(prehooks[i].address + slide);
		if (memcmp(at, prehooks[i].words, 16)) {
			log_line("hook: %s entry bytes differ %08x %08x", prehooks[i].name, at[0], at[1]);
			return 0;
		}
	}
	for (unsigned i = 0; i < sizeof(prehooks) / sizeof(prehooks[0]); i++) {
		uint32_t *at = (uint32_t *)(prehooks[i].address + slide);
		vm_address_t p = (vm_address_t)at & ~(vm_address_t)(vm_page_size - 1);
		*prehooks[i].resume = (uint64_t)(at + 4);
		if (vm_protect(mach_task_self(), p, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY)) {
			log_line("hook: %s not writable", prehooks[i].name);
			return 0;
		}
		struct { uint32_t ldr, br; uint64_t address; } pre = { 0x58000050, 0xd61f0200, (uint64_t)prehooks[i].stub };
		memcpy(at, &pre, sizeof(pre));
		sys_icache_invalidate(at, sizeof(pre));
		vm_protect(mach_task_self(), p, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	}
	log_line("hook: %u pre-hooks installed", (unsigned)(sizeof(prehooks) / sizeof(prehooks[0])));
	return 1;
}

static int
create_buffer(uint64_t bytes, uint64_t flags, uint64_t *va, uint32_t *type, uint8_t **cpu)
{
	uint64_t in[2] = { bytes, flags }, out[2] = { 0, 0 };
	uint32_t count = 2;
	mach_vm_address_t address = 0;
	mach_vm_size_t size = 0;

	if (IOConnectCallMethod(connection, kB2LMaliCreateBuffer, in, 2, NULL, 0, out, &count, NULL, NULL) ||
	    IOConnectMapMemory64(connection, (uint32_t)out[1], mach_task_self(), &address, &size, 1)) {
		return 0;
	}
	*va = out[0];
	*type = (uint32_t)out[1];
	*cpu = (uint8_t *)address;
	return 1;
}

static int
prepare_shadow(void)
{
	uint64_t info[3] = { 0, 0, 0 };
	uint32_t count = 3, type;
	uint8_t *code = NULL;

	if (IOConnectCallMethod(connection, kB2LMaliInfo, NULL, 0, NULL, 0, info, &count, NULL, NULL) ||
	    !create_buffer(B2L_MALI_PASS_BYTES(PASS_DRAWS), 0, &pass_va, &pass_type, &pass_cpu) ||
	    !create_buffer(4096, B2L_MALI_BUFFER_CODE, &code_va, &type, &code) ||
	    !create_buffer(SHADOW_BYTES, 0, &shadow_va, &type, &shadow_cpu) ||
	    !create_buffer(STAGE_BYTES, 0, &stage_va, &type, &stage_cpu)) {
		log_line("translator: buffers failed");
		return 0;
	}
	heap_va = info[0];
	heap_bytes = (uint32_t)info[1];
	memcpy(code + SHADER_TEXTURED, b2l_mali_shader_textured, sizeof(b2l_mali_shader_textured));
	memcpy(code + SHADER_SOLID, b2l_mali_shader_solid, sizeof(b2l_mali_shader_solid));
	memcpy(code + SHADER_MODULATED, b2l_mali_shader_modulated, sizeof(b2l_mali_shader_modulated));
	memcpy(code + SHADER_TINTED, b2l_mali_shader_tinted, sizeof(b2l_mali_shader_tinted));
	struct flush_rect page = { code, 4096, 4096, 1 };
	cache_flush(&page);
	return 1;
}

static void *
start_thread(void *unused)
{
	(void)unused;
	for (int attempt = 0; attempt < 120 && connection == MACH_PORT_NULL; attempt++) {
		io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("B2LMali"));
		if (service != MACH_PORT_NULL) {
			if (IOServiceOpen(service, mach_task_self(), 0, &connection) != KERN_SUCCESS) {
				connection = MACH_PORT_NULL;
			}
			IOObjectRelease(service);
		}
		if (connection == MACH_PORT_NULL) {
			sleep(1);
		}
	}
	if (connection == MACH_PORT_NULL) {
		return NULL;
	}
	log_line("library running in pid %d", getpid());
	mach_timebase_info(&timebase);
	window_start = mach_absolute_time();
	shadow_ready = prepare_shadow();
	install_hook();
	log_line("translator ready=%d", shadow_ready);
	return NULL;
}

__attribute__((constructor)) static void
b2l_gpu_start(void)
{
	pthread_t thread;
	pthread_attr_t attributes;

	pthread_attr_init(&attributes);
	pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	pthread_create(&thread, &attributes, start_thread, NULL);
	pthread_attr_destroy(&attributes);
}
