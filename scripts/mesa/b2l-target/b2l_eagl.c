// SPDX-License-Identifier: MIT
/*
 * S10 port: the EAGL layer Core Animation's OpenGL ES renderer needs, on the
 * Mesa frontend of b2l_mesa.c. backboardd's library (userspace/b2l-gpu.c)
 * makes the window server build CA::OGL::GLESContext on the object made here
 * instead of its software context (iOS 26's window server only tries Metal
 * and then software; GLESContext itself is still in QuartzCore).
 *
 * What GLESContext asks of its EAGLContext, from the 23G90 QuartzCore:
 *  - API (above 2 turns on its ES 3 paths) and getMacroContextPrivate: a
 *    GLIContext followed by the GLI dispatch table. Every GL call goes through
 *    that table with the GLIContext first; GLESContext never makes a context
 *    current itself, so each entry here does.
 *  - attachImage:toCoreSurface:invertedRender: for the display's buffers: the
 *    IOSurface becomes the image of the texture bound to the target, and
 *    GLESContext puts a framebuffer around that texture.
 *  - texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:
 *    for layer contents in IOSurfaces.
 * IOSurfaces become Mesa resources through the B2LMali import (libdrm's
 * surface fd, as the probe's) and reach the bound texture as EGL images
 * (glEGLImageTargetTexture2DOES with this file's frontend-screen callbacks).
 *
 * This first version is meant to show where Core Animation breaks: GL entries
 * Mesa does not have, GL errors in the first calls, selectors the class lacks
 * and every surface import are logged ("B2L GPU UI: eagl: ...").
 */
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/glheader.h"
#include "state_tracker/st_context.h"
#include "frontend/api.h"
#include "frontend/winsys_handle.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "glapi/glapi.h"
#include "main/dispatch.h"
#include "drm-uapi/drm_fourcc.h"
#include <IOSurface/IOSurfaceRef.h>
#include <objc/runtime.h>
#include "b2l_drm.h"

#define B2L_EXPORT __attribute__((visibility("default")))

B2L_EXPORT void *b2l_eagl_context_create(void);
B2L_EXPORT int b2l_eagl_flush(void);
struct pipe_frontend_screen *b2l_fscreen(void);

/* Enums from Apple's and the ES extension headers. */
#define E_BGRA                  0x80E1
#define E_RGBA                  0x1908
#define E_RGBA8                 0x8058
#define E_RED                   0x1903
#define E_ALPHA                 0x1906
#define E_LUMINANCE             0x1909
#define E_LUMINANCE_ALPHA       0x190A
#define E_RG                    0x8227
#define E_UNSIGNED_BYTE         0x1401
#define E_UNSIGNED_SHORT        0x1403
#define E_FLOAT                 0x1406
#define E_HALF_FLOAT            0x140B
#define E_HALF_FLOAT_OES        0x8D61
#define E_UINT_8888_REV         0x8367
#define E_UINT_2101010_REV      0x8368
#define E_CURRENT_PROGRAM       0x8B8D
#define E_PROGRAM_OBJECT_ARB    0x8B40

#define LOG_LINES       2500    /* "eagl:" lines per process, summaries aside */
#define ERROR_WINDOW    4000    /* GL errors checked after each of the first calls */
#define IMAGES          48      /* imported IOSurfaces kept */
#define IMAGE_MAGIC     0xb2e61a6eu

static _glapi_proc b2l_gli_enter(void *c, unsigned index, uint64_t a0);
static void b2l_gli_leave(unsigned index);
#include "b2l_gli_table.h"

struct b2l_eagl {
	struct {
		void *rend;                     /* GLIContext: this struct */
		void *disp[B2L_GLI_COUNT];      /* GLIFunctionDispatch */
	} macro;
	struct st_context *st;
	id object;
};

struct b2l_image {
	uint32_t magic;
	IOSurfaceRef surface;           /* retained while cached */
	uint32_t surface_id, plane;
	enum pipe_format format;
	unsigned width, height;
	struct pipe_resource *res;
	uint64_t used;
};

static int gli_slot[B2L_GLI_COUNT];             /* Mesa dispatch offset; -1 none; -2 our own */
static uint8_t gli_seen[B2L_GLI_COUNT];         /* 1 first use logged, 2 also "missing" */
static uint32_t gli_calls[B2L_GLI_COUNT];
static uint64_t calls_total, errors_total, window_calls, window_start;
static unsigned lines, attaches, teximages, import_failures, switches, swap_flushes, copies;
static unsigned expanded_draws, expand_fallbacks;
static uint64_t swap_flush_ms;
static struct b2l_eagl *the_eagl;
static struct b2l_image images[IMAGES];
static uint64_t image_clock;
static pthread_mutex_t image_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t
now_ms(void)
{
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom / 1000000;
}

