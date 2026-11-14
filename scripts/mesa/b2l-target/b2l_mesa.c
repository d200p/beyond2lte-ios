// SPDX-License-Identifier: MIT
/*
 * S10 port: Mesa on iOS over B2LMali. A minimal GL frontend in the manner of
 * OSMesa: Panfrost screens on the B2LMali libdrm, OpenGL ES contexts that
 * draw into pipe resources this file owns. It is the base of the EAGL layer
 * that will serve Core Animation; for now b2l_mesa_probe() tests it on the
 * phone: OpenGL ES 3 contexts, a clear and a shader-drawn triangle into
 * 64 x 64 RGBA targets, read back and checked, each step logged to the
 * kernel log ("B2L GPU UI: mesa: ...").
 */
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "util/glheader.h"
#include "state_tracker/st_context.h"
#include "frontend/api.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/u_atomic.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/driconf.h"
#include "util/xmlconfig.h"
#include "glapi/glapi.h"
#include "main/dispatch.h"
#include "panfrost/drm/panfrost_drm_public.h"
#include "panfrost/pan_public.h"
#include <stdlib.h>
#include "renderonly/renderonly.h"
#include "b2l_drm.h"

#define B2L_EXPORT __attribute__((visibility("default")))

B2L_EXPORT int b2l_mesa_probe(void);
#define PROBE_SIZE 64

/* Render-only (a separate KMS device) is never used here: libgallium leaves
 * renderonly.c out without libdrm, so Panfrost's two references end here. */
struct renderonly_scanout *
renderonly_create_gpu_import_for_resource(struct pipe_resource *rsc, struct renderonly *ro,
    struct winsys_handle *out_handle)
{
	(void)rsc;
	(void)ro;
	(void)out_handle;
	return NULL;
}

void
renderonly_scanout_destroy(struct renderonly_scanout *scanout, struct renderonly *ro)
{
	(void)scanout;
	(void)ro;
}

static const driOptionDescription pan_driconf[] = {
#include "panfrost/driinfo_panfrost.h"
};

struct b2l_drawable {
	struct pipe_frontend_drawable base;
	struct st_visual visual;
	unsigned width, height;
	bool linear;                    /* PIPE_BIND_LINEAR, as IOSurfaces will be */
	struct pipe_resource *textures[ST_ATTACHMENT_COUNT];
};

static struct {
	int fd;
	driOptionCache info, options;
	struct pipe_frontend_screen fscreen;
} b2l = { .fd = -1 };

static uint64_t
now_us(void)
{
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom / 1000;
}

static int
fscreen_get_param(struct pipe_frontend_screen *fscreen, enum st_manager_param param)
{
	(void)fscreen;
	(void)param;
	return 0;
}

/* The screen, once per process (for the EAGL layer; the probe makes its own). */
__attribute__((unused)) static struct pipe_frontend_screen *
b2l_fscreen(void)
{
	if (b2l.fscreen.screen != NULL) {
		return &b2l.fscreen;
	}
	b2l.fd = b2l_drm_open();
	if (b2l.fd < 0) {
		return NULL;
	}
	driParseOptionInfo(&b2l.info, pan_driconf, ARRAY_SIZE(pan_driconf));
	struct pipe_screen_config config = { false, &b2l.options, &b2l.info };
	struct pipe_screen *screen = panfrost_drm_screen_create(b2l.fd, &config);
	if (screen == NULL) {
		b2l_drm_note("mesa: panfrost screen creation failed");
		return NULL;
	}
	b2l.fscreen.screen = screen;
	b2l.fscreen.get_param = fscreen_get_param;
	b2l_drm_note("mesa: screen %s / %s", screen->get_name(screen), screen->get_vendor(screen));
	return &b2l.fscreen;
}

static bool
drawable_flush_front(struct st_context *st, struct pipe_frontend_drawable *drawable, enum st_attachment_type statt)
{
	(void)st;
	(void)drawable;
	(void)statt;
	return true;
}

static bool
drawable_validate(struct st_context *st, struct pipe_frontend_drawable *drawable,
    const enum st_attachment_type *statts, unsigned count, struct pipe_resource **out,
    struct pipe_resource **resolve)
{
	struct b2l_drawable *d = (struct b2l_drawable *)drawable;
	struct pipe_screen *screen = d->base.fscreen->screen;
	(void)st;
	(void)resolve;

