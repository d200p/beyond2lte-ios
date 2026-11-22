// SPDX-License-Identifier: BSD-2-Clause
/*
 * S10 port: Metal milestone M2a's test (docs/metal-on-panfrost.md).
 *
 * Run by the backboardd that userspace/b2l-gpu.c starts with B2L_MTL_TEST=1
 * (boot argument b2l_metal=1 shows the device to that process only). It uses
 * Metal as a client does -- MTLCreateSystemDefaultDevice, Metal's own
 * descriptor classes, the device's queue -- so what it checks is the path
 * Core Animation will take:
 *
 *   buffers (zeroed, from bytes), a blit (buffer copy and fill), a 64 x 64
 *   BGRA8 texture written and read back, an IOSurface texture, two render
 *   passes whose load action clears them (the IOSurface read back through its
 *   own lock, pixel by pixel), a texture copy, a sampler, QuartzCore's library
 *   and two render pipelines compiled by Panfrost (std_vert0 + attachment_clear,
 *   and fixed_vert + the window server's uber shader with fc_texture_function
 *   given by name, as Core Animation gives it).
 *
 * M2b draws: a triangle onto the IOSurface (vertex buffer, setVertexBytes,
 * viewport orientation) and a textured quad onto the texture (vertex_id strip,
 * a 2 x 2 texture and a sampler), both read back pixel by pixel; an indexed
 * draw, culling, a scissor, blending; a compute kernel copying a texture
 * (QuartzCore's read_write_surf_compute); a shared event signalled by a
 * command buffer, a heap, a fence, a texture view; and last, Core Animation's
 * own Metal renderer (CARenderer) drawing two layers into a texture. One kernel
 * log line per step ("B2L GPU UI: mtl M2: test: ..."), with ok or FAILED,
 * through a B2LMali client of its own that only logs.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "b2l_mali_uapi.h"

void b2l_mtl_test_run(void) __attribute__((visibility("hidden")));

typedef mach_port_t io_object_t, io_service_t, io_connect_t;
extern const mach_port_t kIOMainPortDefault;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t port, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallMethod(io_connect_t c, uint32_t selector, const uint64_t *in, uint32_t in_count,
    const void *struct_in, size_t struct_in_size, uint64_t *out, uint32_t *out_count, void *struct_out,
    size_t *struct_out_size);

#define QC_LIBRARY "/System/Library/Frameworks/QuartzCore.framework/default.metallib"
#define SIDE 64
#define ROW (SIDE * 4)

typedef struct {
	double red, green, blue, alpha;
} clear_color;
typedef struct {
	unsigned long x, y, z;
} origin_t;
typedef struct {
	unsigned long width, height, depth;
} extent_t;
typedef struct {
	origin_t origin;
	extent_t size;
} region_t;

#define MSG(ret, ...) ((ret (*)(id, SEL, ##__VA_ARGS__))objc_msgSend)

static io_connect_t test_log;
static unsigned passed, failed;

static void
say(const char *format, ...)
{
	char text[B2L_MALI_MAX_LOG - 1];
	int head = snprintf(text, sizeof(text), "mtl M2: test: ");
	va_list ap;
	va_start(ap, format);
	vsnprintf(text + head, sizeof(text) - (size_t)head, format, ap);
	va_end(ap);
	if (test_log == MACH_PORT_NULL) {
		io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("B2LMali"));
		if (service != MACH_PORT_NULL) {
			/* only kB2LMaliLog is sent: the kernel's runtime is not reached */
			if (IOServiceOpen(service, mach_task_self(), B2L_MALI_CLIENT_BO, &test_log) != KERN_SUCCESS) {
				test_log = MACH_PORT_NULL;
			}
			IOObjectRelease(service);
		}
	}
	if (test_log != MACH_PORT_NULL) {
		IOConnectCallMethod(test_log, kB2LMaliLog, NULL, 0, text, strnlen(text, sizeof(text)), NULL, NULL, NULL,
		    NULL);
	}
}

static void
verdict(const char *what, bool ok, const char *format, ...)
{
	char detail[200] = "";
	va_list ap;
	va_start(ap, format);
	vsnprintf(detail, sizeof(detail), format, ap);
	va_end(ap);
	if (ok) {
		passed++;
	} else {
		failed++;
	}
	say("%s: %s%s%s", what, ok ? "ok" : "FAILED", detail[0] != 0 ? ", " : "", detail);
}

static uint64_t
now_ms(void)
{
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom / 1000000;
}

static SEL
S(const char *name)
{
	return sel_registerName(name);
}

static id
cls(const char *name)
{
	return (id)objc_getClass(name);
}

static id
make(const char *name)
{
	id c = cls(name);
	return c != nil ? MSG(id)(MSG(id)(c, S("alloc")), S("init")) : nil;
}

static id
nsstr(const char *text)
{
	return (id)CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
}

static const char *
name_of(id o)
{
	return o != nil ? object_getClassName(o) : "nil";
}

/* An NSError's description, into out. */
static const char *
error_text(id error, char *out, size_t size)
{
	out[0] = 0;
	id text = error != nil ? MSG(id)(error, S("localizedDescription")) : nil;
	if (text != nil) {
		CFStringGetCString((CFStringRef)text, out, (CFIndex)size, kCFStringEncodingUTF8);
	}
	return out;
}

static id
at(id array, unsigned long index)
{
	return array != nil ? MSG(id, unsigned long)(array, S("objectAtIndexedSubscript:"), index) : nil;
}

