/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Mali-G76 job probe for the Android recovery kernel.
 *
 * Static AArch64 Linux program without libc. Runs one fragment job through
 * /dev/mali0 (Arm kbase) that clears a BGRA-sized surface, then checks every
 * pixel from the CPU. It proves the job header, framebuffer descriptor and
 * render target layouts that the XNU path will submit, on the real GPU and
 * without flashing anything. It needs no shader: the tiler list is empty, so
 * only the clear colour reaches the tiles.
 *
 * Usage: b2l-mali-probe [width height [runs]]
 *        b2l-mali-probe draw
 *        b2l-mali-probe flush
 *
 * "draw" runs tiler and fragment jobs with the project's Bifrost shaders: a
 * solid rectangle, a textured copy, alpha blending, opacity, linear filtering,
 * a BGRA target, a partial redraw and many-layer passes, then a timed
 * full-screen blend against a plain C loop.
 *
 * The job comes from b2l_mali_core.h, the builder the XNU driver uses; the
 * ioctls are kbase's public interface. Nothing here touches clocks or power;
 * the kernel driver owns both.
 */
#include <stdint.h>
#include <stddef.h>
#include "b2l_mali_core.h"
#include "b2l_mali_draw.h"

#define SYS_ioctl 29
#define SYS_openat 56
#define SYS_close 57
#define SYS_read 63
#define SYS_write 64
#define SYS_ppoll 73
#define SYS_exit 93
#define SYS_clock_gettime 113
#define SYS_mmap 222
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define AT_FDCWD (-100)
#define O_RDWR 2
#define O_CLOEXEC 0x80000
#define PROT_NONE 0
#define PROT_RW 3
#define MAP_SHARED 1
#define POLLIN 1
#define CLOCK_MONOTONIC 1

static long
sys6(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ volatile ("svc #0" : "+r"(x0)
	    : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
	    : "memory", "cc");
	return x0;
}

/* kbase ioctl interface, type 0x80, major 11. */
#define KBASE_VERSION_CHECK 0xC0048000UL
#define KBASE_SET_FLAGS     0x40048001UL
#define KBASE_JOB_SUBMIT    0x40108002UL
#define KBASE_MEM_ALLOC     0xC0208005UL
#define KBASE_TRACKING_PAGE (3UL << 12)

#define MEM_CPU_RD  (1UL << 0)
#define MEM_CPU_WR  (1UL << 1)
#define MEM_GPU_RD  (1UL << 2)
#define MEM_GPU_WR  (1UL << 3)
#define MEM_SAME_VA (1UL << 13)
#define MEM_GPU_EX  (1UL << 4)
#define JD_REQ_FS   1U
#define JD_REQ_T    4U
#define JD_DEP_DATA 1U
#define JD_EVENT_DONE 1U

struct kbase_version { uint16_t major, minor; };
struct kbase_flags { uint32_t create_flags; };
union kbase_alloc {
	struct { uint64_t va_pages, commit_pages, extent, flags; } in;
	struct { uint64_t flags, gpu_va; } out;
};
struct kbase_submit { uint64_t addr; uint32_t nr_atoms, stride; };
struct kbase_atom {
	uint64_t jc, udata[2], extres_list;
	uint16_t nr_extres;
	uint8_t jit_id[2], pre_dep[4];
	uint8_t atom_number, prio, device_nr, jobslot;
	uint32_t core_req;
	uint8_t renderpass_id, padding[7];
};
_Static_assert(sizeof(struct kbase_atom) == 56, "base_jd_atom_v2 layout");
struct kbase_event {
	uint32_t code;
	uint8_t atom_number;
	uint64_t udata[2];
};

#define CLEAR_COLOUR 0xff2040c0U
#define MAX_DRAWS 64
#define SENTINEL 0x5aa55aa5U

static void
out(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	sys6(SYS_write, 1, (long)s, (long)n, 0, 0, 0);
}

static void
outu(uint64_t v, unsigned base)
{
	char b[24];
	unsigned i = sizeof(b);
	b[--i] = 0;
	do {
		b[--i] = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);
	out(b + i);
}

static void
kv(const char *key, uint64_t v, unsigned base)
{
	out(key);
	out(base == 16 ? "=0x" : "=");
	outu(v, base);
	out(" ");
}

