// SPDX-License-Identifier: MIT
/*
 * S10 port: Mesa on iOS over B2LMali. A minimal GL frontend in the manner of
 * OSMesa: one Panfrost screen on the B2LMali libdrm, OpenGL ES contexts that
 * draw into pipe resources this file owns. It is the base of the EAGL layer
 * that will serve Core Animation; for now b2l_mesa_probe() is the first test
 * on the phone: an OpenGL ES 3 context, a clear and a shader-drawn triangle
 * into a 64 x 64 RGBA target, read back and checked, each step logged to the
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

/* The screen, once per process. */
static struct pipe_frontend_screen *
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
				templat.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
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
drawable_init(struct b2l_drawable *d, struct pipe_frontend_screen *fscreen, unsigned width, unsigned height)
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

/* Pixels equal to rgba (with 1 of rounding slack), in the 64 x 64 readback. */
static unsigned
probe_count(const uint8_t *pixels, const uint8_t rgba[4])
{
	unsigned n = 0;
	for (unsigned i = 0; i < PROBE_SIZE * PROBE_SIZE; i++) {
		const uint8_t *p = pixels + 4 * i;
		bool same = true;
		for (unsigned c = 0; c < 4; c++) {
			int d = (int)p[c] - (int)rgba[c];
			same &= d >= -1 && d <= 1;
		}
		n += same;
	}
	return n;
}

/*
 * The first Mesa run on the phone. Returns 1 when the clear and the triangle
 * both read back right, 0 otherwise; every step is logged.
 */
B2L_EXPORT int
b2l_mesa_probe(void)
{
	uint64_t start = now_us();
	struct pipe_frontend_screen *fscreen = b2l_fscreen();
	if (fscreen == NULL) {
		return 0;
	}
	uint64_t screen_us = now_us() - start;

	int core = 0, compat = 0, es1 = 0, es2 = 0;
	struct st_config_options options;
	memset(&options, 0, sizeof(options));
	st_api_query_versions(fscreen, &options, &core, &compat, &es1, &es2);

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
	b2l_drm_note("mesa: screen in %llu us, versions core %d compat %d es1 %d es2 %d, context %s (error %d)",
	    (unsigned long long)screen_us, core, compat, es1, es2, st ? "ok" : "FAILED", (int)error);
	if (st == NULL) {
		return 0;
	}

	struct b2l_drawable drawable;
	drawable_init(&drawable, fscreen, PROBE_SIZE, PROBE_SIZE);
	if (!st_api_make_current(st, &drawable.base, &drawable.base)) {
		b2l_drm_note("mesa: make current failed");
		st_destroy_context(st);
		return 0;
	}
	struct _glapi_table *disp = GET_DISPATCH();
	b2l_drm_note("mesa: %s | %s | GLSL %s", (const char *)CALL_GetString(disp, (GL_RENDERER)),
	    (const char *)CALL_GetString(disp, (GL_VERSION)),
	    (const char *)CALL_GetString(disp, (GL_SHADING_LANGUAGE_VERSION)));

	static uint8_t pixels[PROBE_SIZE * PROBE_SIZE * 4];
	static const uint8_t clear[4] = { 64, 128, 191, 255 }, red[4] = { 255, 0, 0, 255 };
	int result = 1;

	/* 1. A clear: one fragment job. */
	uint64_t t0 = now_us();
	CALL_Viewport(disp, (0, 0, PROBE_SIZE, PROBE_SIZE));
	CALL_ClearColor(disp, (64 / 255.0f, 128 / 255.0f, 191 / 255.0f, 1.0f));
	CALL_Clear(disp, (GL_COLOR_BUFFER_BIT));
	memset(pixels, 0, sizeof(pixels));
	CALL_ReadPixels(disp, (0, 0, PROBE_SIZE, PROBE_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels));
	unsigned cleared = probe_count(pixels, clear);
	b2l_drm_note("mesa: clear %u/%u pixels right in %llu us, first %02x%02x%02x%02x, error 0x%x", cleared,
	    PROBE_SIZE * PROBE_SIZE, (unsigned long long)(now_us() - t0), pixels[0], pixels[1], pixels[2], pixels[3],
	    CALL_GetError(disp, ()));
	result &= cleared == PROBE_SIZE * PROBE_SIZE;

	/* 2. A triangle over the lower-left half: compiler, vertex and tiler jobs. */
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
	if (!linked) {
		b2l_drm_note("mesa: shaders not ready (vs %u fs %u program %u)", vs, fs, program);
		result = 0;
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
		memset(pixels, 0, sizeof(pixels));
		CALL_ReadPixels(disp, (0, 0, PROBE_SIZE, PROBE_SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels));
		unsigned reds = probe_count(pixels, red), rest = probe_count(pixels, clear);
		const uint8_t *corner = pixels + 4 * (PROBE_SIZE * PROBE_SIZE - 1);
		b2l_drm_note("mesa: triangle red %u clear %u of %u (half is %u) compile %llu us draw+read %llu us, "
		    "corners %02x%02x%02x %02x%02x%02x, error 0x%x", reds, rest, PROBE_SIZE * PROBE_SIZE,
		    PROBE_SIZE * PROBE_SIZE / 2, (unsigned long long)compile_us, (unsigned long long)(now_us() - t0),
		    pixels[0], pixels[1], pixels[2], corner[0], corner[1], corner[2], CALL_GetError(disp, ()));
		result &= reds > PROBE_SIZE * PROBE_SIZE * 2 / 5 && rest > PROBE_SIZE * PROBE_SIZE * 2 / 5 &&
		    reds + rest == PROBE_SIZE * PROBE_SIZE;
	}

	st_api_make_current(NULL, NULL, NULL);
	drawable_release(&drawable);
	st_destroy_context(st);
	b2l_drm_note("mesa: probe %s in %llu ms", result ? "PASSED" : "FAILED",
	    (unsigned long long)((now_us() - start) / 1000));
	return result;
}