static void note(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void
note(const char *format, ...)
{
	char text[240];
	va_list ap;
	if (__atomic_fetch_add(&lines, 1, __ATOMIC_RELAXED) >= LOG_LINES) {
		return;
	}
	va_start(ap, format);
	vsnprintf(text, sizeof(text), format, ap);
	va_end(ap);
	b2l_drm_note("eagl: %s", text);
}

static void
summary(void)
{
	uint64_t now = now_ms();
	if (window_start == 0) {
		window_start = now;
		return;
	}
	if (now - window_start < 5000) {
		return;
	}
	char used[200] = "";
	size_t at = 0;
	for (unsigned i = 0; i < B2L_GLI_COUNT && at + 24 < sizeof(used); i++) {
		if (gli_calls[i] >= window_calls / 20 && gli_calls[i] > 0) {
			at += (size_t)snprintf(used + at, sizeof(used) - at, " %s=%u", b2l_gli_name[i], gli_calls[i]);
		}
	}
	b2l_drm_note("eagl: 5 s: %llu calls (%llu total), attach %u, teximage %u (copied %u), import failures %u,"
	    " GL errors %llu, context switches %u, swaps flushed %u (%llu ms), draws expanded %u (kept %u); busiest:%s",
	    (unsigned long long)window_calls, (unsigned long long)calls_total, attaches, teximages, copies,
	    import_failures, (unsigned long long)errors_total, switches, swap_flushes,
	    (unsigned long long)swap_flush_ms, expanded_draws, expand_fallbacks, used);
	swap_flush_ms = 0;
	memset(gli_calls, 0, sizeof(gli_calls));
	window_calls = 0;
	window_start = now;
}

/* Our context current on this thread, the first time a thread calls in. */
static int
begin(void *c, unsigned index, uint64_t a0)
{
	struct b2l_eagl *e = c;
	if (e == NULL || e != the_eagl) {
		note("GLI %s called with context %p, not ours", index < B2L_GLI_COUNT ? b2l_gli_name[index] : "?", c);
		return 0;
	}
	if (_mesa_glapi_get_context() != (void *)e->st->ctx) {
		if (!st_api_make_current(e->st, NULL, NULL)) {
			note("make current failed on a thread");
			return 0;
		}
		if (switches++ < 8) {
			note("context made current on thread %p (switch %u)", (void *)pthread_self(), switches);
		}
	}
	calls_total++;
	window_calls++;
	if (index < B2L_GLI_COUNT) {
		gli_calls[index]++;
		if (!gli_seen[index]) {
			gli_seen[index] = 1;
			note("first %s (#%u) a0=0x%llx at call %llu", b2l_gli_name[index], index, (unsigned long long)a0,
			    (unsigned long long)calls_total);
		}
	}
	if ((calls_total & 63) == 0) {
		summary();
	}
	return 1;
}

static _glapi_proc
b2l_gli_enter(void *c, unsigned index, uint64_t a0)
{
	if (!begin(c, index, a0)) {
		return NULL;
	}
	int slot = gli_slot[index];
	if (slot < 0) {
		if (gli_seen[index] < 2) {
			gli_seen[index] = 2;
			note("%s (#%u) has no Mesa function (tried %s); returns 0", b2l_gli_name[index], index,
			    b2l_gli_gl[index]);
		}
		return NULL;
	}
	return ((_glapi_proc *)GET_DISPATCH())[slot];
}

static void
b2l_gli_leave(unsigned index)
{
	if (calls_total > ERROR_WINDOW && gli_calls[index] > 2) {
		return;
	}
	GLenum error = CALL_GetError(GET_DISPATCH(), ());
	if (error != GL_NO_ERROR) {
		errors_total++;
		note("%s (#%u) at call %llu: GL error 0x%x", b2l_gli_name[index], index, (unsigned long long)calls_total,
		    error);
	}
}

/* ---- GLI entries of our own ---- */

static int idx_get_string, idx_tex_image2D, idx_tex_sub_image2D, idx_finish_object, idx_delete_object,
    idx_create_shader_object, idx_create_program_object, idx_attach_object, idx_detach_object,
    idx_get_object_parameteriv, idx_get_object_parameterfv, idx_get_info_log, idx_get_attached_objects,
    idx_get_handle, idx_insert_marker, idx_push_marker, idx_pop_marker, idx_shader_source, idx_attrib_pointer,
    idx_draw_elements;

#define HANDLE(h) ((GLuint)(uintptr_t)(h))
#define TO_HANDLE(n) ((void *)(uintptr_t)(n))

/*
 * Boots 49-51, quads losing or stretching a triangle in different frames: the
 * first shaders Core Animation compiles (precision of its position math), the
 * first vertex array setups and the first indexed draws (with the index range
 * read on the CPU) go to the log, so the next step rests on what it draws.
 */
#define SHADERS_LOGGED  10
#define SHADER_LINES    40
#define ATTRIBS_LOGGED  48
#define DRAWS_LOGGED    32

static void
note_text(const char *what, unsigned id, const char *text, size_t length)
{
	char chunk[184];
	unsigned lines_done = 0;
	for (size_t at = 0; at < length && lines_done < SHADER_LINES; lines_done++) {
		size_t n = 0;
		while (n < sizeof(chunk) - 1 && at < length) {
			char ch = text[at++];
			chunk[n++] = ch == '\n' ? '|' : (ch == '\t' ? ' ' : ch);
		}
		chunk[n] = '\0';
		note("%s %u.%u: %s", what, id, lines_done, chunk);
	}
}

static void
gli_shader_source(void *c, void *shader, int32_t count, const void *strings, const void *lengths)
{
	static unsigned logged;
	if (!begin(c, (unsigned)idx_shader_source, HANDLE(shader))) {
		return;
	}
	struct _glapi_table *d = GET_DISPATCH();
	CALL_ShaderSource(d, (HANDLE(shader), count, (const GLchar *const *)strings, (const GLint *)lengths));
	b2l_gli_leave((unsigned)idx_shader_source);
	if (logged < SHADERS_LOGGED && strings != NULL && count > 0) {
		GLint kind = 0;
		CALL_GetShaderiv(d, (HANDLE(shader), GL_SHADER_TYPE, &kind));
		unsigned id = logged++;
		note("shader %u: %s, name %u, %d strings", id, kind == GL_VERTEX_SHADER ? "vertex" : "fragment",
		    HANDLE(shader), count);
		for (int32_t i = 0; i < count; i++) {
			const char *s = ((const char *const *)strings)[i];
			GLint len = lengths != NULL ? ((const GLint *)lengths)[i] : -1;
			if (s != NULL) {
				note_text("src", id, s, len >= 0 ? (size_t)len : strlen(s));
			}
		}
	}
}

static void
gli_attrib_pointer(void *c, uint32_t index, int32_t size, uint32_t type, uint8_t normalized, int32_t stride,
    const void *pointer)
{
	static unsigned logged;
	if (!begin(c, (unsigned)idx_attrib_pointer, index)) {
		return;
	}
	struct _glapi_table *d = GET_DISPATCH();
	if (logged < ATTRIBS_LOGGED) {
		GLint array_buffer = 0;
		CALL_GetIntegerv(d, (GL_ARRAY_BUFFER_BINDING, &array_buffer));
		note("attrib %u: index %u size %d type 0x%x normalized %u stride %d pointer %p (array buffer %d)",
		    logged++, index, size, type, (unsigned)normalized, stride, pointer, array_buffer);
	}
	CALL_VertexAttribPointer(d, (index, size, type, normalized, stride, pointer));
	b2l_gli_leave((unsigned)idx_attrib_pointer);
}

/*
 * Boot 52: every Core Animation draw is a quad, four vertices drawn with the
 * indices 0,1,2, 2,3,0, its client arrays rewritten for the next quad right
 * after. The broken quads lose or stretch the second triangle, the only one
 * using vertex 3, the highest index: as if the last vertex of the range were
 * not shaded and its position came from whatever an earlier draw left. Test,
 * and workaround if it holds: the draw is expanded on the CPU into a
 * non-indexed one (the vertices in index order, from the client arrays) with
 * one more vertex than the primitives use, so no needed vertex is last.
 * B2L_EAGL_EXPAND=0 keeps the indexed draws.
 */
#define EXPAND_ATTRIBS  8
static int expand_enabled = -1;

static unsigned
type_bytes(GLenum type)
{
	switch (type) {
	case GL_BYTE:
	case GL_UNSIGNED_BYTE:
		return 1;
	case GL_SHORT:
	case GL_UNSIGNED_SHORT:
	case GL_HALF_FLOAT:
	case E_HALF_FLOAT_OES:
		return 2;
	case GL_INT:
	case GL_UNSIGNED_INT:
	case GL_FLOAT:
	case GL_FIXED:
		return 4;
	default:
		return 0;
	}
}

static bool
draw_expanded(struct _glapi_table *d, uint32_t mode, int32_t count, uint32_t type, const void *indices)
{
	static uint8_t *scratch[EXPAND_ATTRIBS];
	static size_t scratch_size[EXPAND_ATTRIBS];
	struct {
		GLint enabled, size, type, normalized, stride, buffer;
		void *pointer;
		unsigned bytes;
	} a[EXPAND_ATTRIBS];
	GLint element_buffer = 0;
	CALL_GetIntegerv(d, (GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_buffer));
	unsigned index_bytes = type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_BYTE ? 1 : type == GL_UNSIGNED_INT ? 4 : 0;
	if (element_buffer != 0 || indices == NULL || count <= 0 || index_bytes == 0) {
		return false;
	}
	unsigned pad = (mode == GL_TRIANGLES && count % 3 == 0) || (mode == GL_LINES && count % 2 == 0) ? 1 : 0;
	unsigned total = (unsigned)count + pad;
	for (unsigned i = 0; i < EXPAND_ATTRIBS; i++) {
		a[i].enabled = 0;
		CALL_GetVertexAttribiv(d, (i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a[i].enabled));
		if (!a[i].enabled) {
			continue;
		}
		CALL_GetVertexAttribiv(d, (i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a[i].size));
		CALL_GetVertexAttribiv(d, (i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a[i].type));
		CALL_GetVertexAttribiv(d, (i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a[i].normalized));
		CALL_GetVertexAttribiv(d, (i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a[i].stride));
		CALL_GetVertexAttribiv(d, (i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a[i].buffer));
		CALL_GetVertexAttribPointerv(d, (i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a[i].pointer));
		a[i].bytes = type_bytes((GLenum)a[i].type) * (unsigned)a[i].size;
		if (a[i].buffer != 0 || a[i].pointer == NULL || a[i].bytes == 0 || a[i].size < 1 || a[i].size > 4) {
			return false;
		}
		size_t need = (size_t)total * a[i].bytes;
		if (scratch_size[i] < need) {
			uint8_t *grown = realloc(scratch[i], need);
			if (grown == NULL) {
				return false;
			}
			scratch[i] = grown;
			scratch_size[i] = need;
		}
	}
	for (unsigned i = 0; i < EXPAND_ATTRIBS; i++) {
		if (!a[i].enabled) {
			continue;
		}
		size_t step = a[i].stride != 0 ? (size_t)a[i].stride : a[i].bytes;
		const uint8_t *src = a[i].pointer;
		for (unsigned k = 0; k < total; k++) {
			unsigned at = k < (unsigned)count ? k : 0;   /* the padding vertex repeats the first */
			uint32_t v = index_bytes == 2 ? ((const uint16_t *)indices)[at] :
			    index_bytes == 1 ? ((const uint8_t *)indices)[at] : ((const uint32_t *)indices)[at];
			memcpy(scratch[i] + (size_t)k * a[i].bytes, src + (size_t)v * step, a[i].bytes);
		}
		CALL_VertexAttribPointer(d, (i, a[i].size, (GLenum)a[i].type, (GLboolean)a[i].normalized, 0, scratch[i]));
	}
	CALL_DrawArrays(d, (mode, 0, (GLsizei)total));
	for (unsigned i = 0; i < EXPAND_ATTRIBS; i++) {
		if (a[i].enabled) {
			CALL_VertexAttribPointer(d, (i, a[i].size, (GLenum)a[i].type, (GLboolean)a[i].normalized, a[i].stride,
			    a[i].pointer));
		}
	}
	return true;
}

static void
gli_draw_elements(void *c, uint32_t mode, int32_t count, uint32_t type, const void *indices)
{
	static unsigned logged;
	if (!begin(c, (unsigned)idx_draw_elements, mode)) {
		return;
	}
	struct _glapi_table *d = GET_DISPATCH();
	if (expand_enabled < 0) {
		const char *env = getenv("B2L_EAGL_EXPAND");
		expand_enabled = env == NULL || env[0] != '0';
		note("indexed draws %s", expand_enabled ? "expanded on the CPU (B2L_EAGL_EXPAND=0 keeps them)" : "kept");
	}
	if (expand_enabled) {
		if (draw_expanded(d, mode, count, type, indices)) {
			expanded_draws++;
			b2l_gli_leave((unsigned)idx_draw_elements);
			return;
		}
		if (expand_fallbacks++ < 8) {
			note("draw_elements mode 0x%x count %d type 0x%x not expanded (buffer objects or type)", mode, count,
			    type);
		}
	}
	if (logged < DRAWS_LOGGED) {
		GLint element_buffer = 0, program = 0;
		CALL_GetIntegerv(d, (GL_ELEMENT_ARRAY_BUFFER_BINDING, &element_buffer));
		CALL_GetIntegerv(d, (GL_CURRENT_PROGRAM, &program));
		unsigned lo = ~0u, hi = 0;
		char first[64] = "";
		if (element_buffer == 0 && indices != NULL && count > 0 && type == GL_UNSIGNED_SHORT) {
			const uint16_t *ix = indices;
			for (int32_t i = 0; i < count; i++) {
				lo = ix[i] < lo ? ix[i] : lo;
				hi = ix[i] > hi ? ix[i] : hi;
			}
			snprintf(first, sizeof(first), "%u,%u,%u,%u,%u,%u", ix[0], count > 1 ? ix[1] : 0,
			    count > 2 ? ix[2] : 0, count > 3 ? ix[3] : 0, count > 4 ? ix[4] : 0, count > 5 ? ix[5] : 0);
		}
		note("draw %u: mode 0x%x count %d type 0x%x indices %p (element buffer %d) program %d range %u..%u first %s",
		    logged++, mode, count, type, indices, element_buffer, program, lo, hi, first);
	}
	CALL_DrawElements(d, (mode, count, type, indices));
	b2l_gli_leave((unsigned)idx_draw_elements);
}

static const void *
gli_get_string(void *c, uint32_t name)
{
	if (!begin(c, (unsigned)idx_get_string, name)) {
		return "";
	}
	switch (name) {
	case GL_VENDOR:
	case GL_RENDERER:
	case GL_VERSION:
	case GL_SHADING_LANGUAGE_VERSION:
	case GL_EXTENSIONS: {
		const GLubyte *s = CALL_GetString(GET_DISPATCH(), (name));
		b2l_gli_leave((unsigned)idx_get_string);
		return s != NULL ? (const void *)s : "";
	}
	default: {
		static unsigned logged;
		if (logged++ < 8) {
			note("get_string 0x%x (not a Mesa string): \"\"", name);
		}
		return "";
	}
	}
}

/* Apple's BGRA upload: internal format GL_RGBA with format GL_BGRA, which ES
 * (EXT_texture_format_BGRA8888) spells internal format GL_BGRA. */
static void
gli_tex_image2D(void *c, uint32_t target, int32_t level, uint32_t internal, int32_t width, int32_t height,
    int32_t border, uint32_t format, uint32_t type, const void *pixels)
{
	if (!begin(c, (unsigned)idx_tex_image2D, target)) {
		return;
	}
	if (format == E_BGRA && (internal == E_RGBA || internal == E_RGBA8)) {
		internal = E_BGRA;
	}
	if (format == E_BGRA && type == E_UINT_8888_REV) {
		type = E_UNSIGNED_BYTE;
	}
	if (type == E_HALF_FLOAT_OES) {
		type = E_HALF_FLOAT;
	}
	CALL_TexImage2D(GET_DISPATCH(), (target, level, (GLint)internal, width, height, border, format, type, pixels));
	b2l_gli_leave((unsigned)idx_tex_image2D);
}

static void
gli_tex_sub_image2D(void *c, uint32_t target, int32_t level, int32_t x, int32_t y, int32_t width, int32_t height,
    uint32_t format, uint32_t type, const void *pixels)
{
	if (!begin(c, (unsigned)idx_tex_sub_image2D, target)) {
		return;
	}
	if (format == E_BGRA && type == E_UINT_8888_REV) {
		type = E_UNSIGNED_BYTE;
	}
	if (type == E_HALF_FLOAT_OES) {
		type = E_HALF_FLOAT;
	}
	CALL_TexSubImage2D(GET_DISPATCH(), (target, level, x, y, width, height, format, type, pixels));
	b2l_gli_leave((unsigned)idx_tex_sub_image2D);
}

/* APPLE_fence's finish on one object: all of it is simplest. */
static void
gli_finish_object(void *c, uint32_t object, int32_t name)
{
	if (begin(c, (unsigned)idx_finish_object, object)) {
		(void)name;
		CALL_Finish(GET_DISPATCH(), ());
	}
}

/* The ARB shader-object entries, which ES has under other names. */
static void
gli_delete_object(void *c, void *object)
{
	if (begin(c, (unsigned)idx_delete_object, HANDLE(object))) {
		struct _glapi_table *d = GET_DISPATCH();
		if (CALL_IsProgram(d, (HANDLE(object)))) {
			CALL_DeleteProgram(d, (HANDLE(object)));
		} else {
			CALL_DeleteShader(d, (HANDLE(object)));
		}
		b2l_gli_leave((unsigned)idx_delete_object);
	}
}

static void *
gli_create_shader_object(void *c, uint32_t type)
{
	if (!begin(c, (unsigned)idx_create_shader_object, type)) {
		return NULL;
	}
	GLuint shader = CALL_CreateShader(GET_DISPATCH(), (type));
	b2l_gli_leave((unsigned)idx_create_shader_object);
	return TO_HANDLE(shader);
}

static void *
gli_create_program_object(void *c)
{
	if (!begin(c, (unsigned)idx_create_program_object, 0)) {
		return NULL;
	}
	GLuint program = CALL_CreateProgram(GET_DISPATCH(), ());
	b2l_gli_leave((unsigned)idx_create_program_object);
	return TO_HANDLE(program);
}

static void
gli_attach_object(void *c, void *container, void *object)
{
	if (begin(c, (unsigned)idx_attach_object, HANDLE(container))) {
		CALL_AttachShader(GET_DISPATCH(), (HANDLE(container), HANDLE(object)));
		b2l_gli_leave((unsigned)idx_attach_object);
	}
}

static void
gli_detach_object(void *c, void *container, void *object)
{
	if (begin(c, (unsigned)idx_detach_object, HANDLE(container))) {
		CALL_DetachShader(GET_DISPATCH(), (HANDLE(container), HANDLE(object)));
		b2l_gli_leave((unsigned)idx_detach_object);
	}
}

static void
gli_get_object_parameteriv(void *c, void *object, uint32_t pname, void *params)
{
	if (begin(c, (unsigned)idx_get_object_parameteriv, HANDLE(object))) {
		struct _glapi_table *d = GET_DISPATCH();
		if (CALL_IsProgram(d, (HANDLE(object)))) {
			CALL_GetProgramiv(d, (HANDLE(object), pname, (GLint *)params));
		} else {
			CALL_GetShaderiv(d, (HANDLE(object), pname, (GLint *)params));
		}
		b2l_gli_leave((unsigned)idx_get_object_parameteriv);
	}
}

static void
gli_get_object_parameterfv(void *c, void *object, uint32_t pname, void *params)
{
	if (begin(c, (unsigned)idx_get_object_parameterfv, HANDLE(object))) {
		struct _glapi_table *d = GET_DISPATCH();
		GLint value = 0;
		if (CALL_IsProgram(d, (HANDLE(object)))) {
			CALL_GetProgramiv(d, (HANDLE(object), pname, &value));
		} else {
			CALL_GetShaderiv(d, (HANDLE(object), pname, &value));
		}
		*(GLfloat *)params = (GLfloat)value;
		b2l_gli_leave((unsigned)idx_get_object_parameterfv);
	}
}

static void
gli_get_info_log(void *c, void *object, int32_t max, void *length, void *log)
{
	if (begin(c, (unsigned)idx_get_info_log, HANDLE(object))) {
		struct _glapi_table *d = GET_DISPATCH();
		if (CALL_IsProgram(d, (HANDLE(object)))) {
			CALL_GetProgramInfoLog(d, (HANDLE(object), max, (GLsizei *)length, (GLchar *)log));
		} else {
			CALL_GetShaderInfoLog(d, (HANDLE(object), max, (GLsizei *)length, (GLchar *)log));
		}
		b2l_gli_leave((unsigned)idx_get_info_log);
	}
}

static void
gli_get_attached_objects(void *c, void *container, int32_t max, void *count, void *objects)
{
	if (begin(c, (unsigned)idx_get_attached_objects, HANDLE(container))) {
		GLuint names[16];
		GLsizei n = 0;
		CALL_GetAttachedShaders(GET_DISPATCH(), (HANDLE(container), max < 16 ? max : 16, &n, names));
		for (GLsizei i = 0; i < n; i++) {
			((void **)objects)[i] = TO_HANDLE(names[i]);
		}
		if (count != NULL) {
			*(GLsizei *)count = n;
		}
		b2l_gli_leave((unsigned)idx_get_attached_objects);
	}
}

static void *
gli_get_handle(void *c, uint32_t pname)
{
	GLint program = 0;
	if (begin(c, (unsigned)idx_get_handle, pname) && pname == E_PROGRAM_OBJECT_ARB) {
		CALL_GetIntegerv(GET_DISPATCH(), (E_CURRENT_PROGRAM, &program));
	}
	return TO_HANDLE(program);
}

/* EXT_debug_marker: Mesa has none; nothing to do. */
static void
gli_marker(void *c, int32_t length, const void *marker)
{
	(void)length;
	(void)marker;
	begin(c, (unsigned)idx_insert_marker, 0);
}

static void
gli_push_marker(void *c, int32_t length, const void *marker)
{
	(void)length;
	(void)marker;
	begin(c, (unsigned)idx_push_marker, 0);
}

static void
gli_pop_marker(void *c)
{
	begin(c, (unsigned)idx_pop_marker, 0);
}

static int
gli_index(const char *name)
{
	for (unsigned i = 0; i < B2L_GLI_COUNT; i++) {
		if (strcmp(b2l_gli_name[i], name) == 0) {
			return (int)i;
		}
	}
	note("GLI entry %s not in this table", name);
	return -1;
}

static void
gli_own(struct b2l_eagl *e, const char *name, void *function, int *index)
{
	*index = gli_index(name);
	if (*index >= 0) {
		e->macro.disp[*index] = function;
		gli_slot[*index] = -2;
	} else {
		*index = 0;
	}
}

/* Mesa's dispatch offset for every entry, from its GL names, and the table. */
static void
gli_fill(struct b2l_eagl *e)
{
	unsigned mapped = 0;
	for (unsigned i = 0; i < B2L_GLI_COUNT; i++) {
		char names[200];
		gli_slot[i] = -1;
		snprintf(names, sizeof(names), "%s", b2l_gli_gl[i]);
		for (char *save = NULL, *n = strtok_r(names, " ", &save); n != NULL; n = strtok_r(NULL, " ", &save)) {
			int offset = _mesa_glapi_get_proc_offset(n);
			if (offset >= 0) {
				gli_slot[i] = offset;
				mapped++;
				break;
			}
		}
		e->macro.disp[i] = b2l_gli_thunk[i];
	}
	gli_own(e, "get_string", (void *)gli_get_string, &idx_get_string);
	gli_own(e, "tex_image2D", (void *)gli_tex_image2D, &idx_tex_image2D);
	gli_own(e, "tex_sub_image2D", (void *)gli_tex_sub_image2D, &idx_tex_sub_image2D);
	gli_own(e, "finish_object_APPLE", (void *)gli_finish_object, &idx_finish_object);
	gli_own(e, "delete_object_ARB", (void *)gli_delete_object, &idx_delete_object);
	gli_own(e, "create_shader_object_ARB", (void *)gli_create_shader_object, &idx_create_shader_object);
	gli_own(e, "create_program_object_ARB", (void *)gli_create_program_object, &idx_create_program_object);
	gli_own(e, "attach_object_ARB", (void *)gli_attach_object, &idx_attach_object);
	gli_own(e, "detach_object_ARB", (void *)gli_detach_object, &idx_detach_object);
	gli_own(e, "get_object_parameteriv_ARB", (void *)gli_get_object_parameteriv, &idx_get_object_parameteriv);
	gli_own(e, "get_object_parameterfv_ARB", (void *)gli_get_object_parameterfv, &idx_get_object_parameterfv);
	gli_own(e, "get_info_log_ARB", (void *)gli_get_info_log, &idx_get_info_log);
	gli_own(e, "get_attached_objects_ARB", (void *)gli_get_attached_objects, &idx_get_attached_objects);
	gli_own(e, "get_handle_ARB", (void *)gli_get_handle, &idx_get_handle);
	gli_own(e, "insert_event_marker_EXT", (void *)gli_marker, &idx_insert_marker);
	gli_own(e, "push_group_marker_EXT", (void *)gli_push_marker, &idx_push_marker);
	gli_own(e, "pop_group_marker_EXT", (void *)gli_pop_marker, &idx_pop_marker);
	gli_own(e, "shader_source_ARB", (void *)gli_shader_source, &idx_shader_source);
	gli_own(e, "vertex_attrib_pointer_ARB", (void *)gli_attrib_pointer, &idx_attrib_pointer);
	gli_own(e, "draw_elements", (void *)gli_draw_elements, &idx_draw_elements);
	note("GLI table: %u entries, %u with a Mesa function, 20 our own", (unsigned)B2L_GLI_COUNT, mapped);
}

/* ---- IOSurfaces as EGL images ---- */

static bool
image_valid(const void *p)
{
	const struct b2l_image *img = p;
	return img >= &images[0] && img < &images[IMAGES] &&
	    ((const char *)img - (const char *)images) % sizeof(images[0]) == 0 &&
	    img->magic == IMAGE_MAGIC && img->res != NULL;
}

static bool
eagl_validate_egl_image(struct pipe_frontend_screen *fscreen, void *image)
{
	(void)fscreen;
	return image_valid(image);
}

static bool
eagl_get_egl_image(struct pipe_frontend_screen *fscreen, void *image, struct st_egl_image *out)
{
	(void)fscreen;
	if (!image_valid(image)) {
		return false;
	}
	struct b2l_image *img = image;
	pipe_resource_reference(&out->texture, img->res);
	out->format = img->res->format;
	out->level = 0;
	out->layer = 0;
	out->internalformat = 0;
	return true;
}

static void
image_release(struct b2l_image *img)
{
	if (img->magic == IMAGE_MAGIC) {
		pipe_resource_reference(&img->res, NULL);
		CFRelease(img->surface);
	}
	memset(img, 0, sizeof(*img));
}

/* The surface's plane as a Mesa resource, imported once and kept (LRU). */
static struct b2l_image *
image_get(IOSurfaceRef surface, unsigned plane, enum pipe_format format, unsigned width, unsigned height,
    char *why, size_t why_size)
{
	uint32_t sid = IOSurfaceGetID(surface);
	struct b2l_image *img = NULL, *victim = &images[0];
	pthread_mutex_lock(&image_lock);
	for (unsigned i = 0; i < IMAGES; i++) {
		struct b2l_image *it = &images[i];
		if (it->magic == IMAGE_MAGIC && it->surface == surface && it->surface_id == sid && it->plane == plane &&
		    it->format == format && it->width == width && it->height == height) {
			it->used = ++image_clock;
			pthread_mutex_unlock(&image_lock);
			return it;
		}
		if (it->magic != IMAGE_MAGIC || (victim->magic == IMAGE_MAGIC && it->used < victim->used)) {
			victim = it;
		}
	}
	size_t planes = IOSurfaceGetPlaneCount(surface);
	size_t stride = planes ? IOSurfaceGetBytesPerRowOfPlane(surface, plane) : IOSurfaceGetBytesPerRow(surface);
	size_t offset = 0;
	if (plane > 0) {
		if (plane >= planes) {
			snprintf(why, why_size, "plane %u of %zu", plane, planes);
			goto fail;
		}
		offset = (size_t)((const char *)IOSurfaceGetBaseAddressOfPlane(surface, plane) -
		    (const char *)IOSurfaceGetBaseAddress(surface));
	}
	if (stride % 64 || offset % 64) {
		snprintf(why, why_size, "rows of %zu bytes at offset %zu (the Mali needs multiples of 64)", stride, offset);
		goto fail;
	}
	struct pipe_frontend_screen *fscreen = b2l_fscreen();
	struct pipe_screen *screen = fscreen != NULL ? fscreen->screen : NULL;
	int fd = screen != NULL ? b2l_drm_surface_fd(sid, IOSurfaceGetAllocSize(surface)) : -1;
	if (fd < 0) {
		snprintf(why, why_size, "no surface fd (%d)", fd);
		goto fail;
	}
	struct winsys_handle wh;
	memset(&wh, 0, sizeof(wh));
	wh.type = WINSYS_HANDLE_TYPE_FD;
	wh.handle = (unsigned)fd;
	wh.stride = (unsigned)stride;
	wh.offset = (unsigned)offset;
	wh.modifier = DRM_FORMAT_MOD_LINEAR;
	struct pipe_resource templat;
	memset(&templat, 0, sizeof(templat));
	templat.target = PIPE_TEXTURE_2D;
	templat.format = format;
	templat.width0 = width;
	templat.height0 = height;
	templat.depth0 = 1;
	templat.array_size = 1;
	templat.usage = PIPE_USAGE_DEFAULT;
	templat.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_SHARED | PIPE_BIND_LINEAR;
	struct pipe_resource *res = screen->resource_from_handle(screen, &templat, &wh,
	    PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
	b2l_drm_surface_fd_close(fd);
	if (res == NULL) {
		snprintf(why, why_size, "resource_from_handle refused %s %ux%u rows %zu", util_format_short_name(format),
		    width, height, stride);
		goto fail;
	}
	image_release(victim);
	img = victim;
	img->magic = IMAGE_MAGIC;
	img->surface = (IOSurfaceRef)CFRetain(surface);
	img->surface_id = sid;
	img->plane = plane;
	img->format = format;
	img->width = width;
	img->height = height;
	img->res = res;
	img->used = ++image_clock;
fail:
	pthread_mutex_unlock(&image_lock);
	return img;
}

static enum pipe_format
format_of_surface(IOSurfaceRef surface)
{
	switch (IOSurfaceGetPixelFormat(surface)) {
	case 0x42475241:        /* 'BGRA' */
		return PIPE_FORMAT_B8G8R8A8_UNORM;
	case 0x52474241:        /* 'RGBA' */
		return PIPE_FORMAT_R8G8B8A8_UNORM;
	case 0x52476841:        /* 'RGhA' */
		return PIPE_FORMAT_R16G16B16A16_FLOAT;
	case 0x6c313072:        /* 'l10r' */
		return PIPE_FORMAT_B10G10R10A2_UNORM;
	case 0:
		return IOSurfaceGetBytesPerElement(surface) == 4 ? PIPE_FORMAT_B8G8R8A8_UNORM : PIPE_FORMAT_NONE;
	default:
		return PIPE_FORMAT_NONE;
	}
}

static enum pipe_format
format_of_gl(uint32_t format, uint32_t type)
{
	bool byte = type == E_UNSIGNED_BYTE || type == E_UINT_8888_REV;
	bool half = type == E_HALF_FLOAT || type == E_HALF_FLOAT_OES;
	switch (format) {
	case E_BGRA:
		return byte ? PIPE_FORMAT_B8G8R8A8_UNORM : type == E_UINT_2101010_REV ? PIPE_FORMAT_B10G10R10A2_UNORM :
		    PIPE_FORMAT_NONE;
	case E_RGBA:
		return byte ? PIPE_FORMAT_R8G8B8A8_UNORM : half ? PIPE_FORMAT_R16G16B16A16_FLOAT :
		    type == E_UINT_2101010_REV ? PIPE_FORMAT_R10G10B10A2_UNORM :
		    type == E_FLOAT ? PIPE_FORMAT_R32G32B32A32_FLOAT : PIPE_FORMAT_NONE;
	case E_RED:
	case E_LUMINANCE:
	case E_ALPHA:
		return byte ? PIPE_FORMAT_R8_UNORM : type == E_UNSIGNED_SHORT ? PIPE_FORMAT_R16_UNORM :
		    half ? PIPE_FORMAT_R16_FLOAT : PIPE_FORMAT_NONE;
	case E_RG:
	case E_LUMINANCE_ALPHA:
		return byte ? PIPE_FORMAT_R8G8_UNORM : type == E_UNSIGNED_SHORT ? PIPE_FORMAT_R16G16_UNORM :
		    half ? PIPE_FORMAT_R16G16_FLOAT : PIPE_FORMAT_NONE;
	default:
		return PIPE_FORMAT_NONE;
	}
}

/* The image as the texture bound to target, through Mesa's EGL image path. */
static bool
bind_image(uint32_t target, struct b2l_image *img, GLenum *error)
{
	struct _glapi_table *d = GET_DISPATCH();
	while (CALL_GetError(d, ()) != GL_NO_ERROR) {
	}
	CALL_EGLImageTargetTexture2DOES(d, (target, (GLeglImageOES)img));
	*error = CALL_GetError(d, ());
	return *error == GL_NO_ERROR;
}

/* ---- the Objective-C class ---- */

static struct b2l_eagl *
eagl_of(id self)
{
	struct b2l_eagl **slot = self != NULL ? object_getIndexedIvars(self) : NULL;
	return slot != NULL ? *slot : NULL;
}

static unsigned long
eagl_api(id self, SEL cmd)
{
	(void)self;
	(void)cmd;
	static unsigned logged;
	if (!logged++) {
		note("API asked: 3");
	}
	return 3;
}

static void *
eagl_macro(id self, SEL cmd)
{
	(void)cmd;
	struct b2l_eagl *e = eagl_of(self);
	static unsigned logged;
	if (logged++ < 4) {
		note("getMacroContextPrivate -> %p", (void *)e);
	}
	return e != NULL ? (void *)&e->macro : NULL;
}

static BOOL
eagl_attach(id self, SEL cmd, uint32_t target, IOSurfaceRef surface, BOOL inverted)
{
	(void)cmd;
	struct b2l_eagl *e = eagl_of(self);
	char why[160] = "";
	GLenum error = 0;
	bool ok = false;
	attaches++;
	if (surface == NULL || !begin(e, B2L_GLI_COUNT, 0)) {
		snprintf(why, sizeof(why), "no surface or context");
	} else {
		enum pipe_format format = format_of_surface(surface);
		struct b2l_image *img = NULL;
		if (format == PIPE_FORMAT_NONE) {
			snprintf(why, sizeof(why), "pixel format 0x%x not handled", IOSurfaceGetPixelFormat(surface));
		} else {
			img = image_get(surface, 0, format, (unsigned)IOSurfaceGetWidth(surface),
			    (unsigned)IOSurfaceGetHeight(surface), why, sizeof(why));
		}
		ok = img != NULL && bind_image(target, img, &error);
	}
	if (!ok) {
		import_failures++;
	}
	if (!ok || attaches <= 12) {
		note("attachImage target 0x%x surface %u %zux%zu fmt 0x%x rows %zu inverted %d -> %s %s (GL 0x%x)",
		    target, surface ? IOSurfaceGetID(surface) : 0, surface ? IOSurfaceGetWidth(surface) : 0,
		    surface ? IOSurfaceGetHeight(surface) : 0, surface ? IOSurfaceGetPixelFormat(surface) : 0,
		    surface ? IOSurfaceGetBytesPerRow(surface) : 0, (int)inverted, ok ? "ok" : "FAILED", why, error);
	}
	return ok;
}

/*
 * Boot 49: two small layer surfaces (6 x 90, 6 x 6) had rows of 32 bytes,
 * which the Mali cannot sample in place (64-byte multiples), so their layers
 * were missing. Such surfaces are copied instead: locked for reading and
 * uploaded into the bound texture with glTexImage2D (row length from the
 * surface). Core Animation calls texImageIOSurface again when they change.
 */
static bool
upload_copy(IOSurfaceRef surface, unsigned plane, uint32_t target, uint32_t internal, int32_t width,
    int32_t height, uint32_t format, uint32_t type, GLenum *error)
{
	size_t planes = IOSurfaceGetPlaneCount(surface);
	size_t stride = planes ? IOSurfaceGetBytesPerRowOfPlane(surface, plane) : IOSurfaceGetBytesPerRow(surface);
	size_t bpe = planes ? IOSurfaceGetBytesPerElementOfPlane(surface, plane) : IOSurfaceGetBytesPerElement(surface);
	if (bpe == 0 || stride % bpe != 0 || (planes == 0 && plane != 0) || (planes != 0 && plane >= planes)) {
		return false;
	}
	if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) != 0) {
		return false;
	}
	const void *base = planes ? IOSurfaceGetBaseAddressOfPlane(surface, plane) : IOSurfaceGetBaseAddress(surface);
	struct _glapi_table *d = GET_DISPATCH();
	while (CALL_GetError(d, ()) != GL_NO_ERROR) {
	}
	GLint row = 0, align = 4;
	CALL_GetIntegerv(d, (GL_UNPACK_ROW_LENGTH, &row));
	CALL_GetIntegerv(d, (GL_UNPACK_ALIGNMENT, &align));
	CALL_PixelStorei(d, (GL_UNPACK_ROW_LENGTH, (GLint)(stride / bpe)));
	CALL_PixelStorei(d, (GL_UNPACK_ALIGNMENT, 1));
	if (format == E_BGRA && (internal == E_RGBA || internal == E_RGBA8)) {
		internal = E_BGRA;
	}
	if (format == E_BGRA && type == E_UINT_8888_REV) {
		type = E_UNSIGNED_BYTE;
	}
	if (type == E_HALF_FLOAT_OES) {
		type = E_HALF_FLOAT;
	}
	CALL_TexImage2D(d, (target, 0, (GLint)internal, width, height, 0, format, type, base));
	*error = CALL_GetError(d, ());
	CALL_PixelStorei(d, (GL_UNPACK_ROW_LENGTH, row));
	CALL_PixelStorei(d, (GL_UNPACK_ALIGNMENT, align));
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	return *error == GL_NO_ERROR;
}