static uint64_t
now_ns(void)
{
	long ts[2];
	sys6(SYS_clock_gettime, CLOCK_MONOTONIC, (long)ts, 0, 0, 0, 0);
	return (uint64_t)ts[0] * 1000000000ULL + (uint64_t)ts[1];
}

static void *
gpu_alloc_flags(int fd, size_t bytes, uint64_t flags)
{
	size_t pages = (bytes + 4095) >> 12;
	union kbase_alloc a = { .in = { pages, pages, 0, flags | MEM_SAME_VA } };
	long r = sys6(SYS_ioctl, fd, KBASE_MEM_ALLOC, (long)&a, 0, 0, 0);
	if (r) {
		kv("mem_alloc_error", (uint64_t)-r, 10);
		return NULL;
	}
	/* With SAME_VA the "address" is a cookie; mmap gives the shared VA. */
	r = sys6(SYS_mmap, 0, pages << 12, PROT_RW, MAP_SHARED, fd, (long)a.out.gpu_va);
	if (r < 0 && r > -4096) {
		kv("mem_map_error", (uint64_t)-r, 10);
		return NULL;
	}
	return (void *)r;
}

static void *
gpu_alloc(int fd, size_t bytes)
{
	return gpu_alloc_flags(fd, bytes, MEM_CPU_RD | MEM_CPU_WR | MEM_GPU_RD | MEM_GPU_WR);
}

static int
run_once(int fd, uint8_t *page, uint32_t *rt, uint32_t width, uint32_t height,
    uint32_t stride, unsigned run)
{
	size_t words = (size_t)stride / 4 * height;
	for (size_t i = 0; i < words; i++) {
		rt[i] = SENTINEL;
	}
	b2l_mali_clear_job(page, (uint64_t)page, (uint64_t)rt, width, height, stride,
	    CLEAR_COLOUR);

	struct kbase_atom atom = {
		.jc = (uint64_t)page,
		.udata = { run, 0 },
		.atom_number = 1,
		.core_req = JD_REQ_FS,
	};
	struct kbase_submit submit = { (uint64_t)&atom, 1, sizeof(atom) };
	uint64_t start = now_ns();
	long r = sys6(SYS_ioctl, fd, KBASE_JOB_SUBMIT, (long)&submit, 0, 0, 0);
	if (r) {
		kv("job_submit_error", (uint64_t)-r, 10);
		return 1;
	}

	struct { int fd; short events, revents; } pfd = { fd, POLLIN, 0 };
	long timeout[2] = { 5, 0 };
	r = sys6(SYS_ppoll, (long)&pfd, 1, (long)timeout, 0, 0, 0);
	if (r <= 0) {
		kv("event_timeout", (uint64_t)-r, 10);
		return 1;
	}
	struct kbase_event ev = { 0 };
	r = sys6(SYS_read, fd, (long)&ev, sizeof(ev), 0, 0, 0);
	uint64_t elapsed = now_ns() - start;

	size_t bad = 0, first = (size_t)-1;
	for (size_t i = 0; i < words; i++) {
		if ((i % (stride / 4)) < width && rt[i] != CLEAR_COLOUR) {
			if (!bad++) {
				first = i;
			}
		}
	}
	uint32_t *job = (uint32_t *)page;
	kv("run", run, 10);
	kv("event", ev.code, 16);
	kv("atom", ev.atom_number, 10);
	kv("job_status", job[0], 16);
	kv("ns", elapsed, 10);
	kv("mpix_s", (uint64_t)width * height * 1000 / (elapsed ? elapsed : 1), 10);
	kv("bad_pixels", bad, 10);
	if (bad) {
		kv("first_bad", first, 10);
		kv("value", rt[first], 16);
	}
	out("\n");
	return ev.code != JD_EVENT_DONE || bad;
}


