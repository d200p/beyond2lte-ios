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
 * a 2 x 2 texture and a sampler), both read back pixel by pixel. One kernel
 * log line per step ("B2L GPU UI: mtl M2: test: ..."), with ok or FAILED,
 * through a B2LMali client of its own that only logs.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
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
	}
	say("done: %u passed, %u failed", passed, failed);
}