static IOSurfaceRef
make_surface(void)
{
	int values[] = { SIDE, SIDE, 4, ROW, 0x42475241 /* 'BGRA' */, ROW * SIDE };
	const void *keys[] = {
		kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfaceBytesPerRow,
		kIOSurfacePixelFormat, kIOSurfaceAllocSize,
	};
	const void *numbers[6];
	for (unsigned i = 0; i < 6; i++) {
		numbers[i] = CFNumberCreate(NULL, kCFNumberSInt32Type, &values[i]);
	}
	CFDictionaryRef properties = CFDictionaryCreate(NULL, keys, numbers, 6, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	IOSurfaceRef surface = IOSurfaceCreate(properties);
	CFRelease(properties);
	for (unsigned i = 0; i < 6; i++) {
		CFRelease(numbers[i]);
	}
	return surface;
}

/* A render encoder on `cb` whose pass first clears `texture`. */
static id
begin_pass(id cb, id texture, clear_color color)
{
	id pass = MSG(id)(cls("MTLRenderPassDescriptor"), S("renderPassDescriptor"));
	id a = at(MSG(id)(pass, S("colorAttachments")), 0);
	MSG(void, id)(a, S("setTexture:"), texture);
	MSG(void, unsigned long)(a, S("setLoadAction:"), 2);       /* MTLLoadActionClear */
	MSG(void, unsigned long)(a, S("setStoreAction:"), 1);      /* MTLStoreActionStore */
	MSG(void, clear_color)(a, S("setClearColor:"), color);
	return MSG(id, id)(cb, S("renderCommandEncoderWithDescriptor:"), pass);
}

/* A render pass on `cb` whose only work is its load action. */
static void
clear_pass(id cb, id texture, clear_color color)
{
	MSG(void)(begin_pass(cb, texture, color), S("endEncoding"));
}

/* Commit, wait; the status (4 completed, 5 error) and the error's text. */
static unsigned long
run(id cb, char *why, size_t why_size, uint64_t *ms)
{
	uint64_t t0 = now_ms();
	MSG(void)(cb, S("commit"));
	MSG(void)(cb, S("waitUntilCompleted"));
	*ms = now_ms() - t0;
	error_text(MSG(id)(cb, S("error")), why, why_size);
	return MSG(unsigned long)(cb, S("status"));
}

/* Pixels of BGRA8 rows that are not `bgra` (each channel within one). */
static unsigned
wrong_pixels(const uint8_t *rows, size_t row_bytes, const uint8_t bgra[4], uint32_t *first)
{
	unsigned wrong = 0;
	for (unsigned y = 0; y < SIDE; y++) {
		for (unsigned x = 0; x < SIDE; x++) {
			const uint8_t *p = rows + y * row_bytes + x * 4;
			bool bad = false;
			for (unsigned c = 0; c < 4; c++) {
				int d = (int)p[c] - (int)bgra[c];
				bad |= d < -1 || d > 1;
			}
			if (bad && wrong++ == 0) {
				*first = (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
			}
		}
	}
	return wrong;
}

/* A pipeline of two QuartzCore functions; std: std_vert0's vertex layout,
 * uber: fc_texture_function 0 given by name. Nil when refused. */
static id
pipeline_test(id device, id library, const char *vertex, const char *fragment, bool std, bool uber)
{
	char label[160], why[200];
	snprintf(label, sizeof(label), "pipeline %s + %s%s", vertex, fragment, uber ? " (fc_texture_function 0 by name)" : "");
	id v = MSG(id, id)(library, S("newFunctionWithName:"), nsstr(vertex));
	id f = nil, error = nil;
	if (uber) {
		id values = make("MTLFunctionConstantValues");
		uint8_t clr = 0;
		MSG(void, const void *, unsigned long, id)(values, S("setConstantValue:type:withName:"), &clr,
		    49 /* MTLDataTypeUChar */, nsstr("fc_texture_function"));
		f = MSG(id, id, id, id *)(library, S("newFunctionWithName:constantValues:error:"), nsstr(fragment), values,
		    &error);
	} else {
		f = MSG(id, id)(library, S("newFunctionWithName:"), nsstr(fragment));
	}
	if (v == nil || f == nil) {
		verdict(label, false, "functions %s and %s %s", name_of(v), name_of(f), error_text(error, why, sizeof(why)));
		return nil;
	}
	id d = make("MTLRenderPipelineDescriptor");
	MSG(void, id)(d, S("setVertexFunction:"), v);
	MSG(void, id)(d, S("setFragmentFunction:"), f);
	MSG(void, unsigned long)(at(MSG(id)(d, S("colorAttachments")), 0), S("setPixelFormat:"), 80);
	if (std) {
		/* std_vert0: position float4 (0), texcoord float2 (1), colour half4 (3) */
		id vd = MSG(id)(cls("MTLVertexDescriptor"), S("vertexDescriptor"));
		id attributes = MSG(id)(vd, S("attributes"));
		static const unsigned long slot[] = { 0, 1, 3 }, format[] = { 31, 29, 27 }, offset[] = { 0, 16, 24 };
		for (unsigned i = 0; i < 3; i++) {
			id a = at(attributes, slot[i]);
			MSG(void, unsigned long)(a, S("setFormat:"), format[i]);
			MSG(void, unsigned long)(a, S("setOffset:"), offset[i]);
			MSG(void, unsigned long)(a, S("setBufferIndex:"), 0);
		}
		MSG(void, unsigned long)(at(MSG(id)(vd, S("layouts")), 0), S("setStride:"), 32);
		MSG(void, id)(d, S("setVertexDescriptor:"), vd);
	}
	uint64_t t0 = now_ms();
	error = nil;
	id p = MSG(id, id, id *)(device, S("newRenderPipelineStateWithDescriptor:error:"), d, &error);
	verdict(label, p != nil, "%s in %llu ms%s%s", name_of(p), (unsigned long long)(now_ms() - t0),
	    error != nil ? ": " : "", error_text(error, why, sizeof(why)));
	return p;
}

static const float identity4[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

/* " (x,y)=AARRGGBB" appended to out. */
static void
pixel_text(const uint8_t *rows, size_t row_bytes, unsigned x, unsigned y, char *out, size_t size)
{
	const uint8_t *p = rows + y * row_bytes + x * 4;
	size_t n = strlen(out);
	snprintf(out + n, size - n, " (%u,%u)=%02x%02x%02x%02x", x, y, p[3], p[2], p[1], p[0]);
}

static bool
near(const uint8_t *p, const uint8_t *want, int slack)
{
	for (unsigned c = 0; c < 4; c++) {
		int d = (int)p[c] - (int)want[c];
		if (d < -slack || d > slack) {
			return false;
		}
	}
	return true;
}

/*
 * M2b, a triangle: std_vert0 + attachment_clear (which writes zero), the
 * vertices from a buffer, the matrix from setVertexBytes, onto the IOSurface
 * cleared to (0.25, 0.5, 0.75, 1). NDC (-1, 1), (1, 1), (-1, -1) is the top
 * left half of the target in Metal (row 0 at the top): a pixel whose centre
 * has x + y < 64 is zero, one with x + y > 64 keeps the clear colour, the
 * diagonal itself (x + y = 63) is left out.
 */
static void
draw_triangle(id device, id queue, id pipeline, id target, IOSurfaceRef surface)
{
	struct vertex {
		float position[4], st[2];
		uint16_t colour[4];
	} vertices[3] = {
		{ { -1, 1, 0, 1 }, { 0, 0 }, { 0x3c00, 0, 0, 0x3c00 } },
		{ { 1, 1, 0, 1 }, { 1, 0 }, { 0, 0x3c00, 0, 0x3c00 } },
		{ { -1, -1, 0, 1 }, { 0, 1 }, { 0, 0, 0x3c00, 0x3c00 } },
	};
	char why[200], probes[160] = "";
	uint64_t ms = 0;
	id vb = MSG(id, const void *, unsigned long, unsigned long)(device, S("newBufferWithBytes:length:options:"),
	    vertices, sizeof(vertices), 0);
	id cb = MSG(id)(queue, S("commandBuffer"));
	id e = begin_pass(cb, target, (clear_color){ 0.25, 0.5, 0.75, 1.0 });
	MSG(void, id)(e, S("setRenderPipelineState:"), pipeline);
	MSG(void, id, unsigned long, unsigned long)(e, S("setVertexBuffer:offset:atIndex:"), vb, 0, 0);
	MSG(void, const void *, unsigned long, unsigned long)(e, S("setVertexBytes:length:atIndex:"), identity4,
	    sizeof(identity4), 2);
	MSG(void, unsigned long, unsigned long, unsigned long)(e, S("drawPrimitives:vertexStart:vertexCount:"),
	    3 /* MTLPrimitiveTypeTriangle */, 0, 3);
	MSG(void)(e, S("endEncoding"));
	unsigned long status = run(cb, why, sizeof(why), &ms);
	static const uint8_t blue[4] = { 191, 128, 64, 255 }, zero[4] = { 0, 0, 0, 0 };
	unsigned wrong = 0, inside = 0;
	IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
	const uint8_t *rows = IOSurfaceGetBaseAddress(surface);
	size_t row_bytes = IOSurfaceGetBytesPerRow(surface);
	for (unsigned y = 0; y < SIDE; y++) {
		for (unsigned x = 0; x < SIDE; x++) {
			if (x + y == SIDE - 1) {
				continue;
			}
			bool in = x + y < SIDE - 1;
			wrong += !near(rows + y * row_bytes + x * 4, in ? zero : blue, 1);
			inside += in;
		}
	}
	pixel_text(rows, row_bytes, 4, 4, probes, sizeof(probes));
	pixel_text(rows, row_bytes, 59, 4, probes, sizeof(probes));
	pixel_text(rows, row_bytes, 4, 59, probes, sizeof(probes));
	pixel_text(rows, row_bytes, 59, 59, probes, sizeof(probes));
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	verdict("draw: triangle (std_vert0 + attachment_clear) into the IOSurface", vb != nil && status == 4 &&
	    wrong == 0, "status %lu, %u of %u pixels wrong (%u inside), %llu ms;%s %s", status, wrong,
	    (unsigned)(SIDE * SIDE - SIDE), inside, (unsigned long long)ms, probes, why);
}

/*
 * M2b, a textured quad: image_rect_blit_vert + image_rect_blit_frag (a
 * QuartzCore image: four vertices from [[vertex_id]] as a strip, its
 * rectangle in vertex buffer 1, the matrix in 2, a 2 x 2 transform in 5), a
 * 2 x 2 texture at texture(3) with a nearest sampler at sampler(0), the
 * colour multiplier u[1] = 1 in fragment buffer 1. The rectangle is NDC
 * (-1, -1) to (0, 1), st (0, 0) to (1, 1): the left half of the target, with
 * st's row 0 at the bottom. So the top left quarter shows texel (0, 1) blue,
 * the top right one (1, 1) white, the bottom left (0, 0) red, the bottom
 * right (1, 0) green; the right half keeps the clear colour, black. Rows and
 * columns where a boundary falls are left out.
 */
static void
draw_quad(id device, id queue, id pipeline, id target)
{
	char why[200], probes[200] = "";
	uint64_t ms = 0;
	id td = MSG(id, unsigned long, unsigned long, unsigned long, bool)(cls("MTLTextureDescriptor"),
	    S("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"), 80, 2, 2, false);
	MSG(void, unsigned long)(td, S("setUsage:"), 1);
	id texels = MSG(id, id)(device, S("newTextureWithDescriptor:"), td);
	/* BGRA: row 0 red, green; row 1 blue, white */
	static const uint8_t colours[16] = { 0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255 };
	region_t r = { { 0, 0, 0 }, { 2, 2, 1 } };
	if (texels != nil) {
		MSG(void, region_t, unsigned long, const void *, unsigned long)(texels,
		    S("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"), r, 0, colours, 8);
	}
	id sd = make("MTLSamplerDescriptor");          /* nearest, clamp to edge, normalised */
	id sampler = MSG(id, id)(device, S("newSamplerStateWithDescriptor:"), sd);
	struct {
		float min[2], max[2], st_min[2], st_max[2];
		uint32_t edge_mask, pad;
	} rect = { { -1, -1 }, { 0, 1 }, { 0, 0 }, { 1, 1 }, 0, 0 };
	static const float transform[4] = { 1, 0, 0, 1 };         /* float2x2 identity */
	static const float u[8] = { 0, 0, 0, 0, 1, 1, 1, 1 };     /* u[1] = 1 */
	id cb = MSG(id)(queue, S("commandBuffer"));
	id e = begin_pass(cb, target, (clear_color){ 0.0, 0.0, 0.0, 1.0 });
	MSG(void, id)(e, S("setRenderPipelineState:"), pipeline);
	MSG(void, const void *, unsigned long, unsigned long)(e, S("setVertexBytes:length:atIndex:"), &rect,
	    sizeof(rect), 1);
	MSG(void, const void *, unsigned long, unsigned long)(e, S("setVertexBytes:length:atIndex:"), identity4,
	    sizeof(identity4), 2);
	MSG(void, const void *, unsigned long, unsigned long)(e, S("setVertexBytes:length:atIndex:"), transform,
	    sizeof(transform), 5);
	MSG(void, const void *, unsigned long, unsigned long)(e, S("setFragmentBytes:length:atIndex:"), u, sizeof(u), 1);
	MSG(void, id, unsigned long)(e, S("setFragmentTexture:atIndex:"), texels, 3);
	MSG(void, id, unsigned long)(e, S("setFragmentSamplerState:atIndex:"), sampler, 0);
	MSG(void, unsigned long, unsigned long, unsigned long)(e, S("drawPrimitives:vertexStart:vertexCount:"),
	    4 /* MTLPrimitiveTypeTriangleStrip */, 0, 4);
	MSG(void)(e, S("endEncoding"));
	unsigned long status = run(cb, why, sizeof(why), &ms);
	static uint8_t back[ROW * SIDE];
	region_t whole = { { 0, 0, 0 }, { SIDE, SIDE, 1 } };
	MSG(void, void *, unsigned long, region_t, unsigned long)(target, S("getBytes:bytesPerRow:fromRegion:mipmapLevel:"),
	    back, ROW, whole, 0);
	static const uint8_t black[4] = { 0, 0, 0, 255 };
	unsigned wrong = 0, checked = 0;
	for (unsigned y = 0; y < SIDE; y++) {
		for (unsigned x = 0; x < SIDE; x++) {
			if (y == 31 || y == 32 || x == 15 || x == 16 || x == 31 || x == 32) {
				continue;
			}
			const uint8_t *want = x > 32 ? black : colours + ((y < 32 ? 1u : 0u) * 2 + (x < 16 ? 0u : 1u)) * 4;
			wrong += !near(back + y * ROW + x * 4, want, 2);
			checked++;
		}
	}
	pixel_text(back, ROW, 8, 8, probes, sizeof(probes));
	pixel_text(back, ROW, 24, 8, probes, sizeof(probes));
	pixel_text(back, ROW, 8, 56, probes, sizeof(probes));
	pixel_text(back, ROW, 24, 56, probes, sizeof(probes));
	pixel_text(back, ROW, 48, 32, probes, sizeof(probes));
	verdict("draw: textured quad (image_rect_blit) into the texture", texels != nil && sampler != nil &&
	    status == 4 && wrong == 0, "status %lu, %u of %u pixels wrong, %llu ms;%s %s", status, wrong, checked,
	    (unsigned long long)ms, probes, why);
}



/* ---- more draws, compute, events, heaps, views (M2b) ---- */

typedef struct {
	unsigned long x, y, width, height;
} mtl_scissor_rect;

enum shape {
	SHAPE_NONE,             /* nothing drawn: the clear colour everywhere */
	SHAPE_TRIANGLE,         /* the top left half, as in draw_triangle */
	SHAPE_LEFT_HALF,        /* x < 32 */
	SHAPE_ALL,
};

/* attachment_clear (zero) over the IOSurface cleared blue, one way or
 * another; `shape` is what must come out zero. */
static void
solid_draw(id device, id queue, id pipeline, id target, IOSurfaceRef surface, const char *label, enum shape shape,
    unsigned long cull, bool indexed, bool full, bool scissor)
{
	struct vertex {
		float position[4], st[2];
		uint16_t colour[4];
	} vertices[6] = {
		{ { -1, 1, 0, 1 }, { 0, 0 }, { 0, 0, 0, 0 } },
		{ { 1, 1, 0, 1 }, { 1, 0 }, { 0, 0, 0, 0 } },
		{ { -1, -1, 0, 1 }, { 0, 1 }, { 0, 0, 0, 0 } },
		{ { 1, 1, 0, 1 }, { 1, 0 }, { 0, 0, 0, 0 } },
		{ { 1, -1, 0, 1 }, { 1, 1 }, { 0, 0, 0, 0 } },
		{ { -1, -1, 0, 1 }, { 0, 1 }, { 0, 0, 0, 0 } },
	};
	static const uint16_t indices[6] = { 0, 1, 2, 3, 4, 5 };
	char why[200], probes[160] = "";
	uint64_t ms = 0;
	unsigned count = full ? 6 : 3;
	id vb = MSG(id, const void *, unsigned long, unsigned long)(device, S("newBufferWithBytes:length:options:"),
	    vertices, sizeof(vertices), 0);
	id ib = MSG(id, const void *, unsigned long, unsigned long)(device, S("newBufferWithBytes:length:options:"),
	    indices, sizeof(indices), 0);
	id cb = MSG(id)(queue, S("commandBuffer"));
	id e = begin_pass(cb, target, (clear_color){ 0.25, 0.5, 0.75, 1.0 });
	MSG(void, id)(e, S("setRenderPipelineState:"), pipeline);
	MSG(void, unsigned long)(e, S("setCullMode:"), cull);
	if (scissor) {
		mtl_scissor_rect r = { 0, 0, SIDE / 2, SIDE };
		MSG(void, mtl_scissor_rect)(e, S("setScissorRect:"), r);
	}
	MSG(void, id, unsigned long, unsigned long)(e, S("setVertexBuffer:offset:atIndex:"), vb, 0, 0);
	MSG(void, const void *, unsigned long, unsigned long)(e, S("setVertexBytes:length:atIndex:"), identity4,
	    sizeof(identity4), 2);
	if (indexed) {
		MSG(void, unsigned long, unsigned long, unsigned long, id, unsigned long)(e,
		    S("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:"), 3, count,
		    0 /* MTLIndexTypeUInt16 */, ib, 0);
	} else {
		MSG(void, unsigned long, unsigned long, unsigned long)(e, S("drawPrimitives:vertexStart:vertexCount:"), 3, 0,
		    count);
	}
	MSG(void)(e, S("endEncoding"));
	unsigned long status = run(cb, why, sizeof(why), &ms);
	static const uint8_t blue[4] = { 191, 128, 64, 255 }, zero[4] = { 0, 0, 0, 0 };
	unsigned wrong = 0, checked = 0;
	IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
	const uint8_t *rows = IOSurfaceGetBaseAddress(surface);
	size_t row_bytes = IOSurfaceGetBytesPerRow(surface);
	for (unsigned y = 0; y < SIDE; y++) {
		for (unsigned x = 0; x < SIDE; x++) {
			bool in;
			switch (shape) {
			case SHAPE_TRIANGLE:
				if (x + y == SIDE - 1) {
					continue;
				}
				in = x + y < SIDE - 1;
				break;
			case SHAPE_LEFT_HALF:
				in = x < SIDE / 2;
				break;
			case SHAPE_ALL:
				in = true;
				break;
			default:
				in = false;
				break;
			}
			wrong += !near(rows + y * row_bytes + x * 4, in ? zero : blue, 1);
			checked++;
		}
	}
	pixel_text(rows, row_bytes, 4, 4, probes, sizeof(probes));
	pixel_text(rows, row_bytes, 59, 4, probes, sizeof(probes));
	pixel_text(rows, row_bytes, 4, 59, probes, sizeof(probes));
	pixel_text(rows, row_bytes, 59, 59, probes, sizeof(probes));
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	verdict(label, vb != nil && ib != nil && status == 4 && wrong == 0,
	    "status %lu, %u of %u pixels wrong, %llu ms;%s %s", status, wrong, checked, (unsigned long long)ms, probes,
	    why);
}

/* attachment_clear with blending source alpha / one minus source alpha:
 * its zero alpha leaves the target as it was. */
static id
blend_pipeline(id device, id library)
{
	id v = MSG(id, id)(library, S("newFunctionWithName:"), nsstr("std_vert0_lph"));
	id f = MSG(id, id)(library, S("newFunctionWithName:"), nsstr("attachment_clear_frag_lph"));
	id d = make("MTLRenderPipelineDescriptor");
	MSG(void, id)(d, S("setVertexFunction:"), v);
	MSG(void, id)(d, S("setFragmentFunction:"), f);
	id c = at(MSG(id)(d, S("colorAttachments")), 0);
	MSG(void, unsigned long)(c, S("setPixelFormat:"), 80);
	MSG(void, bool)(c, S("setBlendingEnabled:"), true);
	MSG(void, unsigned long)(c, S("setSourceRGBBlendFactor:"), 4);         /* SourceAlpha */
	MSG(void, unsigned long)(c, S("setDestinationRGBBlendFactor:"), 5);    /* OneMinusSourceAlpha */
	MSG(void, unsigned long)(c, S("setSourceAlphaBlendFactor:"), 4);
	MSG(void, unsigned long)(c, S("setDestinationAlphaBlendFactor:"), 5);
	id vd = MSG(id)(cls("MTLVertexDescriptor"), S("vertexDescriptor"));
	id attributes = MSG(id)(vd, S("attributes"));
	static const unsigned long slot[] = { 0, 1, 3 }, format[] = { 31, 29, 27 }, offset[] = { 0, 16, 24 };
	for (unsigned i = 0; i < 3; i++) {
		id a = at(attributes, slot[i]);
		MSG(void, unsigned long)(a, S("setFormat:"), format[i]);
		MSG(void, unsigned long)(a, S("setOffset:"), offset[i]);
		MSG(void, unsigned long)(a, S("setBufferIndex:"), 0);
	}
	MSG(void, unsigned long)(at(MSG(id)(vd, S("layouts")), 0), S("setStride:"), 32);
	MSG(void, id)(d, S("setVertexDescriptor:"), vd);
	id error = nil;
	char why[200];
	id p = MSG(id, id, id *)(device, S("newRenderPipelineStateWithDescriptor:error:"), d, &error);
	verdict("pipeline std_vert0 + attachment_clear, blending", p != nil, "%s %s", name_of(p),
	    error_text(error, why, sizeof(why)));
	return p;
}

/*
 * M2c (boot 74): Core Animation's own draw, outside Core Animation. Its frame
 * stayed zero though at commit its matrix, vertices and indices were right.
 * Here is the same pipeline (fixed_vert_lph_spc + fixed_frag_lph_cph with all
 * 60 of its fragment constants, typed as the AIR declares them:
 * fc_attachment_count 1, fc_framebuffer_fetch 1, fc_blend_function 0x3e, the
 * rest 0; the 5 vertex ones 0) and the same vertex layout (stride 48,
 * colour half4 at 32). The matrix is the one it had at commit (pixels to NDC),
 * with its zero fragment uniforms and its indices 0 1 2 2 3 0. One red layer
 * goes over the left half of a target cleared blue. With `fetch` off,
 * fc_framebuffer_fetch and fc_blend_function are 0, to tell the shader's
 * blending apart from the rest.
 */
enum { CA_B = 53, CA_U = 49, CA_U2 = 50, CA_U4 = 52 };     /* MTLDataType Bool, UChar, UChar2, UChar4 */
static const struct {
	const char *name;
	unsigned char type;
} ca_constants[60] = {
	{ "fc_generic", CA_B }, { "fc_vertex_layout", CA_U }, { "fc_framebuffer_fetch", CA_B },
	{ "fc_attachment_count", CA_U }, { "fc_has_lod_bias", CA_B }, { "fc_has_noise_scale", CA_B },
	{ "fc_has_ycbcr_matrix", CA_B }, { "fc_has_gamma_luts", CA_B }, { "fc_has_asg_77_weights", CA_B },
	{ "fc_has_attachment_0", CA_B }, { "fc_has_attachment_1", CA_B }, { "fc_has_attachment_2", CA_B },
	{ "fc_has_image_0A", CA_B }, { "fc_has_image_1A", CA_B }, { "fc_has_image_0B", CA_B },
	{ "fc_has_image_1B", CA_B }, { "fc_has_image_0C", CA_B }, { "fc_has_image_1C", CA_B },
	{ "fc_has_noise_tex", CA_B }, { "fc_has_lut_tex", CA_B }, { "fc_has_color_params", CA_B },
	{ "fc_has_color_trc0", CA_B }, { "fc_has_color_trc1", CA_B }, { "fc_has_color_cube0", CA_B },
	{ "fc_has_color_cube1", CA_B }, { "fc_texture_function", CA_U }, { "fc_blend_function", CA_U },
	{ "fc_destination_function", CA_U }, { "fc_image_count", CA_U }, { "fc_dest_index", CA_U },
	{ "fc_dest_count", CA_U }, { "fc_noise_width", CA_U }, { "fc_clamp_color", CA_B },
	{ "fc_extended_range", CA_B }, { "fc_unbounded_range", CA_B }, { "fc_debug_xr", CA_B },
	{ "fc_has_premultiply", CA_B }, { "fc_has_hdr_scale", CA_B }, { "fc_has_edr_factor", CA_B },
	{ "fc_perspective", CA_B }, { "fc_image_function0", CA_U }, { "fc_image_function1", CA_U },
	{ "fc_coord_function0", CA_U }, { "fc_coord_function1", CA_U }, { "fc_plane_count0", CA_U },
	{ "fc_plane_count1", CA_U }, { "fc_texcoord_count0", CA_U }, { "fc_texcoord_count1", CA_U },
	{ "fc_memoryless_index0", CA_U }, { "fc_memoryless_index1", CA_U }, { "fc_color_inputs0", CA_U },
	{ "fc_color_inputs1", CA_U }, { "fc_color_opcodes0", CA_U4 }, { "fc_color_opcodes1", CA_U4 },
	{ "fc_color_opcodes_ext0", CA_U2 }, { "fc_color_opcodes_ext1", CA_U2 }, { "fc_color_opaque0", CA_B },
	{ "fc_color_opaque1", CA_B }, { "fc_filter0", CA_U }, { "fc_filter1", CA_U },
};

/* `clear_fs` (boot 75: both draws left the target untouched): Core
 * Animation's vertex shader with attachment_clear_frag_lph, a fragment
 * shader the other tests prove, which writes zero. A zero left half clears
 * the vertex shader, blue there means the vertex shader is at fault too.
 *
 * M2c (boot 78: the vertex shader draws, the red layer came out 00000000).
 * Its fragment shader picks color_lin (a 16-bit noperspective varying) by
 * bytes in scratch; b2l_mtl.c now forwards those bytes and widens such
 * varyings to 32-bit. `perspective` sets fc_perspective in both stages (the
 * smooth color_per instead); `tag` is the pipeline label: "b2l:raw-scratch"
 * leaves the scratch as it was, "b2l:narrow" the varyings 16-bit,
 * "b2l:all-targets" the stores to colours without an attachment (controls,
 * logged, not counted). */
static void
ca_uber_draw(id device, id queue, id library, bool fetch, bool clear_fs, bool perspective, const char *tag)
{
	char label[160], why[200], probes[160] = "";
	if (clear_fs) {
		snprintf(label, sizeof(label), "draw: Core Animation's vertex shader and data with attachment_clear");
	} else {
		snprintf(label, sizeof(label), "draw: Core Animation's red layer with its pipeline and data "
		    "(framebuffer fetch %s%s)", fetch ? "on, blend 0x3e" : "off, blend 0", perspective ? ", perspective" : "");
	}
	id values = make("MTLFunctionConstantValues");
	for (unsigned i = 0; i < 60; i++) {
		uint8_t v[4] = { 0, 0, 0, 0 };
		if (!strcmp(ca_constants[i].name, "fc_attachment_count")) {
			v[0] = 1;
		}
		if (fetch && !strcmp(ca_constants[i].name, "fc_framebuffer_fetch")) {
			v[0] = 1;
		}
		if (fetch && !strcmp(ca_constants[i].name, "fc_blend_function")) {
			v[0] = 0x3e;
		}
		if (perspective && !strcmp(ca_constants[i].name, "fc_perspective")) {
			v[0] = 1;
		}
		MSG(void, const void *, unsigned long, id)(values, S("setConstantValue:type:withName:"), v,
		    ca_constants[i].type, nsstr(ca_constants[i].name));
	}
	id vvalues = make("MTLFunctionConstantValues");
	static const char *const vnames[5] = { "fc_generic", "fc_perspective", "fc_texcoord_count0", "fc_texcoord_count1",
		"fc_vertex_layout" };
	static const unsigned char vtypes[5] = { CA_B, CA_B, CA_U, CA_U, CA_U };
	for (unsigned i = 0; i < 5; i++) {
		uint8_t value = perspective && !strcmp(vnames[i], "fc_perspective");
		MSG(void, const void *, unsigned long, id)(vvalues, S("setConstantValue:type:withName:"), &value, vtypes[i],
		    nsstr(vnames[i]));
	}
	id error = nil;
	id v = MSG(id, id, id, id *)(library, S("newFunctionWithName:constantValues:error:"), nsstr("fixed_vert_lph_spc"),
	    vvalues, &error);
	id f = v == nil ? nil : clear_fs ? MSG(id, id)(library, S("newFunctionWithName:"), nsstr("attachment_clear_frag_lph")) :
	    MSG(id, id, id, id *)(library, S("newFunctionWithName:constantValues:error:"), nsstr("fixed_frag_lph_cph"),
	    values, &error);
	if (v == nil || f == nil) {
		verdict(label, false, "functions %s and %s %s", name_of(v), name_of(f), error_text(error, why, sizeof(why)));
		return;
	}
	id d = make("MTLRenderPipelineDescriptor");
	MSG(void, id)(d, S("setVertexFunction:"), v);
	MSG(void, id)(d, S("setFragmentFunction:"), f);
	if (tag != NULL) {
		MSG(void, id)(d, S("setLabel:"), nsstr(tag));
	}
	MSG(void, unsigned long)(at(MSG(id)(d, S("colorAttachments")), 0), S("setPixelFormat:"), 80);
	id vd = MSG(id)(cls("MTLVertexDescriptor"), S("vertexDescriptor"));
	id attributes = MSG(id)(vd, S("attributes"));
	static const unsigned long format[] = { 31, 29, 29, 27 }, offset[] = { 0, 16, 24, 32 };
	for (unsigned i = 0; i < 4; i++) {
		id a = at(attributes, i);
		MSG(void, unsigned long)(a, S("setFormat:"), format[i]);
		MSG(void, unsigned long)(a, S("setOffset:"), offset[i]);
		MSG(void, unsigned long)(a, S("setBufferIndex:"), 1);
	}
	MSG(void, unsigned long)(at(MSG(id)(vd, S("layouts")), 1), S("setStride:"), 48);
	MSG(void, id)(d, S("setVertexDescriptor:"), vd);
	error = nil;
	id p = MSG(id, id, id *)(device, S("newRenderPipelineStateWithDescriptor:error:"), d, &error);
	id td = MSG(id, unsigned long, unsigned long, unsigned long, bool)(cls("MTLTextureDescriptor"),
	    S("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"), 80, SIDE, SIDE, false);
	MSG(void, unsigned long)(td, S("setUsage:"), 5);   /* shader read, render target */
	id target = MSG(id, id)(device, S("newTextureWithDescriptor:"), td);
	if (p == nil || target == nil) {
		verdict(label, false, "pipeline %s, target %s %s", name_of(p), name_of(target),
		    error_text(error, why, sizeof(why)));
		return;
	}
	struct vertex {
		float position[4], st0[2], st1[2];
		uint16_t colour[4];
		uint32_t pad[2];
	} quad[4] = {
		{ { 0, 0, 0, 1 }, { 0, 0 }, { 0, 0 }, { 0x3c00, 0, 0, 0x3c00 }, { 0, 0 } },
		{ { 32, 0, 0, 1 }, { 0, 0 }, { 0, 0 }, { 0x3c00, 0, 0, 0x3c00 }, { 0, 0 } },
		{ { 32, 64, 0, 1 }, { 0, 0 }, { 0, 0 }, { 0x3c00, 0, 0, 0x3c00 }, { 0, 0 } },
		{ { 0, 64, 0, 1 }, { 0, 0 }, { 0, 0 }, { 0x3c00, 0, 0, 0x3c00 }, { 0, 0 } },
	};
	static const float mvp[16] = { 1.0f / 32, 0, 0, 0, 0, -1.0f / 32, 0, 0, 0, 0, 0, 0, -1, 1, 0, 1 };
	static const uint8_t uniforms[256];
	static const uint16_t indices[6] = { 0, 1, 2, 2, 3, 0 };
	id vb = MSG(id, const void *, unsigned long, unsigned long)(device, S("newBufferWithBytes:length:options:"), quad,
	    sizeof(quad), 0);
	id ib = MSG(id, const void *, unsigned long, unsigned long)(device, S("newBufferWithBytes:length:options:"),
	    indices, sizeof(indices), 0);
	uint64_t ms = 0;
	id cb = MSG(id)(queue, S("commandBuffer"));
	id e = begin_pass(cb, target, (clear_color){ 0.25, 0.5, 0.75, 1.0 });
	MSG(void, id)(e, S("setRenderPipelineState:"), p);
	MSG(void, id, unsigned long, unsigned long)(e, S("setVertexBuffer:offset:atIndex:"), vb, 0, 1);
	MSG(void, const void *, unsigned long, unsigned long)(e, S("setVertexBytes:length:atIndex:"), mvp, sizeof(mvp), 2);
	MSG(void, const void *, unsigned long, unsigned long)(e, S("setFragmentBytes:length:atIndex:"), uniforms,
	    sizeof(uniforms), 1);
	MSG(void, unsigned long, unsigned long, unsigned long, id, unsigned long)(e,
	    S("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:"), 3 /* triangles */, 6,
	    0 /* MTLIndexTypeUInt16 */, ib, 0);
	MSG(void)(e, S("endEncoding"));
	unsigned long status = run(cb, why, sizeof(why), &ms);
	static uint8_t back[ROW * SIDE];
	region_t whole = { { 0, 0, 0 }, { SIDE, SIDE, 1 } };
	MSG(void, void *, unsigned long, region_t, unsigned long)(target, S("getBytes:bytesPerRow:fromRegion:mipmapLevel:"),
	    back, ROW, whole, 0);
	static const uint8_t red[4] = { 0, 0, 255, 255 }, zero[4] = { 0, 0, 0, 0 }, blue[4] = { 191, 128, 64, 255 };
	unsigned wrong_left = 0, wrong_right = 0;
	for (unsigned y = 0; y < SIDE; y++) {
		for (unsigned x = 0; x < SIDE; x++) {
			if (x == 31 || x == 32) {
				continue;
			}
			if (x < 31) {
				wrong_left += !near(back + y * ROW + x * 4, clear_fs ? zero : red, 2);
			} else {
				wrong_right += !near(back + y * ROW + x * 4, blue, 2);
			}
		}
	}
	pixel_text(back, ROW, 16, 32, probes, sizeof(probes));
	pixel_text(back, ROW, 48, 32, probes, sizeof(probes));
	pixel_text(back, ROW, 2, 2, probes, sizeof(probes));
	if (tag != NULL) {
		say("control (%s, not counted): %s: status %lu, %u of %u left pixels not %s, %u right not blue;%s", tag,
		    label, status, wrong_left, 31u * SIDE, clear_fs ? "zero" : "red", wrong_right, probes);
		return;
	}
	verdict(label, vb != nil && ib != nil && status == 4 && wrong_left == 0 && wrong_right == 0,
	    "status %lu, %u of %u left pixels not %s, %u of %u right ones not blue, %llu ms;%s %s", status, wrong_left,
	    31u * SIDE, clear_fs ? "zero" : "red", wrong_right, 31u * SIDE, (unsigned long long)ms, probes, why);
}

/*
 * M2c (boot 76): Core Animation's vertex shader with attachment_clear left
 * the target untouched, so nothing it emits is rasterised. Each probe changes
 * one thing from that draw, all with attachment_clear (a zero left half means
 * the triangles were drawn):
 *   PROBE_STD      std_vert0_lph (proven) instead of fixed_vert_lph_spc,
 *                  with Core Animation's buffer layout and data
 *   PROBE_NDC      the vertices already in NDC, an identity matrix
 *   PROBE_BUFFER0  the vertices in buffer 0, not 1
 *   PROBE_FLAT     six vertices and no index buffer
 *   PROBE_PLAIN    fixed_vert_lph_spc without constant values
 */
enum { PROBE_STD = 1, PROBE_NDC = 2, PROBE_BUFFER0 = 4, PROBE_FLAT = 8, PROBE_PLAIN = 16 };

/* `tag` (M2c, boot 77): the pipeline's label, which b2l_mtl.c reads. "b2l:keep"
 * leaves the output fixes out, the control: logged, not counted. */
static void
vs_probe(id device, id queue, id library, unsigned flags, const char *what, const char *tag)
{
	char label[120], why[200], probes[120] = "";
	const bool control = tag != NULL && strstr(tag, "b2l:keep") != NULL;
	snprintf(label, sizeof(label), "draw: Core Animation's vertex data with attachment_clear, %s", what);
	id error = nil, v = nil;
	if (flags & (PROBE_STD | PROBE_PLAIN)) {
		v = MSG(id, id)(library, S("newFunctionWithName:"),
		    nsstr((flags & PROBE_STD) ? "std_vert0_lph" : "fixed_vert_lph_spc"));
	} else {
		id vvalues = make("MTLFunctionConstantValues");
		static const char *const vnames[5] = { "fc_generic", "fc_perspective", "fc_texcoord_count0",
			"fc_texcoord_count1", "fc_vertex_layout" };
		static const unsigned char vtypes[5] = { CA_B, CA_B, CA_U, CA_U, CA_U };
		for (unsigned i = 0; i < 5; i++) {
			uint8_t zero = 0;
			MSG(void, const void *, unsigned long, id)(vvalues, S("setConstantValue:type:withName:"), &zero,
			    vtypes[i], nsstr(vnames[i]));
		}
		v = MSG(id, id, id, id *)(library, S("newFunctionWithName:constantValues:error:"),
		    nsstr("fixed_vert_lph_spc"), vvalues, &error);
	}
	id f = MSG(id, id)(library, S("newFunctionWithName:"), nsstr("attachment_clear_frag_lph"));
	if (v == nil || f == nil) {
		verdict(label, false, "functions %s and %s %s", name_of(v), name_of(f), error_text(error, why, sizeof(why)));
		return;
	}
	const unsigned long buffer = (flags & PROBE_BUFFER0) ? 0 : 1;
	id d = make("MTLRenderPipelineDescriptor");
	MSG(void, id)(d, S("setVertexFunction:"), v);
	MSG(void, id)(d, S("setFragmentFunction:"), f);
	if (tag != NULL) {
		MSG(void, id)(d, S("setLabel:"), nsstr(tag));
	}
	MSG(void, unsigned long)(at(MSG(id)(d, S("colorAttachments")), 0), S("setPixelFormat:"), 80);
	id vd = MSG(id)(cls("MTLVertexDescriptor"), S("vertexDescriptor"));
	id attributes = MSG(id)(vd, S("attributes"));
	/* std_vert0: position (0), texcoord (1), colour (3); fixed_vert: 0 to 3 */
	static const unsigned long std_slot[] = { 0, 1, 3 }, std_format[] = { 31, 29, 27 }, std_offset[] = { 0, 16, 32 };
	static const unsigned long ca_slot[] = { 0, 1, 2, 3 }, ca_format[] = { 31, 29, 29, 27 }, ca_offset[] = { 0, 16, 24, 32 };
	const bool std = (flags & PROBE_STD) != 0;
	for (unsigned i = 0; i < (std ? 3u : 4u); i++) {
		id a = at(attributes, std ? std_slot[i] : ca_slot[i]);
		MSG(void, unsigned long)(a, S("setFormat:"), std ? std_format[i] : ca_format[i]);
		MSG(void, unsigned long)(a, S("setOffset:"), std ? std_offset[i] : ca_offset[i]);
		MSG(void, unsigned long)(a, S("setBufferIndex:"), buffer);
	}
	MSG(void, unsigned long)(at(MSG(id)(vd, S("layouts")), buffer), S("setStride:"), 48);
	MSG(void, id)(d, S("setVertexDescriptor:"), vd);
	error = nil;
	id p = MSG(id, id, id *)(device, S("newRenderPipelineStateWithDescriptor:error:"), d, &error);
	id td = MSG(id, unsigned long, unsigned long, unsigned long, bool)(cls("MTLTextureDescriptor"),
	    S("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"), 80, SIDE, SIDE, false);
	MSG(void, unsigned long)(td, S("setUsage:"), 5);
	id target = MSG(id, id)(device, S("newTextureWithDescriptor:"), td);
	if (p == nil || target == nil) {
		verdict(label, false, "pipeline %s, target %s %s", name_of(p), name_of(target),
		    error_text(error, why, sizeof(why)));
		return;
	}
	struct vertex {
		float position[4], st0[2], st1[2];
		uint16_t colour[4];
		uint32_t pad[2];
	} corners[4], flat[6];
	static const float pixels[4][2] = { { 0, 0 }, { 32, 0 }, { 32, 64 }, { 0, 64 } };
	static const float ndc[4][2] = { { -1, 1 }, { 0, 1 }, { 0, -1 }, { -1, -1 } };
	memset(corners, 0, sizeof(corners));
	for (unsigned i = 0; i < 4; i++) {
		corners[i].position[0] = (flags & PROBE_NDC) ? ndc[i][0] : pixels[i][0];
		corners[i].position[1] = (flags & PROBE_NDC) ? ndc[i][1] : pixels[i][1];
		corners[i].position[3] = 1;
		corners[i].colour[0] = corners[i].colour[3] = 0x3c00;
	}
	static const uint16_t indices[6] = { 0, 1, 2, 2, 3, 0 };
	for (unsigned i = 0; i < 6; i++) {
		flat[i] = corners[indices[i]];
	}
	static const float mvp[16] = { 1.0f / 32, 0, 0, 0, 0, -1.0f / 32, 0, 0, 0, 0, 0, 0, -1, 1, 0, 1 };
	static const uint8_t uniforms[256];
	const bool unindexed = (flags & PROBE_FLAT) != 0;
	id vb = MSG(id, const void *, unsigned long, unsigned long)(device, S("newBufferWithBytes:length:options:"),
	    unindexed ? (const void *)flat : (const void *)corners, unindexed ? sizeof(flat) : sizeof(corners), 0);
	id ib = MSG(id, const void *, unsigned long, unsigned long)(device, S("newBufferWithBytes:length:options:"),
	    indices, sizeof(indices), 0);
	uint64_t ms = 0;
	id cb = MSG(id)(queue, S("commandBuffer"));
	id e = begin_pass(cb, target, (clear_color){ 0.25, 0.5, 0.75, 1.0 });
	MSG(void, id)(e, S("setRenderPipelineState:"), p);
	MSG(void, id, unsigned long, unsigned long)(e, S("setVertexBuffer:offset:atIndex:"), vb, 0, buffer);
	MSG(void, const void *, unsigned long, unsigned long)(e, S("setVertexBytes:length:atIndex:"),
	    (flags & PROBE_NDC) ? identity4 : mvp, sizeof(mvp), 2);
	MSG(void, const void *, unsigned long, unsigned long)(e, S("setFragmentBytes:length:atIndex:"), uniforms,
	    sizeof(uniforms), 1);
	if (unindexed) {
		MSG(void, unsigned long, unsigned long, unsigned long)(e, S("drawPrimitives:vertexStart:vertexCount:"), 3, 0, 6);
	} else {
		MSG(void, unsigned long, unsigned long, unsigned long, id, unsigned long)(e,
		    S("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:"), 3, 6, 0, ib, 0);
	}
	MSG(void)(e, S("endEncoding"));
	unsigned long status = run(cb, why, sizeof(why), &ms);
	static uint8_t back[ROW * SIDE];
	region_t whole = { { 0, 0, 0 }, { SIDE, SIDE, 1 } };
	MSG(void, void *, unsigned long, region_t, unsigned long)(target, S("getBytes:bytesPerRow:fromRegion:mipmapLevel:"),
	    back, ROW, whole, 0);
	static const uint8_t zero[4] = { 0, 0, 0, 0 }, blue[4] = { 191, 128, 64, 255 };
	unsigned wrong_left = 0, wrong_right = 0;
	for (unsigned y = 0; y < SIDE; y++) {
		for (unsigned x = 0; x < SIDE; x++) {
			if (x == 31 || x == 32) {
				continue;
			}
			if (x < 31) {
				wrong_left += !near(back + y * ROW + x * 4, zero, 2);
			} else {
				wrong_right += !near(back + y * ROW + x * 4, blue, 2);
			}
		}
	}
	pixel_text(back, ROW, 16, 32, probes, sizeof(probes));
	pixel_text(back, ROW, 48, 32, probes, sizeof(probes));
	if (control) {
		say("control (%s, not counted): %s: status %lu, %u of %u left pixels not zero, %u right not blue;%s", tag,
		    label, status, wrong_left, 31u * SIDE, wrong_right, probes);
		return;
	}
	verdict(label, vb != nil && ib != nil && status == 4 && wrong_left == 0 && wrong_right == 0,
	    "status %lu, %u of %u left pixels not zero, %u right not blue, %llu ms;%s %s", status, wrong_left, 31u * SIDE,
	    wrong_right, (unsigned long long)ms, probes, why);
}

static id
texture_rgba(id device, unsigned long usage)
{
	id td = MSG(id, unsigned long, unsigned long, unsigned long, bool)(cls("MTLTextureDescriptor"),
	    S("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"), 70 /* RGBA8Unorm */, SIDE, SIDE, false);
	MSG(void, unsigned long)(td, S("setUsage:"), usage);
	return MSG(id, id)(device, S("newTextureWithDescriptor:"), td);
}

/* read_write_surf_compute (QuartzCore): each thread copies the pixel at its
 * grid position from texture(0) to texture(1), bounds checked. */
static void
compute_copy(id device, id queue, id library)
{
	char why[200], probes[120] = "";
	uint64_t ms = 0;
	id f = MSG(id, id)(library, S("newFunctionWithName:"), nsstr("read_write_surf_compute"));
	id error = nil;
	uint64_t t0 = now_ms();
	id p = f != nil ? MSG(id, id, id *)(device, S("newComputePipelineStateWithFunction:error:"), f, &error) : nil;
	unsigned long max_threads = p != nil ? MSG(unsigned long)(p, S("maxTotalThreadsPerThreadgroup")) : 0;
	verdict("compute pipeline read_write_surf_compute", p != nil, "%s in %llu ms, %lu threads at most %s",
	    name_of(p), (unsigned long long)(now_ms() - t0), max_threads, error_text(error, why, sizeof(why)));
	if (p == nil) {
		return;
	}
	id source = texture_rgba(device, 1);           /* ShaderRead */
	id dest = texture_rgba(device, 1 | 2);         /* ShaderRead | ShaderWrite */
	static uint8_t pixels[ROW * SIDE], back[ROW * SIDE];
	for (unsigned i = 0; i < sizeof(pixels); i++) {
		pixels[i] = (uint8_t)(i * 29 + 7);
	}
	region_t whole = { { 0, 0, 0 }, { SIDE, SIDE, 1 } };
	if (source != nil) {
		MSG(void, region_t, unsigned long, const void *, unsigned long)(source,
		    S("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"), whole, 0, pixels, ROW);
	}
	id cb = MSG(id)(queue, S("commandBuffer"));
	id e = MSG(id)(cb, S("computeCommandEncoder"));
	MSG(void, id)(e, S("setComputePipelineState:"), p);
	MSG(void, id, unsigned long)(e, S("setTexture:atIndex:"), source, 0);
	MSG(void, id, unsigned long)(e, S("setTexture:atIndex:"), dest, 1);
	extent_t groups = { SIDE / 8, SIDE / 8, 1 }, threads = { 8, 8, 1 };
	MSG(void, extent_t, extent_t)(e, S("dispatchThreadgroups:threadsPerThreadgroup:"), groups, threads);
	MSG(void)(e, S("endEncoding"));
	unsigned long status = run(cb, why, sizeof(why), &ms);
	memset(back, 0, sizeof(back));
	if (dest != nil) {
		MSG(void, void *, unsigned long, region_t, unsigned long)(dest, S("getBytes:bytesPerRow:fromRegion:mipmapLevel:"),
		    back, ROW, whole, 0);
	}
	unsigned wrong = 0;
	for (unsigned i = 0; i < SIDE * SIDE; i++) {
		wrong += memcmp(back + i * 4, pixels + i * 4, 4) != 0;
	}
	pixel_text(back, ROW, 0, 0, probes, sizeof(probes));
	pixel_text(back, ROW, 63, 63, probes, sizeof(probes));
	verdict("compute: read_write_surf_compute copies a 64x64 texture", source != nil && dest != nil && status == 4 &&
	    wrong == 0, "status %lu, %u of %u pixels wrong, %llu ms;%s %s", status, wrong, (unsigned)(SIDE * SIDE),
	    (unsigned long long)ms, probes, why);
}

static volatile int listener_called;

static void
listener_block_body(id event, uint64_t value)
{
	(void)event;
	listener_called = (int)value;
}

/* A shared event signalled by a command buffer, a listener, a heap. */
static void
events_and_heap(id device, id queue)
{
	char why[200];
	uint64_t ms = 0;
	id ev = MSG(id)(device, S("newSharedEvent"));
	/* Boot 71: make() gave nil here. Say why, then try the other ways in:
	 * +sharedListener, -initWithDispatchQueue:. */
	id listener_class = cls("MTLSharedEventListener");
	id listener = make("MTLSharedEventListener");
	const char *how = "alloc/init";
	if (listener == nil && listener_class != nil &&
	    MSG(bool, SEL)(listener_class, S("respondsToSelector:"), S("sharedListener"))) {
		listener = MSG(id)(listener_class, S("sharedListener"));
		how = "+sharedListener";
	}
	if (listener == nil && listener_class != nil) {
		dispatch_queue_t q = dispatch_queue_create("dev.b2l.mtltest.listener", NULL);
		listener = MSG(id, dispatch_queue_t)(MSG(id)(listener_class, S("alloc")), S("initWithDispatchQueue:"), q);
		how = "-initWithDispatchQueue:";
	}
	say("shared event listener: class %s, %s gave %s", listener_class != nil ? "found" : "MISSING", how,
	    name_of(listener));
	listener_called = 0;
	/* Boot 72: every way in gave nil (Apple's listener needs Apple's device).
	 * Then try ours without one: the plug-in calls the block on its own queue. */
	if (ev != nil) {
		MSG(void, id, uint64_t, void (^)(id, uint64_t))(ev, S("notifyListener:atValue:block:"), listener, 5,
		    ^(id event, uint64_t value) {
			    listener_block_body(event, value);
		    });
	}
	id cb = MSG(id)(queue, S("commandBuffer"));
	MSG(void, id, uint64_t)(cb, S("encodeSignalEvent:value:"), ev, 5);
	unsigned long status = run(cb, why, sizeof(why), &ms);
	for (unsigned i = 0; i < 200 && listener_called == 0; i++) {
		usleep(5000);
	}
	uint64_t value = ev != nil ? MSG(uint64_t)(ev, S("signaledValue")) : 0;
	verdict("shared event signalled by a command buffer, with a listener", ev != nil && status == 4 && value == 5 &&
	    listener_called == 5, "%s value %llu, listener %s called %d", name_of(ev), (unsigned long long)value,
	    name_of(listener), listener_called);

	id hd = make("MTLHeapDescriptor");
	MSG(void, unsigned long)(hd, S("setSize:"), 1u << 20);
	id heap = MSG(id, id)(device, S("newHeapWithDescriptor:"), hd);
	id hb = heap != nil ? MSG(id, unsigned long, unsigned long)(heap, S("newBufferWithLength:options:"), 4096, 0) : nil;
	void *contents = hb != nil ? MSG(void *)(hb, S("contents")) : NULL;
	unsigned long used = heap != nil ? MSG(unsigned long)(heap, S("usedSize")) : 0;
	verdict("heap and a buffer from it", heap != nil && contents != NULL && used >= 4096, "%s, %s, used %lu",
	    name_of(heap), name_of(hb), used);
	id fence = MSG(id)(device, S("newFence"));
	verdict("fence", fence != nil, "%s", name_of(fence));
}

/* A BGRA8 texture's sRGB view rendered to: its parent reads red. */
static void
texture_view_test(id queue, id texture)
{
	char why[200], probes[80] = "";
	uint64_t ms = 0;
	id view = MSG(id, unsigned long)(texture, S("newTextureViewWithPixelFormat:"), 81 /* BGRA8Unorm_sRGB */);
	if (view == nil) {
		verdict("texture view (sRGB) rendered to", false, "no view");
		return;
	}
	id cb = MSG(id)(queue, S("commandBuffer"));
	clear_pass(cb, view, (clear_color){ 1.0, 0.0, 0.0, 1.0 });
	unsigned long status = run(cb, why, sizeof(why), &ms);
	static uint8_t back[ROW * SIDE];
	region_t whole = { { 0, 0, 0 }, { SIDE, SIDE, 1 } };
	MSG(void, void *, unsigned long, region_t, unsigned long)(texture, S("getBytes:bytesPerRow:fromRegion:mipmapLevel:"),
	    back, ROW, whole, 0);
	static const uint8_t red[4] = { 0, 0, 255, 255 };
	uint32_t first = 0;
	unsigned wrong = wrong_pixels(back, ROW, red, &first);
	pixel_text(back, ROW, 10, 10, probes, sizeof(probes));
	verdict("texture view (sRGB) cleared, read through its parent", status == 4 && wrong == 0,
	    "%s, status %lu, %u pixels wrong;%s %s", name_of(MSG(id)(view, S("parentTexture"))), status, wrong, probes,
	    why);
}

typedef struct {
	double x, y, width, height;
} cg_rect;

typedef struct {
	double x, y;
} cg_point;

/*
 * Core Animation's own Metal renderer, in this process only: CARenderer
 * draws a layer tree -- a red layer whose right half is covered by a green
 * one -- into a texture of this device. What it asks of the driver and does
 * not get is logged by B2LMetal.bundle; this checks the two halves' colours.
 * Last of the tests: the step it fails in is logged before it is taken.
 */
static void
carenderer_test(id device, id queue)
{
	void *qc = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW | RTLD_GLOBAL);
	double (*media_time)(void) = qc != NULL ? (double (*)(void))dlsym(qc, "CACurrentMediaTime") : NULL;
	const void **queue_key = qc != NULL ? (const void **)dlsym(qc, "kCARendererMetalCommandQueue") : NULL;
	const void *(*srgb)(double, double, double, double) =
	    (const void *(*)(double, double, double, double))dlsym(RTLD_DEFAULT, "CGColorCreateSRGB");
	if (media_time == NULL || srgb == NULL || cls("CARenderer") == nil) {
		verdict("CARenderer", false, "QuartzCore or CoreGraphics not found");
		return;
	}
	id texture = MSG(id, unsigned long, unsigned long, unsigned long, bool)(cls("MTLTextureDescriptor"),
	    S("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"), 80, SIDE, SIDE, false);
	MSG(void, unsigned long)(texture, S("setUsage:"), 1 | 4);
	id target = MSG(id, id)(device, S("newTextureWithDescriptor:"), texture);
	if (target == nil) {
		verdict("CARenderer", false, "no target texture");
		return;
	}
	say("CARenderer: making the layers");
	id transaction = cls("CATransaction");
	MSG(void)(transaction, S("begin"));
	MSG(void, bool)(transaction, S("setDisableActions:"), true);
	cg_rect bounds = { 0, 0, SIDE, SIDE };
	id root = MSG(id)(cls("CALayer"), S("layer"));
	MSG(void, cg_rect)(root, S("setBounds:"), bounds);
	MSG(void, cg_point)(root, S("setPosition:"), (cg_point){ SIDE / 2, SIDE / 2 });
	MSG(void, const void *)(root, S("setBackgroundColor:"), srgb(1, 0, 0, 1));
	id half = MSG(id)(cls("CALayer"), S("layer"));
	MSG(void, cg_rect)(half, S("setFrame:"), (cg_rect){ SIDE / 2, 0, SIDE / 2, SIDE });
	MSG(void, const void *)(half, S("setBackgroundColor:"), srgb(0, 1, 0, 1));
	MSG(void, id)(root, S("addSublayer:"), half);
	MSG(void)(transaction, S("commit"));
	id options = nil;
	if (queue_key != NULL && *queue_key != NULL) {
		const void *values[1] = { queue };
		options = (id)CFDictionaryCreate(NULL, queue_key, values, 1, &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
	}
	say("CARenderer: rendererWithMTLTexture:options: (%s queue)", options != nil ? "our" : "its own");
	uint64_t t0 = now_ms();
	say("CARenderer: target texture %p", (void *)target);   /* to match the "ca: render pass" lines */
	id renderer = MSG(id, id, id)(cls("CARenderer"), S("rendererWithMTLTexture:options:"), target, options);
	say("CARenderer: %s in %llu ms", name_of(renderer), (unsigned long long)(now_ms() - t0));
	if (renderer == nil) {
		verdict("CARenderer", false, "rendererWithMTLTexture gave nil");
		return;
	}
	/* Boot 70: set after the commit, the renderer had no committed tree and
	 * drew nothing (no command buffer). Attach it inside a transaction and
	 * flush, so the layers reach the renderer's context before the frame. */
	MSG(void)(transaction, S("begin"));
	MSG(void, bool)(transaction, S("setDisableActions:"), true);
	MSG(void, id)(renderer, S("setLayer:"), root);
	MSG(void, cg_rect)(renderer, S("setBounds:"), bounds);
	MSG(void)(transaction, S("commit"));
	MSG(void)(transaction, S("flush"));
	say("CARenderer: beginFrame, render, endFrame");
	/* Labelled empty buffers around the frame: the plug-in logs their
	 * numbers, and the gap is how many buffers Core Animation committed. */
	id marker = MSG(id)(queue, S("commandBuffer"));
	MSG(void, id)(marker, S("setLabel:"), nsstr("test: before CARenderer"));
	MSG(void)(marker, S("commit"));
	MSG(void)(marker, S("waitUntilCompleted"));
	t0 = now_ms();
	MSG(void, double, void *)(renderer, S("beginFrameAtTime:timeStamp:"), media_time(), NULL);
	MSG(void, cg_rect)(renderer, S("addUpdateRect:"), bounds);
	MSG(void)(renderer, S("render"));
	MSG(void)(renderer, S("endFrame"));
	uint64_t render_ms = now_ms() - t0;
	usleep(200000);                 /* a command buffer it committed and did not wait for */
	marker = MSG(id)(queue, S("commandBuffer"));
	MSG(void, id)(marker, S("setLabel:"), nsstr("test: after CARenderer"));
	MSG(void)(marker, S("commit"));
	MSG(void)(marker, S("waitUntilCompleted"));
	static uint8_t back[ROW * SIDE];
	region_t whole = { { 0, 0, 0 }, { SIDE, SIDE, 1 } };
	MSG(void, void *, unsigned long, region_t, unsigned long)(target, S("getBytes:bytesPerRow:fromRegion:mipmapLevel:"),
	    back, ROW, whole, 0);
	char probes[160] = "";
	pixel_text(back, ROW, 16, 32, probes, sizeof(probes));
	pixel_text(back, ROW, 48, 32, probes, sizeof(probes));
	pixel_text(back, ROW, 2, 2, probes, sizeof(probes));
	pixel_text(back, ROW, 61, 61, probes, sizeof(probes));
	const uint8_t *l = back + 32 * ROW + 16 * 4, *r = back + 32 * ROW + 48 * 4;
	bool red_left = l[2] > 200 && l[1] < 60, green_right = r[1] > 200 && r[2] < 60;
	verdict("CARenderer (Core Animation's Metal renderer) draws two layers", red_left && green_right,
	    "render %llu ms;%s", (unsigned long long)render_ms, probes);
}

void
b2l_mtl_test_run(void)
{
	char why[200];
	uint64_t ms = 0;
	void *metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_GLOBAL);
	id (*create)(void) = metal != NULL ? (id (*)(void))dlsym(metal, "MTLCreateSystemDefaultDevice") : NULL;
	if (create == NULL) {
		say("Metal not found: %s", dlerror());
		return;
	}
	uint64_t t0 = now_ms();
	id device = create();
	verdict("MTLCreateSystemDefaultDevice", device != nil, "%s in %llu ms", name_of(device),
	    (unsigned long long)(now_ms() - t0));
	if (device == nil) {
		return;
	}
	id queue = MSG(id)(device, S("newCommandQueue"));
	verdict("supportsMemorylessRenderTargets is NO", !MSG(bool)(device, S("supportsMemorylessRenderTargets")), "");

	/* buffers */
	static uint8_t pattern[4096];
	for (unsigned i = 0; i < sizeof(pattern); i++) {
		pattern[i] = (uint8_t)(i * 7 + 3);
	}
	id a = MSG(id, unsigned long, unsigned long)(device, S("newBufferWithLength:options:"), sizeof(pattern), 0);
	uint8_t *pa = a != nil ? MSG(void *)(a, S("contents")) : NULL;
	bool zero = pa != NULL;
	for (unsigned i = 0; zero && i < sizeof(pattern); i++) {
		zero = pa[i] == 0;
	}
	verdict("newBufferWithLength: 4096, zeroed", zero, "%s", name_of(a));
	if (pa != NULL) {
		memcpy(pa, pattern, sizeof(pattern));
	}
	id b = MSG(id, const void *, unsigned long, unsigned long)(device, S("newBufferWithBytes:length:options:"), pattern,
	    sizeof(pattern), 0);
	uint8_t *pb = b != nil ? MSG(void *)(b, S("contents")) : NULL;
	verdict("newBufferWithBytes:length:options:", pb != NULL && memcmp(pb, pattern, sizeof(pattern)) == 0, "%s",
	    name_of(b));

	/* a blit: a copy, then a fill over part of it */
	id c = MSG(id, unsigned long, unsigned long)(device, S("newBufferWithLength:options:"), sizeof(pattern), 0);
	uint8_t *pc = c != nil ? MSG(void *)(c, S("contents")) : NULL;
	id cb = MSG(id)(queue, S("commandBuffer"));
	id blit = MSG(id)(cb, S("blitCommandEncoder"));
	MSG(void, id, unsigned long, id, unsigned long, unsigned long)(blit,
	    S("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:"), a, 0, c, 0, sizeof(pattern));
	MSG(void, id, CFRange, uint8_t)(blit, S("fillBuffer:range:value:"), c, CFRangeMake(1024, 512), 0x5a);
	MSG(void)(blit, S("endEncoding"));
	unsigned long status = cb != nil ? run(cb, why, sizeof(why), &ms) : 0;
	unsigned bad = pc == NULL ? sizeof(pattern) : 0;
	for (unsigned i = 0; pc != NULL && i < sizeof(pattern); i++) {
		bad += pc[i] != (i >= 1024 && i < 1536 ? 0x5a : pattern[i]);
	}
	verdict("blit: copyFromBuffer and fillBuffer", status == 4 && bad == 0, "%s, status %lu, %u bytes wrong, %llu ms %s",
	    name_of(cb), status, bad, (unsigned long long)ms, why);

	/* a texture written and read back */
	id td = MSG(id, unsigned long, unsigned long, unsigned long, bool)(cls("MTLTextureDescriptor"),
	    S("texture2DDescriptorWithPixelFormat:width:height:mipmapped:"), 80 /* BGRA8Unorm */, SIDE, SIDE, false);
	MSG(void, unsigned long)(td, S("setUsage:"), 1 | 4);       /* ShaderRead | RenderTarget */
	id t = MSG(id, id)(device, S("newTextureWithDescriptor:"), td);
	static uint8_t pixels[ROW * SIDE], back[ROW * SIDE];
	for (unsigned i = 0; i < sizeof(pixels); i++) {
		pixels[i] = (uint8_t)(i * 13 + 1);
	}
	region_t whole = { { 0, 0, 0 }, { SIDE, SIDE, 1 } };
	if (t != nil) {
		MSG(void, region_t, unsigned long, const void *, unsigned long)(t,
		    S("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"), whole, 0, pixels, ROW);
		MSG(void, void *, unsigned long, region_t, unsigned long)(t, S("getBytes:bytesPerRow:fromRegion:mipmapLevel:"),
		    back, ROW, whole, 0);
	}
	verdict("texture 64x64 BGRA8 written and read back", t != nil && memcmp(pixels, back, sizeof(back)) == 0, "%s",
	    name_of(t));

	/* an IOSurface texture */
	IOSurfaceRef surface = make_surface();
	id ts = surface != NULL ? MSG(id, id, IOSurfaceRef, unsigned long)(device,
	    S("newTextureWithDescriptor:iosurface:plane:"), td, surface, 0) : nil;
	verdict("IOSurface texture", ts != nil, "surface %u, %s", surface != NULL ? IOSurfaceGetID(surface) : 0,
	    name_of(ts));

	/* render passes that clear: the IOSurface to (0.25, 0.5, 0.75, 1), the texture to red */
	if (ts != nil && t != nil) {
		cb = MSG(id)(queue, S("commandBuffer"));
		clear_pass(cb, ts, (clear_color){ 0.25, 0.5, 0.75, 1.0 });
		clear_pass(cb, t, (clear_color){ 1.0, 0.0, 0.0, 1.0 });
		status = run(cb, why, sizeof(why), &ms);
		static const uint8_t blue[4] = { 191, 128, 64, 255 }, red[4] = { 0, 0, 255, 255 };
		uint32_t first = 0;
		IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
		unsigned wrong = wrong_pixels(IOSurfaceGetBaseAddress(surface), IOSurfaceGetBytesPerRow(surface), blue, &first);
		IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
		verdict("render pass clear of the IOSurface", status == 4 && wrong == 0,
		    "status %lu, %u of %u pixels wrong (first 0x%08x), %llu ms %s", status, wrong, (unsigned)(SIDE * SIDE), first,
		    (unsigned long long)ms, why);
		MSG(void, void *, unsigned long, region_t, unsigned long)(t, S("getBytes:bytesPerRow:fromRegion:mipmapLevel:"),
		    back, ROW, whole, 0);
		first = 0;
		wrong = wrong_pixels(back, ROW, red, &first);
		verdict("render pass clear of the texture", status == 4 && wrong == 0, "%u pixels wrong (first 0x%08x)", wrong,
		    first);

		/* a texture copy: the IOSurface's pixels into the texture */
		cb = MSG(id)(queue, S("commandBuffer"));
		blit = MSG(id)(cb, S("blitCommandEncoder"));
		MSG(void, id, id)(blit, S("copyFromTexture:toTexture:"), ts, t);
		MSG(void)(blit, S("endEncoding"));
		status = run(cb, why, sizeof(why), &ms);
		MSG(void, void *, unsigned long, region_t, unsigned long)(t, S("getBytes:bytesPerRow:fromRegion:mipmapLevel:"),
		    back, ROW, whole, 0);
		first = 0;
		wrong = wrong_pixels(back, ROW, blue, &first);
		verdict("blit: copyFromTexture:toTexture:", status == 4 && wrong == 0,
		    "status %lu, %u pixels wrong (first 0x%08x), %llu ms %s", status, wrong, first, (unsigned long long)ms, why);
	}

	/* a sampler */
	id sd = make("MTLSamplerDescriptor");
	MSG(void, unsigned long)(sd, S("setMinFilter:"), 1);
	MSG(void, unsigned long)(sd, S("setMagFilter:"), 1);
	id sampler = MSG(id, id)(device, S("newSamplerStateWithDescriptor:"), sd);
	verdict("sampler state (linear, clamp to edge)", sampler != nil, "%s", name_of(sampler));

	/* QuartzCore's library and two pipelines */
	id error = nil;
	id url = MSG(id, id)(cls("NSURL"), S("fileURLWithPath:"), nsstr(QC_LIBRARY));
	t0 = now_ms();
	id library = MSG(id, id, id *)(device, S("newLibraryWithURL:error:"), url, &error);
	unsigned long functions = library != nil ? MSG(unsigned long)(MSG(id)(library, S("functionNames")), S("count")) : 0;
	verdict("QuartzCore's default.metallib", library != nil && functions > 100, "%s, %lu functions in %llu ms %s",
	    name_of(library), functions, (unsigned long long)(now_ms() - t0), error_text(error, why, sizeof(why)));
	if (library != nil) {
		id solid = pipeline_test(device, library, "std_vert0_lph", "attachment_clear_frag_lph", true, false);
		pipeline_test(device, library, "fixed_vert_lph_spc", "fixed_frag_lph_cph", false, true);
		id image = pipeline_test(device, library, "image_rect_blit_vert_lph", "image_rect_blit_frag_lph", false,
		    false);
		/* M2b: the draws */
		if (solid != nil && ts != nil) {
			draw_triangle(device, queue, solid, ts, surface);
		}
		if (image != nil && t != nil) {
			draw_quad(device, queue, image, t);
		}
		if (solid != nil && ts != nil) {
			solid_draw(device, queue, solid, ts, surface, "draw: indexed triangle", SHAPE_TRIANGLE, 0, true, false,
			    false);
			solid_draw(device, queue, solid, ts, surface, "draw: two triangles cover the target", SHAPE_ALL, 0, false,
			    true, false);
			solid_draw(device, queue, solid, ts, surface, "draw: culling back faces keeps the clockwise triangle",
			    SHAPE_TRIANGLE, 2 /* MTLCullModeBack */, false, false, false);
			solid_draw(device, queue, solid, ts, surface, "draw: culling front faces drops it", SHAPE_NONE,
			    1 /* MTLCullModeFront */, false, false, false);
			solid_draw(device, queue, solid, ts, surface, "draw: scissor to the left half", SHAPE_LEFT_HALF, 0, false,
			    true, true);
			id blended = blend_pipeline(device, library);
			if (blended != nil) {
				solid_draw(device, queue, blended, ts, surface, "draw: blending a zero alpha keeps the target",
				    SHAPE_NONE, 0, false, true, false);
			}
		}
		compute_copy(device, queue, library);
		/* M2c: Core Animation's draw by hand, with and without its blending */
		ca_uber_draw(device, queue, library, true, false, false, NULL);
		ca_uber_draw(device, queue, library, false, false, false, NULL);
		ca_uber_draw(device, queue, library, false, true, false, NULL);
		/* M2c (boot 78): the red layer was 00000000. The smooth colour
		 * (fc_perspective), then the controls without the scratch
		 * forwarding and without the 32-bit varyings. */
		ca_uber_draw(device, queue, library, false, false, true, NULL);
		ca_uber_draw(device, queue, library, false, false, false, "b2l:raw-scratch");
		ca_uber_draw(device, queue, library, false, false, false, "b2l:narrow");
		/* M2c (boot 79): still 00000000 with the shader down to color0 =
		 * color_lin and NaN to colours 1 and 2, which have no attachment;
		 * b2l_mtl.c now drops those stores. The control keeps them. */
		ca_uber_draw(device, queue, library, false, false, false, "b2l:all-targets");
		/* M2c (boot 76): one change at a time from that vertex-shader draw;
		 * boot 77: only std_vert0 drew. With b2l_mtl.c's output fixes now
		 * on, the same probes, then the control without them (b2l:keep)
		 * and every point-size output removed (b2l:no-psiz). */
		vs_probe(device, queue, library, 0, "its vertex shader, output fixes on", NULL);
		vs_probe(device, queue, library, 0, "its vertex shader, output fixes off", "b2l:keep");
		vs_probe(device, queue, library, 0, "its vertex shader, no point size at all", "b2l:no-psiz");
		vs_probe(device, queue, library, PROBE_STD, "std_vert0 (proven) as its vertex shader", NULL);
		vs_probe(device, queue, library, PROBE_NDC, "vertices in NDC, identity matrix", NULL);
		vs_probe(device, queue, library, PROBE_BUFFER0, "vertices in buffer 0", NULL);
		vs_probe(device, queue, library, PROBE_FLAT, "six vertices, no index buffer", NULL);
		vs_probe(device, queue, library, PROBE_PLAIN, "no constant values", NULL);
		vs_probe(device, queue, library, PROBE_NDC | PROBE_BUFFER0 | PROBE_FLAT, "NDC, buffer 0 and no indices", NULL);
	}
	events_and_heap(device, queue);
	if (t != nil) {
		texture_view_test(queue, t);
	}
	/* last: Core Animation's own Metal renderer */
	carenderer_test(device, queue);
	say("done: %u passed, %u failed", passed, failed);
}