static BOOL
eagl_teximage(id self, SEL cmd, IOSurfaceRef surface, uint32_t target, uint32_t internal, int32_t width,
    int32_t height, uint32_t format, uint32_t type, uint32_t plane, BOOL invert)
{
	(void)cmd;
	struct b2l_eagl *e = eagl_of(self);
	char why[160] = "";
	GLenum error = 0;
	bool ok = false;
	teximages++;
	if (surface == NULL || width <= 0 || height <= 0 || !begin(e, B2L_GLI_COUNT, 0)) {
		snprintf(why, sizeof(why), "no surface, size or context");
	} else {
		enum pipe_format pf = format_of_gl(format, type);
		struct b2l_image *img = NULL;
		if (pf == PIPE_FORMAT_NONE) {
			snprintf(why, sizeof(why), "format 0x%x type 0x%x not handled", format, type);
		} else {
			img = image_get(surface, plane, pf, (unsigned)width, (unsigned)height, why, sizeof(why));
		}
		ok = img != NULL && bind_image(target, img, &error);
		if (img == NULL && pf != PIPE_FORMAT_NONE) {
			ok = upload_copy(surface, plane, target, internal, width, height, format, type, &error);
			copies++;
			if (ok && copies > 4) {
				return ok;      /* logged the first few times only */
			}
			size_t used = strlen(why);
			snprintf(why + used, sizeof(why) - used, "; copied instead: %s", ok ? "ok" : "FAILED");
		}
	}
	if (!ok) {
		import_failures++;
	}
	if (!ok || teximages <= 12) {
		note("texImageIOSurface surface %u target 0x%x internal 0x%x %dx%d format 0x%x type 0x%x plane %u invert %d"
		    " -> %s %s (GL 0x%x)", surface ? IOSurfaceGetID(surface) : 0, target, internal, width, height, format,
		    type, plane, (int)invert, ok ? "ok" : "FAILED", why, error);
	}
	return ok;
}