	for (unsigned i = 0; i < count; i++) {
		enum st_attachment_type a = statts[i];
		if (d->textures[a] == NULL) {
			struct pipe_resource templat;
			memset(&templat, 0, sizeof(templat));
			templat.target = PIPE_TEXTURE_2D;
			templat.width0 = d->width;
			templat.height0 = d->height;
			templat.depth0 = 1;
			templat.array_size = 1;
			templat.usage = PIPE_USAGE_DEFAULT;
			if (a == ST_ATTACHMENT_DEPTH_STENCIL) {
				templat.format = d->visual.depth_stencil_format;
				templat.bind = PIPE_BIND_DEPTH_STENCIL;
			} else {
				templat.format = d->visual.color_format;
				templat.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW |
				    (d->linear ? PIPE_BIND_LINEAR : 0);
			}
			d->textures[a] = screen->resource_create(screen, &templat);
			if (d->textures[a] == NULL) {
				return false;
			}
		}
		pipe_resource_reference(&out[i], d->textures[a]);
	}
	return true;
}

static void
drawable_init(struct b2l_drawable *d, struct pipe_frontend_screen *fscreen, unsigned width, unsigned height,
    bool linear)
{
	static uint32_t ids;
	memset(d, 0, sizeof(*d));
	d->visual.buffer_mask = ST_ATTACHMENT_FRONT_LEFT_MASK;
	d->visual.color_format = PIPE_FORMAT_R8G8B8A8_UNORM;
	d->visual.depth_stencil_format = PIPE_FORMAT_NONE;
	d->visual.accum_format = PIPE_FORMAT_NONE;
	d->visual.samples = 1;
	d->width = width;
	d->height = height;
	d->linear = linear;
	d->base.flush_front = drawable_flush_front;
	d->base.validate = drawable_validate;
	p_atomic_set(&d->base.stamp, 1);
	d->base.ID = p_atomic_inc_return(&ids);
	d->base.fscreen = fscreen;
	d->base.visual = &d->visual;
}

static void
drawable_release(struct b2l_drawable *d)
{
	st_api_destroy_drawable(&d->base);
	for (unsigned i = 0; i < ST_ATTACHMENT_COUNT; i++) {
		pipe_resource_reference(&d->textures[i], NULL);
	}
}

static GLuint
probe_shader(struct _glapi_table *disp, GLenum kind, const char *source)
{
	GLuint shader = CALL_CreateShader(disp, (kind));
	GLint ok = 0;
	CALL_ShaderSource(disp, (shader, 1, &source, NULL));
	CALL_CompileShader(disp, (shader));
	CALL_GetShaderiv(disp, (shader, GL_COMPILE_STATUS, &ok));
	if (!ok) {
		char text[200] = "";
		CALL_GetShaderInfoLog(disp, (shader, sizeof(text), NULL, text));
		b2l_drm_note("mesa: %s shader failed: %s", kind == GL_VERTEX_SHADER ? "vertex" : "fragment", text);
		return 0;
	}
	return shader;
}

/*
 * What the 64 x 64 readback should hold: the clear colour everywhere after
 * the clear; after the triangle, red where x + y < 63 (its lower-left half,
 * GL's origin at the bottom), the clear colour where x + y > 63, either on
 * the diagonal. Returns the wrong pixels and writes a 4 x 4 map of the 16 x 16
 * tiles ('.' none wrong, else wrong/16 in hex, 'g' for a whole tile), top row
 * first, and the first wrong pixel.
 */
static const uint8_t probe_clear[4] = { 64, 128, 191, 255 }, probe_red[4] = { 255, 0, 0, 255 };

static bool
probe_same(const uint8_t *p, const uint8_t rgba[4])
{
	for (unsigned c = 0; c < 4; c++) {
		int d = (int)p[c] - (int)rgba[c];
		if (d < -1 || d > 1) {
			return false;
		}
	}
	return true;
}