/* Tiler atom, then a fragment atom that depends on it. */
static uint64_t
submit_frame(int fd, uint8_t *frame, uint32_t *status)
{
	struct kbase_atom atoms[2] = {
		{ .jc = (uint64_t)frame + B2L_MALI_PASS_DRAWS, .atom_number = 1, .core_req = JD_REQ_T },
		{ .jc = (uint64_t)frame + B2L_MALI_PASS_FRAGMENT, .atom_number = 2,
		  .core_req = JD_REQ_FS, .pre_dep = { 1, JD_DEP_DATA } },
	};
	struct kbase_submit submit = { (uint64_t)atoms, 2, sizeof(atoms[0]) };
	uint64_t start = now_ns();
	long r = sys6(SYS_ioctl, fd, KBASE_JOB_SUBMIT, (long)&submit, 0, 0, 0);

	*status = 0xffffffffu;
	if (r) {
		kv("job_submit_error", (uint64_t)-r, 10);
		return 0;
	}
	uint32_t worst = JD_EVENT_DONE;
	for (int i = 0; i < 2; i++) {
		struct { int fd; short events, revents; } pfd = { fd, POLLIN, 0 };
		long timeout[2] = { 5, 0 };
		struct kbase_event ev = { 0 };

		if (sys6(SYS_ppoll, (long)&pfd, 1, (long)timeout, 0, 0, 0) <= 0) {
			kv("event_timeout", (uint64_t)i, 10);
			return 0;
		}
		sys6(SYS_read, fd, (long)&ev, sizeof(ev), 0, 0, 0);
		if (ev.code != JD_EVENT_DONE) {
			worst = ev.code;
			kv("atom", ev.atom_number, 10);
			kv("event", ev.code, 16);
		}
	}
	*status = worst;
	return now_ns() - start;
}

static uint32_t
F(float value)
{
	union { float f; uint32_t u; } v;
	v.f = value;
	return v.u;
}

struct draw_context {
	int fd;
	uint8_t *frame, *code;
	uint32_t *rt, *texture, *heap;
	struct b2l_mali_target target;
	struct b2l_mali_pass pass;
};

static int
run_pass(struct draw_context *c, const char *name, uint64_t *ns)
{
	uint32_t status;

	*ns = submit_frame(c->fd, c->frame, &status);
	if (status != JD_EVENT_DONE) {
		out(name);
		kv(" tiler_status", ((uint32_t *)(c->frame + B2L_MALI_PASS_DRAWS))[0], 16);
		kv("frag_status", ((uint32_t *)(c->frame + B2L_MALI_PASS_FRAGMENT))[0], 16);
		out("FAIL\n");
		return 1;
	}
	return 0;
}

static int
draw_quad(struct draw_context *c, struct b2l_mali_quad *q, const char *name, uint64_t *ns)
{
	b2l_mali_pass_begin(&c->pass, c->frame, (uint64_t)c->frame, &c->target);
	b2l_mali_pass_add(&c->pass, q);
	return run_pass(c, name, ns);
}

/* Rectangle [x0,x1) x [y0,y1) holds `expect(x, y)`, the rest the clear colour. */
static int
check_rect(struct draw_context *c, const char *name, uint32_t x0, uint32_t y0, uint32_t x1,
    uint32_t y1, uint32_t (*expect)(uint32_t, uint32_t), int tolerance, uint64_t ns)
{
	size_t bad = 0;
	uint32_t first_x = 0, first_y = 0, got = 0, want = 0;

	for (uint32_t y = 0; y < c->target.height; y++) {
		for (uint32_t x = 0; x < c->target.width; x++) {
			uint32_t value = c->rt[(size_t)y * (c->target.stride / 4) + x];
			int inside = x >= x0 && x < x1 && y >= y0 && y < y1;
			uint32_t wanted = inside ? expect(x - x0, y - y0) : c->target.clear;
			int mismatch = 0;

			for (int shift = 0; shift < 32; shift += 8) {
				int a = (int)((value >> shift) & 255), b = (int)((wanted >> shift) & 255);
				mismatch |= a - b > tolerance || b - a > tolerance;
			}
			if (mismatch && !bad++) {
				first_x = x; first_y = y; got = value; want = wanted;
			}
		}
	}
	out(name);
	out(" ");
	kv("ns", ns, 10);
	kv("bad_pixels", bad, 10);
	if (bad) {
		kv("x", first_x, 10);
		kv("y", first_y, 10);
		kv("got", got, 16);
		kv("want", want, 16);
	}
	out(bad ? "FAIL\n" : "PASS\n");
	return bad != 0;
}