/* Selectors we lack: logged, then the usual "unrecognized selector". */
static BOOL
eagl_resolve(id cls, SEL cmd, SEL name)
{
	(void)cls;
	(void)cmd;
	static unsigned logged;
	if (logged++ < 64) {
		note("no method %s", sel_getName(name));
	}
	return NO;
}

static Class
eagl_class(void)
{
	static Class cls;
	if (cls != Nil) {
		return cls;
	}
	Class base = objc_getClass("NSObject");
	Class made = base != Nil ? objc_allocateClassPair(base, "B2LEAGLContext", 0) : Nil;
	if (made == Nil) {
		note("class B2LEAGLContext not made (NSObject %p)", (void *)base);
		return Nil;
	}
	class_addMethod(made, sel_registerName("API"), (IMP)eagl_api, "Q@:");
	class_addMethod(made, sel_registerName("getMacroContextPrivate"), (IMP)eagl_macro, "^v@:");
	class_addMethod(made, sel_registerName("attachImage:toCoreSurface:invertedRender:"), (IMP)eagl_attach,
	    "B@:I^vB");
	class_addMethod(made, sel_registerName("texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:"),
	    (IMP)eagl_teximage, "B@:^vIIiiIIIB");
	class_addMethod(object_getClass((id)made), sel_registerName("resolveInstanceMethod:"), (IMP)eagl_resolve, "B#::");
	objc_registerClassPair(made);
	cls = made;
	return cls;
}