static unsigned
probe_check(const uint8_t *pixels, bool triangle, char map[20], char first[48])
{
	unsigned wrong = 0, tiles[16] = { 0 };
	first[0] = 0;
	for (unsigned y = 0; y < PROBE_SIZE; y++) {
		for (unsigned x = 0; x < PROBE_SIZE; x++) {
			const uint8_t *p = pixels + 4 * (y * PROBE_SIZE + x);
			bool ok;
			if (!triangle || x + y > 63) {
				ok = probe_same(p, probe_clear);
			} else if (x + y < 63) {
				ok = probe_same(p, probe_red);
			} else {
				ok = probe_same(p, probe_clear) || probe_same(p, probe_red);
			}
			if (!ok) {
				if (wrong++ == 0) {
					snprintf(first, 48, "(%u,%u)=%02x%02x%02x%02x", x, y, p[0], p[1], p[2], p[3]);
				}
				tiles[(y / 16) * 4 + x / 16]++;
			}
		}
	}
	unsigned n = 0;
	for (int ty = 3; ty >= 0; ty--) {
		for (unsigned tx = 0; tx < 4; tx++) {
			unsigned t = tiles[ty * 4 + tx];
			map[n++] = t == 0 ? '.' : t >= 256 ? 'g' : "0123456789abcdef"[t / 16];
		}
		map[n++] = ty ? '/' : 0;
	}
	return wrong;
}

/* One render target: a clear, then a triangle over its lower-left half
 * (compiler, vertex, tiler and fragment jobs), each read back and checked. */
static int
probe_target(struct pipe_frontend_screen *fscreen, struct st_context *st, const char *variant, bool linear)
{
	struct b2l_drawable drawable;
	const char *layout = linear ? "linear" : "default";
	drawable_init(&drawable, fscreen, PROBE_SIZE, PROBE_SIZE, linear);
	if (!st_api_make_current(st, &drawable.base, &drawable.base)) {
		b2l_drm_note("mesa: [%s] %s: make current failed", variant, layout);
		return 0;
	}
	struct _glapi_table *disp = GET_DISPATCH();
	static uint8_t pixels[PROBE_SIZE * PROBE_SIZE * 4];
	char map[20], first[48];

	/* 1. A clear: one fragment job. */
	uint64_t t0 = now_us();
	CALL_Viewport(disp, (0, 0, PROBE_SIZE, PROBE_SIZE));
	CALL_ClearColor(disp, (64 / 255.0f, 128 / 255.0f, 191 / 255.0f, 1.0f));
	CALL_Clear(disp, (GL_COLOR_BUFFER_BIT));
	memset(pixels, 0x5a, sizeof(pixels));
	CALL_ReadPixels(disp, (0, 0, PROBE_SIZE, PROBE_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels));
	unsigned wrong_clear = probe_check(pixels, false, map, first);
	b2l_drm_note("mesa: [%s] %s clear: %u wrong in %llu us, tiles %s %s, error 0x%x", variant, layout, wrong_clear,
	    (unsigned long long)(now_us() - t0), map, first, CALL_GetError(disp, ()));

	/* 2. The triangle. */
	t0 = now_us();
	GLuint vs = probe_shader(disp, GL_VERTEX_SHADER,
	    "#version 300 es\nlayout(location = 0) in vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n");
	GLuint fs = probe_shader(disp, GL_FRAGMENT_SHADER,
	    "#version 300 es\nprecision mediump float;\nout vec4 c;\nvoid main() { c = vec4(1.0, 0.0, 0.0, 1.0); }\n");
	GLuint program = 0;
	GLint linked = 0;
	if (vs && fs) {
		program = CALL_CreateProgram(disp, ());
		CALL_AttachShader(disp, (program, vs));
		CALL_AttachShader(disp, (program, fs));
		CALL_LinkProgram(disp, (program));
		CALL_GetProgramiv(disp, (program, GL_LINK_STATUS, &linked));
	}
	uint64_t compile_us = now_us() - t0;
	unsigned wrong_triangle = PROBE_SIZE * PROBE_SIZE;
	if (!linked) {
		b2l_drm_note("mesa: [%s] %s: shaders not ready (vs %u fs %u program %u)", variant, layout, vs, fs, program);
	} else {
		static const float vertices[6] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f };
		GLuint buffer = 0;
		t0 = now_us();
		CALL_UseProgram(disp, (program));
		CALL_GenBuffers(disp, (1, &buffer));
		CALL_BindBuffer(disp, (GL_ARRAY_BUFFER, buffer));
		CALL_BufferData(disp, (GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW));
		CALL_VertexAttribPointer(disp, (0, 2, GL_FLOAT, GL_FALSE, 0, NULL));
		CALL_EnableVertexAttribArray(disp, (0));
		CALL_DrawArrays(disp, (GL_TRIANGLES, 0, 3));
		memset(pixels, 0x5a, sizeof(pixels));
		CALL_ReadPixels(disp, (0, 0, PROBE_SIZE, PROBE_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels));
		wrong_triangle = probe_check(pixels, true, map, first);
		b2l_drm_note("mesa: [%s] %s triangle: %u wrong, compile %llu us, draw+read %llu us, tiles %s %s, error 0x%x",
		    variant, layout, wrong_triangle, (unsigned long long)compile_us, (unsigned long long)(now_us() - t0),
		    map, first, CALL_GetError(disp, ()));
		CALL_DeleteBuffers(disp, (1, &buffer));
	}
	st_api_make_current(st, NULL, NULL);
	drawable_release(&drawable);
	return wrong_clear == 0 && wrong_triangle == 0;
}