static uint32_t expect_green(uint32_t x, uint32_t y) { (void)x; (void)y; return 0xff00ff00u; }
static uint32_t expect_ramp(uint32_t x, uint32_t y) { return 0xff550000u | (y << 8) | x; }
/* 0x80404040 premultiplied over 0xff2040c0, in R8G8B8A8 little-endian order. */
static uint32_t expect_bgra(uint32_t x, uint32_t y) { return 0xff000055u | (x << 16) | (y << 8); }
/* 0.5 white premultiplied over green: R=B=0x80, G=0xff, A=0xff. */
static uint32_t expect_half_white(uint32_t x, uint32_t y) { (void)x; (void)y; return 0xff80ff80u; }
static uint32_t expect_linear(uint32_t x, uint32_t y)
{
	(void)y;
	uint32_t v = (2 * x + 1) * 255 / 512;
	return 0xff000000u | v | (v << 8) | (v << 16);
}
static uint32_t expect_tint(uint32_t x, uint32_t y) { (void)x; (void)y; return 0x80802040u; }
static uint32_t expect_over(uint32_t x, uint32_t y) { (void)x; (void)y; return 0xff5060a0u; }

static void
c_src_over(uint32_t *dst, const uint32_t *src, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		uint32_t s = src[i], d = dst[i], inverse = 255 - (s >> 24), out_px = 0;

		for (int shift = 0; shift < 32; shift += 8) {
			uint32_t channel = ((s >> shift) & 255) + (((d >> shift) & 255) * inverse + 127) / 255;
			out_px |= (channel > 255 ? 255 : channel) << shift;
		}
		dst[i] = out_px;
	}
}