/*
 * The EAGLContext-like object for GLESContext: one per process, an OpenGL ES
 * 3.0 context on the Panfrost screen with no window-system buffers (Core
 * Animation draws into framebuffers of its own), current on the calling
 * thread. NULL, logged, on any failure: the caller keeps the software path.
 */
void *
b2l_eagl_context_create(void)
{
	if (the_eagl != NULL) {
		return the_eagl->object;
	}
	uint64_t start = now_ms();
	struct pipe_frontend_screen *fscreen = b2l_fscreen();
	if (fscreen == NULL) {
		b2l_drm_note("eagl: no Panfrost screen");
		return NULL;
	}
	fscreen->get_egl_image = eagl_get_egl_image;
	fscreen->validate_egl_image = eagl_validate_egl_image;
	struct st_context_attribs attribs;
	memset(&attribs, 0, sizeof(attribs));
	attribs.profile = API_OPENGLES2;
	attribs.major = 3;
	attribs.minor = 0;
	attribs.visual.color_format = PIPE_FORMAT_NONE;
	attribs.visual.depth_stencil_format = PIPE_FORMAT_NONE;
	attribs.visual.accum_format = PIPE_FORMAT_NONE;
	enum st_context_error error = ST_CONTEXT_SUCCESS;
	struct st_context *st = st_api_create_context(fscreen, &attribs, &error, NULL);
	if (st == NULL) {
		note("context FAILED (error %d)", (int)error);
		return NULL;
	}
	if (!st_api_make_current(st, NULL, NULL)) {
		note("make current FAILED");
		st_destroy_context(st);
		return NULL;
	}
	Class cls = eagl_class();
	struct b2l_eagl *e = cls != Nil ? calloc(1, sizeof(*e)) : NULL;
	id object = e != NULL ? class_createInstance(cls, sizeof(struct b2l_eagl *)) : nil;
	if (object == nil) {
		note("object not made");
		free(e);
		st_api_make_current(NULL, NULL, NULL);
		st_destroy_context(st);
		return NULL;
	}
	*(struct b2l_eagl **)object_getIndexedIvars(object) = e;
	e->macro.rend = e;
	e->st = st;
	e->object = object;
	the_eagl = e;
	gli_fill(e);
	struct _glapi_table *d = GET_DISPATCH();
	const GLubyte *renderer = CALL_GetString(d, (GL_RENDERER));
	const GLubyte *version = CALL_GetString(d, (GL_VERSION));
	const GLubyte *extensions = CALL_GetString(d, (GL_EXTENSIONS));
	GLint max_texture = 0;
	CALL_GetIntegerv(d, (GL_MAX_TEXTURE_SIZE, &max_texture));
	Dl_info where;
	memset(&where, 0, sizeof(where));
	dladdr((const void *)b2l_eagl_context_create, &where);
	note("context ready in %llu ms: %s, %s, max texture %d, extensions %zu bytes, object %p, library at %p",
	    (unsigned long long)(now_ms() - start), renderer ? (const char *)renderer : "?",
	    version ? (const char *)version : "?", max_texture, extensions ? strlen((const char *)extensions) : 0,
	    (void *)object, where.dli_fbase);
	return object;
}

/*
 * Before the window server hands a display buffer to the framebuffer
 * (userspace/b2l-gpu.c hooks IOMFBDisplay::fb_swap_set_layer): GLESContext
 * never calls flush, finish or a fence itself (boot 47: none in 80 s), so
 * Panfrost kept each frame's batch pending while the panel showed the
 * buffer as it was. glFinish submits every batch; B2LMali's submission
 * waits for the GPU, so the IOSurface holds the frame on return.
 */
int
b2l_eagl_flush(void)
{
	struct b2l_eagl *e = the_eagl;
	if (e == NULL) {
		return -1;
	}
	if (_mesa_glapi_get_context() != (void *)e->st->ctx && !st_api_make_current(e->st, NULL, NULL)) {
		return -2;
	}
	uint64_t start = now_ms();
	CALL_Finish(GET_DISPATCH(), ());
	uint64_t took = now_ms() - start;
	swap_flushes++;
	swap_flush_ms += took;
	if (swap_flushes <= 6) {
		note("swap %u: frame finished on the GPU in %llu ms", swap_flushes, (unsigned long long)took);
	}
	return 0;
}