/*
 * One Panfrost screen with PAN_MESA_DEBUG set to flags (Panfrost reads it when
 * the screen is made), an ES 3 context on it, and three targets: default,
 * linear, linear again (boot 38 showed default right only for the triangle
 * and linear right only for the clear; the order tells first-use effects from
 * layout ones). Everything is released afterwards.
 */
static int
probe_variant(const char *flags)
{
	const char *name = flags[0] ? flags : "plain";
	setenv("PAN_MESA_DEBUG", flags, 1);
	int fd = b2l_drm_open();
	if (fd < 0) {
		return 0;
	}
	static driOptionCache info, options;
	driParseOptionInfo(&info, pan_driconf, ARRAY_SIZE(pan_driconf));
	struct pipe_screen_config config = { false, &options, &info };
	uint64_t start = now_us();
	struct pipe_screen *screen = panfrost_create_screen(fd, &config, NULL);
	if (screen == NULL) {
		b2l_drm_note("mesa: [%s] panfrost screen creation failed", name);
		return 0;
	}
	struct pipe_frontend_screen *fscreen = CALLOC_STRUCT(pipe_frontend_screen);
	fscreen->screen = screen;
	fscreen->get_param = fscreen_get_param;

	struct st_context_attribs attribs;
	memset(&attribs, 0, sizeof(attribs));
	attribs.profile = API_OPENGLES2;
	attribs.major = 3;
	attribs.minor = 0;
	attribs.visual.buffer_mask = ST_ATTACHMENT_FRONT_LEFT_MASK;
	attribs.visual.color_format = PIPE_FORMAT_R8G8B8A8_UNORM;
	attribs.visual.depth_stencil_format = PIPE_FORMAT_NONE;
	attribs.visual.accum_format = PIPE_FORMAT_NONE;
	attribs.visual.samples = 1;
	enum st_context_error error = ST_CONTEXT_SUCCESS;
	struct st_context *st = st_api_create_context(fscreen, &attribs, &error, NULL);
	int result = 0;
	if (st == NULL) {
		b2l_drm_note("mesa: [%s] context FAILED (error %d)", name, (int)error);
	} else {
		b2l_drm_note("mesa: [%s] %s, screen and context in %llu us", name, screen->get_name(screen),
		    (unsigned long long)(now_us() - start));
		result = probe_target(fscreen, st, name, false);
		result &= probe_target(fscreen, st, name, true);
		result &= probe_target(fscreen, st, name, true);
		st_api_make_current(NULL, NULL, NULL);
		st_destroy_context(st);
	}
	st_screen_destroy(fscreen);
	FREE(fscreen);
	screen->destroy(screen);
	b2l_drm_note("mesa: [%s] %s", name, result ? "all right" : "WRONG");
	return result;
}

/*
 * The Mesa run on the phone: the same three targets with transaction
 * elimination (CRC) and AFBC each on and off, to find which one loses the
 * clear or the triangle. Returns a bit per variant that came out right.
 */
B2L_EXPORT int
b2l_mesa_probe(void)
{
	static const char *const variants[] = { "", "nocrc", "noafbc", "nocrc,noafbc" };
	uint64_t start = now_us();
	int passed = 0;
	for (unsigned i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
		if (probe_variant(variants[i])) {
			passed |= 1 << i;
		}
	}
	unsetenv("PAN_MESA_DEBUG");
	b2l_drm_note("mesa: probe done in %llu ms, right: plain %d nocrc %d noafbc %d both %d",
	    (unsigned long long)((now_us() - start) / 1000), passed & 1, (passed >> 1) & 1, (passed >> 2) & 1,
	    (passed >> 3) & 1);
	return passed;
}