static int
draw_tests(void)
{
	const uint32_t width = 1440, height = 2904, stride = width * 4;
	const size_t surface = (size_t)stride * height;
	const uint64_t ex = MEM_CPU_RD | MEM_CPU_WR | MEM_GPU_RD | MEM_GPU_EX;
	struct draw_context c = { 0 };
	struct kbase_version version = { 11, 25 };
	struct kbase_flags flags = { 0 };
	uint64_t ns;
	int failed = 0;

	c.fd = (int)sys6(SYS_openat, AT_FDCWD, (long)"/dev/mali0", O_RDWR | O_CLOEXEC, 0, 0, 0);
	if (c.fd < 0 || sys6(SYS_ioctl, c.fd, KBASE_VERSION_CHECK, (long)&version, 0, 0, 0) ||
	    sys6(SYS_ioctl, c.fd, KBASE_SET_FLAGS, (long)&flags, 0, 0, 0)) {
		out("kbase setup failed\n");
		return 1;
	}
	sys6(SYS_mmap, 0, 4096, PROT_NONE, MAP_SHARED, c.fd, KBASE_TRACKING_PAGE);
	const uint32_t heap_bytes = 16u << 20;
	c.frame = gpu_alloc(c.fd, B2L_MALI_PASS_BYTES(MAX_DRAWS));
	c.code = gpu_alloc_flags(c.fd, 4096, ex);
	c.rt = gpu_alloc(c.fd, surface);
	c.texture = gpu_alloc(c.fd, surface);
	c.heap = gpu_alloc(c.fd, heap_bytes);
	if (!c.frame || !c.code || !c.rt || !c.texture || !c.heap) {
		out("allocation failed\n");
		return 1;
	}
	c.target = (struct b2l_mali_target){ (uint64_t)c.rt, width, height, stride, CLEAR_COLOUR,
		B2L_RT_RGBA, (uint64_t)c.heap, heap_bytes };

	uint64_t *solid = (uint64_t *)c.code, *textured = solid + 64, *modulated = solid + 128, *tinted = solid + 192;
	for (size_t i = 0; i < sizeof(b2l_mali_shader_solid) / 8; i++) {
		solid[i] = b2l_mali_shader_solid[i];
	}
	for (size_t i = 0; i < sizeof(b2l_mali_shader_textured) / 8; i++) {
		textured[i] = b2l_mali_shader_textured[i];
	}

	for (size_t i = 0; i < sizeof(b2l_mali_shader_modulated) / 8; i++) {
		modulated[i] = b2l_mali_shader_modulated[i];
	}

	for (size_t i = 0; i < sizeof(b2l_mali_shader_tinted) / 8; i++) {
		tinted[i] = b2l_mali_shader_tinted[i];
	}

	struct b2l_mali_quad q = {
		F(100), F(200), F(300), F(400), F(0), F(0), F(1), F(1), { F(0), F(1), F(0), F(1) },
		(uint64_t)solid, 0, 0, 0, 0, 0, B2L_EQ_COPY, 2, 0, 0,
	};
	if (draw_quad(&c, &q, "solid", &ns)) {
		return 1;
	}
	failed |= check_rect(&c, "solid", 100, 200, 300, 400, expect_green, 0, ns);

	/* BGRA memory, as an IOSurface: 0xAARRGGBB with R=x, G=y, B=0x55. */
	for (uint32_t y = 0; y < 256; y++) {
		for (uint32_t x = 0; x < 256; x++) {
			c.texture[y * 256 + x] = 0xff000055u | (x << 16) | (y << 8);
		}
	}
	q = (struct b2l_mali_quad){
		F(100), F(200), F(356), F(456), F(0), F(0), F(1), F(1), { F(0), F(0), F(0), F(0) },
		(uint64_t)textured, (uint64_t)c.texture, 256, 256, 1024, B2L_FORMAT_BGRA8_UNORM,
		B2L_EQ_COPY, 0, 1, 0,
	};
	if (draw_quad(&c, &q, "textured", &ns)) {
		return 1;
	}
	failed |= check_rect(&c, "textured", 100, 200, 356, 456, expect_ramp, 0, ns);

	for (uint32_t i = 0; i < 256 * 256; i++) {
		c.texture[i] = 0x80404040u;
	}
	q.equation = B2L_EQ_SRC_OVER;
	if (draw_quad(&c, &q, "src_over", &ns)) {
		return 1;
	}
	failed |= check_rect(&c, "src_over", 100, 200, 356, 456, expect_over, 2, ns);

	/* A one-byte mask times a full colour, the way text is drawn. */
	uint8_t *mask = (uint8_t *)c.texture;
	for (uint32_t i = 0; i < 256 * 256; i++) {
		mask[i] = 128;
	}
	struct b2l_mali_quad tint = q;
	tint.colour[0] = F(0.5f);
	tint.colour[1] = F(0.25f);
	tint.colour[2] = F(1);
	tint.colour[3] = F(1);
	tint.shader_va = (uint64_t)tinted;
	tint.texture_stride = 256;
	tint.texture_format = B2L_FORMAT_R8_UNORM;
	tint.equation = B2L_EQ_COPY;
	tint.fau_slots = 2;
	tint.first_clause_blends = 0;
	tint.texture_mask = 1;
	if (draw_quad(&c, &tint, "tinted", &ns)) {
		return 1;
	}
	failed |= check_rect(&c, "tinted", 100, 200, 356, 456, expect_tint, 2, ns);

	/* One full-screen translucent layer, the UI's common case. */
	for (size_t i = 0; i < surface / 4; i++) {
		c.texture[i] = 0x80404040u;
	}
	q = (struct b2l_mali_quad){
		F(0), F(0), F((float)width), F((float)height), F(0), F(0), F(1), F(1), { F(0), F(0), F(0), F(0) },
		(uint64_t)textured, (uint64_t)c.texture, width, height, stride,
		B2L_FORMAT_BGRA8_UNORM, B2L_EQ_SRC_OVER, 0, 1, 0,
	};
	for (int run = 0; run < 4; run++) {
		if (draw_quad(&c, &q, "full", &ns)) {
			return 1;
		}
		out("full_screen_blend ");
		kv("ns", ns, 10);
		kv("mpix_s", (uint64_t)width * height * 1000 / ns, 10);
		out("\n");
	}
	failed |= check_rect(&c, "full", 0, 0, width, height, expect_over, 2, ns);

	/* A BGRA target, as iOS uses: memory must equal the BGRA texture exactly. */
	for (uint32_t y = 0; y < 256; y++) {
		for (uint32_t x = 0; x < 256; x++) {
			c.texture[y * 256 + x] = 0xff000055u | (x << 16) | (y << 8);
		}
	}
	c.target.swizzle = B2L_RT_BGRA;
	c.target.clear = 0xff2040c0u;   /* tile buffer order: R=c0 G=40 B=20 */
	q = (struct b2l_mali_quad){
		F(100), F(200), F(356), F(456), F(0), F(0), F(1), F(1), { F(0), F(0), F(0), F(0) },
		(uint64_t)textured, (uint64_t)c.texture, 256, 256, 1024, B2L_FORMAT_BGRA8_UNORM,
		B2L_EQ_COPY, 0, 1, 0,
	};
	if (draw_quad(&c, &q, "bgra", &ns)) {
		return 1;
	}
	c.target.clear = 0xffc04020u;   /* the same clear as BGRA memory */
	failed |= check_rect(&c, "bgra_target", 100, 200, 356, 456, expect_bgra, 0, ns);

	/*
	 * Partial redraw. Tiles do not load memory, so a pass that keeps old pixels
	 * first draws the target onto itself, then the new layer on top.
	 */
	c.target.clear = 0xff00ff00u;
	b2l_mali_pass_begin(&c.pass, c.frame, (uint64_t)c.frame, &c.target);
	q = (struct b2l_mali_quad){
		F(0), F(0), F((float)width), F((float)height), F(0), F(0), F(1), F(1), { F(0), F(0), F(0), F(0) },
		(uint64_t)textured, (uint64_t)c.rt, width, height, stride,
		B2L_FORMAT_BGRA8_UNORM, B2L_EQ_COPY, 0, 1, 0,
	};
	b2l_mali_pass_add(&c.pass, &q);
	struct b2l_mali_quad red = {
		F(400), F(800), F(500), F(900), F(0), F(0), F(1), F(1), { F(1), F(0), F(0), F(1) },
		(uint64_t)solid, 0, 0, 0, 0, 0, B2L_EQ_COPY, 2, 0, 0,
	};
	b2l_mali_pass_add(&c.pass, &red);
	if (run_pass(&c, "preserve", &ns)) {
		return 1;
	}
	size_t lost = 0, wrong = 0;
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			uint32_t v = c.rt[(size_t)y * width + x], want = 0xffc04020u;

			if (x >= 100 && x < 356 && y >= 200 && y < 456) {
				want = expect_bgra(x - 100, y - 200);
			}
			if (x >= 400 && x < 500 && y >= 800 && y < 900) {
				want = 0xffff0000u;
				wrong += v != want;
			} else {
				lost += v != want;
			}
		}
	}
	out("preserve_old_pixels ");
	kv("ns", ns, 10);
	kv("lost", lost, 10);
	kv("new_wrong", wrong, 10);
	out(lost || wrong ? "FAIL\n" : "PASS\n");
	failed |= lost || wrong;

	/* Layer opacity: white at 0.5 over the green clear. */
	c.target.clear = 0xff00ff00u;
	for (size_t i = 0; i < 256 * 256; i++) {
		c.texture[i] = 0xffffffffu;
	}
	q = (struct b2l_mali_quad){
		F(100), F(200), F(356), F(456), F(0), F(0), F(1), F(1), { F(0.5f), F(0), F(0), F(0) },
		(uint64_t)modulated, (uint64_t)c.texture, 256, 256, 1024, B2L_FORMAT_BGRA8_UNORM,
		B2L_EQ_SRC_OVER, 1, 0, 0,
	};
	if (draw_quad(&c, &q, "opacity", &ns)) {
		return 1;
	}
	failed |= check_rect(&c, "opacity", 100, 200, 356, 456, expect_half_white, 2, ns);

	/* Linear filtering: two texels stretched over 256 pixels make a ramp. */
	c.texture[0] = 0xff000000u;
	c.texture[1] = 0xffffffffu;
	q = (struct b2l_mali_quad){
		F(100), F(200), F(356), F(456), F(0.25f), F(0), F(0.75f), F(1), { F(0), F(0), F(0), F(0) },
		(uint64_t)textured, (uint64_t)c.texture, 2, 1, 64, B2L_FORMAT_BGRA8_UNORM,
		B2L_EQ_COPY, 0, 1, 1,
	};
	if (draw_quad(&c, &q, "linear", &ns)) {
		return 1;
	}
	failed |= check_rect(&c, "linear", 100, 200, 356, 456, expect_linear, 2, ns);

	/* A UI-sized frame: many translucent full-screen layers in one pass. */
	for (size_t i = 0; i < surface / 4; i++) {
		c.texture[i] = 0x10101010u;
	}
	for (unsigned layers = 1; layers <= MAX_DRAWS; layers *= 4) {
		uint64_t best = ~0ull;

		q = (struct b2l_mali_quad){
			F(0), F(0), F((float)width), F((float)height), F(0), F(0), F(1), F(1), { F(0), F(0), F(0), F(0) },
			(uint64_t)textured, (uint64_t)c.texture, width, height, stride,
			B2L_FORMAT_BGRA8_UNORM, B2L_EQ_SRC_OVER, 0, 1, 0,
		};
		for (int run = 0; run < 3; run++) {
			b2l_mali_pass_begin(&c.pass, c.frame, (uint64_t)c.frame, &c.target);
			for (unsigned i = 0; i < layers; i++) {
				b2l_mali_pass_add(&c.pass, &q);
			}
			if (run_pass(&c, "layers", &ns)) {
				return 1;
			}
			best = ns < best ? ns : best;
		}
		/* Green clear with `layers` of 0x10 grey at alpha 0x10, in 8-bit steps. */
		uint32_t channel[3] = { 0, 255, 0 }, expected = 0xff000000u;
		size_t uneven = 0, off = 0;
		uint32_t got = c.rt[0];

		for (unsigned i = 0; i < layers; i++) {
			for (int k = 0; k < 3; k++) {
				channel[k] = 16 + (channel[k] * 239 + 127) / 255;
			}
		}
		expected |= channel[2] | (channel[1] << 8) | (channel[0] << 16);
		for (size_t i = 0; i < surface / 4; i++) {
			uneven += c.rt[i] != got;
		}
		for (int shift = 0; shift < 24; shift += 8) {
			int a = (int)((got >> shift) & 255), b = (int)((expected >> shift) & 255);
			off |= a - b > 8 || b - a > 8;
		}
		out("layers ");
		kv("count", layers, 10);
		kv("ns", best, 10);
		kv("mpix_s", (uint64_t)width * height * layers * 1000 / best, 10);
		kv("pixel", got, 16);
		kv("expected", expected, 16);
		kv("uneven", uneven, 10);
		out(off || uneven ? "FAIL\n" : "PASS\n");
		failed |= off || uneven;
	}

	/* The same blend on the CPU, over ordinary cached memory. */
	long a = sys6(SYS_mmap, 0, surface, PROT_RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	long b = sys6(SYS_mmap, 0, surface, PROT_RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if ((a < 0 && a > -4096) || (b < 0 && b > -4096)) {
		out("cpu baseline allocation failed\n");
		return 1;
	}
	for (size_t i = 0; i < surface / 4; i++) {
		((uint32_t *)a)[i] = CLEAR_COLOUR;
		((uint32_t *)b)[i] = 0x80404040u;
	}
	uint64_t start = now_ns();
	c_src_over((uint32_t *)a, (const uint32_t *)b, surface / 4);
	ns = now_ns() - start;
	out("cpu_c_loop_blend ");
	kv("ns", ns, 10);
	kv("mpix_s", (uint64_t)width * height * 1000 / ns, 10);
	kv("check", ((uint32_t *)a)[12345], 16);
	out("\n");
	out(failed ? "RESULT FAIL\n" : "RESULT PASS\n");
	return failed;
}

/* What keeping a CPU-cached surface coherent with the GPU costs. */
static uint64_t
flush_range(uint8_t *memory, size_t bytes)
{
	uint64_t start = now_ns();

	for (size_t i = 0; i < bytes; i += 64) {
		__asm__ volatile ("dc civac, %0" : : "r"(memory + i));
	}
	__asm__ volatile ("dsb sy");
	return now_ns() - start;
}

static int
flush_tests(void)
{
	const size_t bytes = (size_t)1440 * 4 * 2904;
	uint8_t *memory = (uint8_t *)sys6(SYS_mmap, 0, bytes, PROT_RW, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	for (size_t i = 0; i < bytes; i++) {
		memory[i] = (uint8_t)i;
	}
	for (int run = 0; run < 3; run++) {
		kv("after_write_ns", flush_range(memory, bytes), 10);
		kv("already_clean_ns", flush_range(memory, bytes), 10);
		uint64_t sum = 0;
		for (size_t i = 0; i < bytes; i += 64) {
			sum += memory[i];       /* every line now clean but cached */
		}
		kv("after_read_ns", flush_range(memory, bytes), 10);
		kv("sum", sum & 1, 10);
		for (size_t i = 0; i < bytes; i += 64) {
			memory[i] = (uint8_t)run;
		}
		out("\n");
	}
	/* A 256x256 layer inside the surface, row by row: a typical draw. */
	uint64_t start = now_ns();
	for (int repeat = 0; repeat < 100; repeat++) {
		for (size_t row = 0; row < 256; row++) {
			uint8_t *line = memory + (row + 200) * 5760 + 400;
			for (size_t i = 0; i < 1024; i += 64) {
				__asm__ volatile ("dc civac, %0" : : "r"(line + i));
			}
		}
		__asm__ volatile ("dsb sy");
	}
	kv("layer_256_ns", (now_ns() - start) / 100, 10);
	out("\n");
	return 0;
}

static uint32_t
parse(const char *s, uint32_t fallback)
{
	uint32_t v = 0;
	if (!s) {
		return fallback;
	}
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (uint32_t)(*s++ - '0');
	}
	return v ? v : fallback;
}

static int
probe(int argc, char **argv)
{
	uint32_t width = parse(argc > 1 ? argv[1] : NULL, 1440);
	uint32_t height = parse(argc > 2 ? argv[2] : NULL, 2904);
	uint32_t runs = parse(argc > 3 ? argv[3] : NULL, 4);
	uint32_t stride = (width * 4 + 63) & ~63U;

	long fd = sys6(SYS_openat, AT_FDCWD, (long)"/dev/mali0", O_RDWR | O_CLOEXEC, 0, 0, 0);
	if (fd < 0) {
		kv("open_error", (uint64_t)-fd, 10);
		out("\n");
		return 1;
	}
	struct kbase_version version = { 11, 25 };
	long r = sys6(SYS_ioctl, fd, KBASE_VERSION_CHECK, (long)&version, 0, 0, 0);
	kv("uk", version.major, 10);
	kv("minor", version.minor, 10);
	if (r) {
		kv("version_error", (uint64_t)-r, 10);
		out("\n");
		return 1;
	}
	struct kbase_flags flags = { 0 };
	r = sys6(SYS_ioctl, fd, KBASE_SET_FLAGS, (long)&flags, 0, 0, 0);
	if (r) {
		kv("flags_error", (uint64_t)-r, 10);
		out("\n");
		return 1;
	}
	r = sys6(SYS_mmap, 0, 4096, PROT_NONE, MAP_SHARED, fd, KBASE_TRACKING_PAGE);
	if (r < 0 && r > -4096) {
		kv("tracking_error", (uint64_t)-r, 10);
		out("\n");
		return 1;
	}

	uint8_t *page = gpu_alloc((int)fd, 4096);
	uint32_t *rt = gpu_alloc((int)fd, (size_t)stride * height);
	if (!page || !rt) {
		out("\n");
		return 1;
	}
	kv("width", width, 10);
	kv("height", height, 10);
	kv("stride", stride, 10);
	kv("job_va", (uint64_t)page, 16);
	kv("rt_va", (uint64_t)rt, 16);
	out("\n");

	int failed = 0;
	for (uint32_t run = 0; run < runs; run++) {
		failed |= run_once((int)fd, page, rt, width, height, stride, run);
	}
	sys6(SYS_close, fd, 0, 0, 0, 0, 0);
	out(failed ? "RESULT FAIL\n" : "RESULT PASS\n");
	return failed;
}

__attribute__((used)) void
b2l_start(long *stack)
{
	char **argv = (char **)(stack + 1);
	int status;

	if (stack[0] > 1 && argv[1][0] == 'd') {
		status = draw_tests();
	} else if (stack[0] > 1 && argv[1][0] == 'f') {
		status = flush_tests();
	} else {
		status = probe((int)stack[0], argv);
	}
	sys6(SYS_exit, status, 0, 0, 0, 0, 0);
}

__asm__(".globl _start\n_start:\n mov x0, sp\n and sp, x0, #~15\n b b2l_start\n");
