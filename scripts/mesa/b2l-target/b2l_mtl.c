// SPDX-License-Identifier: MIT
/*
 * S10 port: Metal milestone M2a (docs/metal-on-panfrost.md), the Mesa half of
 * the Metal driver: what B2LMetal.bundle (userspace/b2l-metal.c) asks for,
 * made of Gallium objects on Panfrost (b2l_mtl.h is the interface).
 *
 *   device      the process's Panfrost screen (b2l_fscreen, shared with the
 *               EAGL layer) and a pipe_context of its own, used under its lock
 *   buffer      a pipe buffer mapped once, persistently, for -contents
 *   texture     a pipe texture, or an IOSurface imported as the EAGL layer
 *               imports one (resource_from_handle on a B2LMali surface fd)
 *   sampler, depth-stencil   their CSOs
 *   library     a .metallib read by HakiOS's AIR frontend (src/compiler/air)
 *   function    one of its entries and the constants given to it, translated
 *               to NIR when a pipeline first needs it
 *   pipeline    both functions, fragment inputs linked to vertex outputs by
 *               the name Metal links them by, the half-pixel position, driver
 *               locations, then Panfrost's own shader CSOs (which compile the
 *               default variant at once), the blend and vertex element CSOs
 *   command buffer   a list of commands executed in order by cmdbuf_execute:
 *               render passes (framebuffer, load actions as clears), buffer and
 *               texture copies and fills. M2a draws nothing yet.
 *
 * B2LMali runs each job to its end before the submission returns (b2l_drm.c),
 * so a flush with a fence is the GPU's whole work for a command buffer.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "frontend/api.h"
#include "frontend/winsys_handle.h"
#include "drm-uapi/drm_fourcc.h"
#include "util/format/u_format.h"
#include "util/ralloc.h"
#include "util/u_atomic.h"
#include "util/box.h"
#include "util/u_dynarray.h"
#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/os_time.h"
#include "util/bitscan.h"
#include "util/half_float.h"
#include "util/u_sampler.h"
#include "util/u_upload_mgr.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "nir_air.h"
#include "b2l_drm.h"
#include "b2l_mtl.h"

#define B2L_EXPORT __attribute__((visibility("default")))

B2L_EXPORT const struct b2l_mtl_api *b2l_mtl_api(uint32_t version);

/* b2l_mesa.c: the process's Panfrost screen. */
struct pipe_frontend_screen *b2l_fscreen(void);

#define MTL_AIR64_CPU 0x01000017u    /* the AIR64 slice of a fat metallib */

static void
say(char *why, size_t why_size, const char *format, ...)
{
	if (why == NULL || why_size == 0) {
		return;
	}
	va_list ap;
	va_start(ap, format);
	vsnprintf(why, why_size, format, ap);
	va_end(ap);
}

static uint64_t
now_us(void)
{
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom / 1000;
}

/* ---- reference counting ---- */

struct obj {
	int32_t refs;
	void (*destroy)(void *self);
};

static void
obj_init(struct obj *o, void (*destroy)(void *))
{
	o->refs = 1;
	o->destroy = destroy;
}

static void *
obj_ref(void *p)
{
	if (p != NULL) {
		p_atomic_inc(&((struct obj *)p)->refs);
	}
	return p;
}

static void
obj_unref(void *p)
{
	struct obj *o = p;
	if (o != NULL && p_atomic_dec_zero(&o->refs)) {
		o->destroy(o);
	}
}

/* ---- the device ---- */

struct b2l_mtl_device {
	pthread_mutex_t lock;
	struct pipe_screen *screen;
	struct pipe_context *pipe;
	const nir_shader_compiler_options *nir_options;
	bool pending;                   /* GPU work since the last settle() */
	/* M2b: what a draw binds where the encoder bound nothing, and the
	 * states every draw needs one of. */
	struct pipe_resource *zero_buffer;              /* 4 KiB of zeros */
	struct pipe_resource *zero_texture;             /* 1 x 1, RGBA 0 */
	struct pipe_sampler_view *zero_view;
	void *default_sampler, *default_dsa, *default_blend, *default_velems;
	void *rasterizers[256];                         /* by raster_key() */
};

static struct b2l_mtl_device *the_device;
static pthread_mutex_t open_lock = PTHREAD_MUTEX_INITIALIZER;

#define ZERO_BYTES 4096

/* The device's stand-ins and default states (M2b), made with its context. */
static bool
device_defaults(struct b2l_mtl_device *dev)
{
	static const uint8_t zeros[ZERO_BYTES];
	struct pipe_screen *screen = dev->screen;
	struct pipe_context *pipe = dev->pipe;
	dev->zero_buffer = pipe_buffer_create(screen, PIPE_BIND_VERTEX_BUFFER | PIPE_BIND_CONSTANT_BUFFER |
	    PIPE_BIND_SHADER_BUFFER, PIPE_USAGE_DEFAULT, ZERO_BYTES);
	if (dev->zero_buffer != NULL) {
		pipe->buffer_subdata(pipe, dev->zero_buffer, PIPE_MAP_WRITE, 0, ZERO_BYTES, zeros);
	}
	struct pipe_resource t;
	memset(&t, 0, sizeof(t));
	t.target = PIPE_TEXTURE_2D;
	t.format = PIPE_FORMAT_R8G8B8A8_UNORM;
	t.width0 = t.height0 = t.depth0 = t.array_size = 1;
	t.bind = PIPE_BIND_SAMPLER_VIEW;
	t.usage = PIPE_USAGE_DEFAULT;
	dev->zero_texture = screen->resource_create(screen, &t);
	if (dev->zero_texture != NULL) {
		struct pipe_box box;
		u_box_2d(0, 0, 1, 1, &box);
		pipe->texture_subdata(pipe, dev->zero_texture, 0, PIPE_MAP_WRITE, &box, zeros, 4, 4);
		struct pipe_sampler_view templ;
		u_sampler_view_default_template(&templ, dev->zero_texture, t.format);
		dev->zero_view = pipe->create_sampler_view(pipe, dev->zero_texture, &templ);
	}
	struct pipe_sampler_state s;
	memset(&s, 0, sizeof(s));
	s.wrap_s = s.wrap_t = s.wrap_r = PIPE_TEX_WRAP_CLAMP_TO_EDGE;
	s.min_img_filter = s.mag_img_filter = PIPE_TEX_FILTER_NEAREST;
	s.min_mip_filter = PIPE_TEX_MIPFILTER_NONE;
	s.seamless_cube_map = 1;
	dev->default_sampler = pipe->create_sampler_state(pipe, &s);
	struct pipe_depth_stencil_alpha_state dsa;
	memset(&dsa, 0, sizeof(dsa));
	dev->default_dsa = pipe->create_depth_stencil_alpha_state(pipe, &dsa);
	struct pipe_blend_state blend;
	memset(&blend, 0, sizeof(blend));
	blend.rt[0].colormask = PIPE_MASK_RGBA;
	dev->default_blend = pipe->create_blend_state(pipe, &blend);
	/* for a vertex function with no attributes: one zero read from buffer 0 */
	struct pipe_vertex_element e;
	memset(&e, 0, sizeof(e));
	e.src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
	dev->default_velems = pipe->create_vertex_elements_state(pipe, 1, &e);
	pipe->set_sample_mask(pipe, ~0u);
	return dev->zero_buffer != NULL && dev->zero_view != NULL && dev->default_sampler != NULL &&
	    dev->default_dsa != NULL && dev->default_blend != NULL && dev->default_velems != NULL;
}

static struct b2l_mtl_device *
device_open(char *why, size_t why_size)
{
	pthread_mutex_lock(&open_lock);
	if (the_device != NULL) {
		pthread_mutex_unlock(&open_lock);
		return the_device;
	}
	struct pipe_frontend_screen *fscreen = b2l_fscreen();
	struct pipe_screen *screen = fscreen != NULL ? fscreen->screen : NULL;
	if (screen == NULL) {
		say(why, why_size, "no Panfrost screen (B2LMali libdrm not opened)");
		pthread_mutex_unlock(&open_lock);
		return NULL;
	}
	struct pipe_context *pipe = screen->context_create(screen, NULL, 0);
	if (pipe == NULL) {
		say(why, why_size, "Panfrost gave no context");
		pthread_mutex_unlock(&open_lock);
		return NULL;
	}
	struct b2l_mtl_device *dev = calloc(1, sizeof(*dev));
	if (dev == NULL) {
		pipe->destroy(pipe);
		say(why, why_size, "out of memory");
		pthread_mutex_unlock(&open_lock);
		return NULL;
	}
	pthread_mutex_init(&dev->lock, NULL);
	dev->screen = screen;
	dev->pipe = pipe;
	dev->nir_options = screen->get_compiler_options(screen, PIPE_SHADER_IR_NIR, PIPE_SHADER_FRAGMENT);
	if (!device_defaults(dev)) {
		/* the device stays: what fails is a draw, which says so */
		b2l_drm_note("mtl M2: the draw defaults were not all made");
	}
	the_device = dev;
	pthread_mutex_unlock(&open_lock);
	b2l_drm_note("mtl M2: device on %s, a context of its own", screen->get_name(screen));
	return dev;
}

static uint32_t
device_max_texture_2d(struct b2l_mtl_device *dev)
{
	(void)dev;
	return 8192;
}

/* The GPU's work so far finished, before the CPU reads what it wrote. */
static void
settle(struct b2l_mtl_device *dev)
{
	struct pipe_fence_handle *fence = NULL;
	dev->pipe->flush(dev->pipe, &fence, 0);
	if (fence != NULL) {
		dev->screen->fence_finish(dev->screen, NULL, fence, OS_TIMEOUT_INFINITE);
		dev->screen->fence_reference(dev->screen, &fence, NULL);
	}
	dev->pending = false;
}

/* ---- formats ---- */

static enum pipe_format
pixel_format(uint32_t mtl)
{
	switch (mtl) {
	case 1: return PIPE_FORMAT_A8_UNORM;
	case 10: return PIPE_FORMAT_R8_UNORM;
	case 11: return PIPE_FORMAT_R8_SRGB;
	case 12: return PIPE_FORMAT_R8_SNORM;
	case 13: return PIPE_FORMAT_R8_UINT;
	case 14: return PIPE_FORMAT_R8_SINT;
	case 20: return PIPE_FORMAT_R16_UNORM;
	case 22: return PIPE_FORMAT_R16_SNORM;
	case 23: return PIPE_FORMAT_R16_UINT;
	case 24: return PIPE_FORMAT_R16_SINT;
	case 25: return PIPE_FORMAT_R16_FLOAT;
	case 30: return PIPE_FORMAT_R8G8_UNORM;
	case 31: return PIPE_FORMAT_R8G8_SRGB;
	case 32: return PIPE_FORMAT_R8G8_SNORM;
	case 33: return PIPE_FORMAT_R8G8_UINT;
	case 34: return PIPE_FORMAT_R8G8_SINT;
	case 40: return PIPE_FORMAT_B5G6R5_UNORM;
	case 53: return PIPE_FORMAT_R32_UINT;
	case 54: return PIPE_FORMAT_R32_SINT;
	case 55: return PIPE_FORMAT_R32_FLOAT;
	case 60: return PIPE_FORMAT_R16G16_UNORM;
	case 62: return PIPE_FORMAT_R16G16_SNORM;
	case 63: return PIPE_FORMAT_R16G16_UINT;
	case 64: return PIPE_FORMAT_R16G16_SINT;
	case 65: return PIPE_FORMAT_R16G16_FLOAT;
	case 70: return PIPE_FORMAT_R8G8B8A8_UNORM;
	case 71: return PIPE_FORMAT_R8G8B8A8_SRGB;
	case 72: return PIPE_FORMAT_R8G8B8A8_SNORM;
	case 73: return PIPE_FORMAT_R8G8B8A8_UINT;
	case 74: return PIPE_FORMAT_R8G8B8A8_SINT;
	case 80: return PIPE_FORMAT_B8G8R8A8_UNORM;
	case 81: return PIPE_FORMAT_B8G8R8A8_SRGB;
	case 90: return PIPE_FORMAT_R10G10B10A2_UNORM;
	case 91: return PIPE_FORMAT_R10G10B10A2_UINT;
	case 92: return PIPE_FORMAT_R11G11B10_FLOAT;
	case 93: return PIPE_FORMAT_R9G9B9E5_FLOAT;
	case 94: return PIPE_FORMAT_B10G10R10A2_UNORM;
	case 103: return PIPE_FORMAT_R32G32_UINT;
	case 104: return PIPE_FORMAT_R32G32_SINT;
	case 105: return PIPE_FORMAT_R32G32_FLOAT;
	case 110: return PIPE_FORMAT_R16G16B16A16_UNORM;
	case 112: return PIPE_FORMAT_R16G16B16A16_SNORM;
	case 113: return PIPE_FORMAT_R16G16B16A16_UINT;
	case 114: return PIPE_FORMAT_R16G16B16A16_SINT;
	case 115: return PIPE_FORMAT_R16G16B16A16_FLOAT;
	case 123: return PIPE_FORMAT_R32G32B32A32_UINT;
	case 124: return PIPE_FORMAT_R32G32B32A32_SINT;
	case 125: return PIPE_FORMAT_R32G32B32A32_FLOAT;
	case 250: return PIPE_FORMAT_Z16_UNORM;
	case 252: return PIPE_FORMAT_Z32_FLOAT;
	case 253: return PIPE_FORMAT_S8_UINT;
	case 255: return PIPE_FORMAT_Z24_UNORM_S8_UINT;
	case 260: return PIPE_FORMAT_Z32_FLOAT_S8X24_UINT;
	default: return PIPE_FORMAT_NONE;   /* the XR formats, packed 16-bit ones, compressed */
	}
}

static enum pipe_format
vertex_format(uint32_t mtl)
{
	switch (mtl) {
	case 1: return PIPE_FORMAT_R8G8_UINT;
	case 2: return PIPE_FORMAT_R8G8B8_UINT;
	case 3: return PIPE_FORMAT_R8G8B8A8_UINT;
	case 4: return PIPE_FORMAT_R8G8_SINT;
	case 5: return PIPE_FORMAT_R8G8B8_SINT;
	case 6: return PIPE_FORMAT_R8G8B8A8_SINT;
	case 7: return PIPE_FORMAT_R8G8_UNORM;
	case 8: return PIPE_FORMAT_R8G8B8_UNORM;
	case 9: return PIPE_FORMAT_R8G8B8A8_UNORM;
	case 10: return PIPE_FORMAT_R8G8_SNORM;
	case 11: return PIPE_FORMAT_R8G8B8_SNORM;
	case 12: return PIPE_FORMAT_R8G8B8A8_SNORM;
	case 13: return PIPE_FORMAT_R16G16_UINT;
	case 14: return PIPE_FORMAT_R16G16B16_UINT;
	case 15: return PIPE_FORMAT_R16G16B16A16_UINT;
	case 16: return PIPE_FORMAT_R16G16_SINT;
	case 17: return PIPE_FORMAT_R16G16B16_SINT;
	case 18: return PIPE_FORMAT_R16G16B16A16_SINT;
	case 19: return PIPE_FORMAT_R16G16_UNORM;
	case 20: return PIPE_FORMAT_R16G16B16_UNORM;
	case 21: return PIPE_FORMAT_R16G16B16A16_UNORM;
	case 22: return PIPE_FORMAT_R16G16_SNORM;
	case 23: return PIPE_FORMAT_R16G16B16_SNORM;
	case 24: return PIPE_FORMAT_R16G16B16A16_SNORM;
	case 25: return PIPE_FORMAT_R16G16_FLOAT;
	case 26: return PIPE_FORMAT_R16G16B16_FLOAT;
	case 27: return PIPE_FORMAT_R16G16B16A16_FLOAT;
	case 28: return PIPE_FORMAT_R32_FLOAT;
	case 29: return PIPE_FORMAT_R32G32_FLOAT;
	case 30: return PIPE_FORMAT_R32G32B32_FLOAT;
	case 31: return PIPE_FORMAT_R32G32B32A32_FLOAT;
	case 32: return PIPE_FORMAT_R32_SINT;
	case 33: return PIPE_FORMAT_R32G32_SINT;
	case 34: return PIPE_FORMAT_R32G32B32_SINT;
	case 35: return PIPE_FORMAT_R32G32B32A32_SINT;
	case 36: return PIPE_FORMAT_R32_UINT;
	case 37: return PIPE_FORMAT_R32G32_UINT;
	case 38: return PIPE_FORMAT_R32G32B32_UINT;
	case 39: return PIPE_FORMAT_R32G32B32A32_UINT;
	case 40: return PIPE_FORMAT_R10G10B10A2_SNORM;
	case 41: return PIPE_FORMAT_R10G10B10A2_UNORM;
	case 42: return PIPE_FORMAT_B8G8R8A8_UNORM;
	case 45: return PIPE_FORMAT_R8_UINT;
	case 46: return PIPE_FORMAT_R8_SINT;
	case 47: return PIPE_FORMAT_R8_UNORM;
	case 48: return PIPE_FORMAT_R8_SNORM;
	case 49: return PIPE_FORMAT_R16_UINT;
	case 50: return PIPE_FORMAT_R16_SINT;
	case 51: return PIPE_FORMAT_R16_UNORM;
	case 52: return PIPE_FORMAT_R16_SNORM;
	case 53: return PIPE_FORMAT_R16_FLOAT;
	case 54: return PIPE_FORMAT_R11G11B10_FLOAT;
	case 55: return PIPE_FORMAT_R9G9B9E5_FLOAT;
	default: return PIPE_FORMAT_NONE;
	}
}

/* ---- buffers ---- */

struct b2l_mtl_buffer {
	struct obj base;
	struct b2l_mtl_device *dev;
	struct pipe_resource *res;
	struct pipe_transfer *transfer;
	uint8_t *map;
	uint64_t length;
};

static void
buffer_destroy(void *self)
{
	struct b2l_mtl_buffer *b = self;
	pthread_mutex_lock(&b->dev->lock);
	if (b->transfer != NULL) {
		pipe_buffer_unmap(b->dev->pipe, b->transfer);
	}
	pipe_resource_reference(&b->res, NULL);
	pthread_mutex_unlock(&b->dev->lock);
	free(b);
}

static struct b2l_mtl_buffer *
buffer_new(struct b2l_mtl_device *dev, uint64_t length, const void *bytes, char *why, size_t why_size)
{
	if (length == 0 || length > (256ull << 20)) {
		say(why, why_size, "length %llu (1 byte to 256 MB)", (unsigned long long)length);
		return NULL;
	}
	struct b2l_mtl_buffer *b = calloc(1, sizeof(*b));
	if (b == NULL) {
		say(why, why_size, "out of memory");
		return NULL;
	}
	obj_init(&b->base, buffer_destroy);
	b->dev = dev;
	b->length = length;
	unsigned size = (unsigned)align64(length, 16);
	struct pipe_resource t;
	memset(&t, 0, sizeof(t));
	t.target = PIPE_BUFFER;
	t.format = PIPE_FORMAT_R8_UNORM;
	t.width0 = size;
	t.height0 = 1;
	t.depth0 = 1;
	t.array_size = 1;
	/* Not PIPE_BIND_INDEX_BUFFER: Panfrost keeps a min/max cache for those
	 * that writes through -contents would leave stale (M2b decides). */
	t.bind = PIPE_BIND_VERTEX_BUFFER | PIPE_BIND_CONSTANT_BUFFER | PIPE_BIND_SHADER_BUFFER;
	t.usage = PIPE_USAGE_DEFAULT;
	t.flags = PIPE_RESOURCE_FLAG_MAP_PERSISTENT | PIPE_RESOURCE_FLAG_MAP_COHERENT;
	pthread_mutex_lock(&dev->lock);
	b->res = dev->screen->resource_create(dev->screen, &t);
	if (b->res != NULL) {
		b->map = pipe_buffer_map_range(dev->pipe, b->res, 0, size,
		    PIPE_MAP_READ | PIPE_MAP_WRITE | PIPE_MAP_PERSISTENT | PIPE_MAP_COHERENT | PIPE_MAP_UNSYNCHRONIZED,
		    &b->transfer);
	}
	pthread_mutex_unlock(&dev->lock);
	if (b->res == NULL || b->map == NULL) {
		say(why, why_size, "%s for %u bytes", b->res == NULL ? "no resource" : "no mapping", size);
		obj_unref(b);
		return NULL;
	}
	if (bytes != NULL) {
		memcpy(b->map, bytes, (size_t)length);
		if (size > length) {
			memset(b->map + length, 0, size - (size_t)length);
		}
	} else {
		memset(b->map, 0, size);
	}
	return b;
}

static void *
buffer_contents(struct b2l_mtl_buffer *b)
{
	return b != NULL ? b->map : NULL;
}

static uint64_t
buffer_length(struct b2l_mtl_buffer *b)
{
	return b != NULL ? b->length : 0;
}

static void
buffer_release(struct b2l_mtl_buffer *b)
{
	obj_unref(b);
}

/* ---- textures ---- */

struct b2l_mtl_texture {
	struct obj base;
	struct b2l_mtl_device *dev;
	struct pipe_resource *res;
	enum pipe_format format;
	struct b2l_mtl_texture_desc desc;
	IOSurfaceRef surface;           /* retained, for an imported surface */
	struct pipe_sampler_view *view; /* made when a draw first samples it */
};

static void
texture_destroy(void *self)
{
	struct b2l_mtl_texture *t = self;
	pthread_mutex_lock(&t->dev->lock);
	pipe_sampler_view_reference(&t->view, NULL);
	pipe_resource_reference(&t->res, NULL);
	pthread_mutex_unlock(&t->dev->lock);
	if (t->surface != NULL) {
		CFRelease(t->surface);
	}
	free(t);
}

static bool
texture_template(const struct b2l_mtl_texture_desc *d, struct pipe_resource *t, char *why, size_t why_size)
{
	memset(t, 0, sizeof(*t));
	t->format = pixel_format(d->pixel_format);
	if (t->format == PIPE_FORMAT_NONE) {
		say(why, why_size, "pixel format %u not handled", d->pixel_format);
		return false;
	}
	unsigned layers = MAX2(d->array_length, 1);
	t->width0 = MAX2(d->width, 1);
	t->height0 = MAX2(d->height, 1);
	t->depth0 = 1;
	t->array_size = 1;
	switch (d->type) {
	case 0: t->target = PIPE_TEXTURE_1D; t->height0 = 1; break;
	case 1: t->target = PIPE_TEXTURE_1D_ARRAY; t->height0 = 1; t->array_size = layers; break;
	case 2: case 4: t->target = PIPE_TEXTURE_2D; break;
	case 3: case 8: t->target = PIPE_TEXTURE_2D_ARRAY; t->array_size = layers; break;
	case 5: t->target = PIPE_TEXTURE_CUBE; t->array_size = 6; break;
	case 6: t->target = PIPE_TEXTURE_CUBE_ARRAY; t->array_size = 6 * layers; break;
	case 7: t->target = PIPE_TEXTURE_3D; t->depth0 = MAX2(d->depth, 1); break;
	default:
		say(why, why_size, "texture type %u not handled", d->type);
		return false;
	}
	t->last_level = d->mip_levels > 1 ? d->mip_levels - 1 : 0;
	t->nr_samples = d->sample_count > 1 ? d->sample_count : 0;
	t->nr_storage_samples = t->nr_samples;
	t->usage = PIPE_USAGE_DEFAULT;
	bool zs = util_format_is_depth_or_stencil(t->format);
	t->bind = PIPE_BIND_SAMPLER_VIEW;
	/* MTLTextureUsageUnknown (0) allows everything */
	if (d->usage == 0 || (d->usage & 4)) {
		t->bind |= zs ? PIPE_BIND_DEPTH_STENCIL : PIPE_BIND_RENDER_TARGET;
	}
	if ((d->usage == 0 || (d->usage & 2)) && !zs) {
		t->bind |= PIPE_BIND_SHADER_IMAGE;
	}
	return true;
}

static struct b2l_mtl_texture *
texture_new(struct b2l_mtl_device *dev, const struct b2l_mtl_texture_desc *desc, char *why, size_t why_size)
{
	struct pipe_resource templat;
	if (!texture_template(desc, &templat, why, why_size)) {
		return NULL;
	}
	struct b2l_mtl_texture *t = calloc(1, sizeof(*t));
	if (t == NULL) {
		say(why, why_size, "out of memory");
		return NULL;
	}
	obj_init(&t->base, texture_destroy);
	t->dev = dev;
	t->format = templat.format;
	t->desc = *desc;
	pthread_mutex_lock(&dev->lock);
	/* A usage the Mali cannot give (an image of this format, say) is dropped
	 * rather than refusing the texture: CA asks for ShaderWrite broadly. */
	unsigned samples = MAX2(templat.nr_samples, 1);
	if ((templat.bind & PIPE_BIND_SHADER_IMAGE) &&
	    !dev->screen->is_format_supported(dev->screen, templat.format, templat.target, samples, samples,
	    templat.bind)) {
		templat.bind &= ~PIPE_BIND_SHADER_IMAGE;
	}
	if (!dev->screen->is_format_supported(dev->screen, templat.format, templat.target, samples, samples,
	    templat.bind)) {
		pthread_mutex_unlock(&dev->lock);
		say(why, why_size, "%s %s, %u samples, bind 0x%x not supported by Panfrost",
		    util_format_short_name(templat.format), desc->type == 7 ? "3D" : "2D", samples, templat.bind);
		free(t);
		return NULL;
	}
	t->res = dev->screen->resource_create(dev->screen, &templat);
	pthread_mutex_unlock(&dev->lock);
	if (t->res == NULL) {
		say(why, why_size, "resource_create refused %s %ux%ux%u, %u levels, %u layers",
		    util_format_short_name(templat.format), templat.width0, templat.height0, templat.depth0,
		    templat.last_level + 1, templat.array_size);
		free(t);
		return NULL;
	}
	return t;
}

static struct b2l_mtl_texture *
texture_from_iosurface(struct b2l_mtl_device *dev, const struct b2l_mtl_texture_desc *desc, void *iosurface,
    uint32_t plane, char *why, size_t why_size)
{
	IOSurfaceRef surface = iosurface;
	struct pipe_resource templat;
	if (surface == NULL || !texture_template(desc, &templat, why, why_size)) {
		if (surface == NULL) {
			say(why, why_size, "no IOSurface");
		}
		return NULL;
	}
	if (templat.target != PIPE_TEXTURE_2D || templat.last_level != 0 || templat.nr_samples > 1) {
		say(why, why_size, "an IOSurface texture is one 2D level");
		return NULL;
	}
	size_t planes = IOSurfaceGetPlaneCount(surface);
	size_t stride = planes ? IOSurfaceGetBytesPerRowOfPlane(surface, plane) : IOSurfaceGetBytesPerRow(surface);
	size_t offset = 0;
	if (plane > 0) {
		if (plane >= planes) {
			say(why, why_size, "plane %u of %zu", plane, planes);
			return NULL;
		}
		offset = (size_t)((const char *)IOSurfaceGetBaseAddressOfPlane(surface, plane) -
		    (const char *)IOSurfaceGetBaseAddress(surface));
	}
	if (stride % 64 || offset % 64) {
		say(why, why_size, "rows of %zu bytes at offset %zu (the Mali needs multiples of 64)", stride, offset);
		return NULL;
	}
	if (stride < (size_t)templat.width0 * util_format_get_blocksize(templat.format)) {
		say(why, why_size, "rows of %zu bytes for %u pixels of %s", stride, templat.width0,
		    util_format_short_name(templat.format));
		return NULL;
	}
	int fd = b2l_drm_surface_fd(IOSurfaceGetID(surface), IOSurfaceGetAllocSize(surface));
	if (fd < 0) {
		say(why, why_size, "no surface fd");
		return NULL;
	}
	struct winsys_handle wh;
	memset(&wh, 0, sizeof(wh));
	wh.type = WINSYS_HANDLE_TYPE_FD;
	wh.handle = (unsigned)fd;
	wh.stride = (unsigned)stride;
	wh.offset = (unsigned)offset;
	wh.modifier = DRM_FORMAT_MOD_LINEAR;
	templat.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_SHARED | PIPE_BIND_LINEAR;
	struct b2l_mtl_texture *t = calloc(1, sizeof(*t));
	if (t == NULL) {
		b2l_drm_surface_fd_close(fd);
		say(why, why_size, "out of memory");
		return NULL;
	}
	obj_init(&t->base, texture_destroy);
	t->dev = dev;
	t->format = templat.format;
	t->desc = *desc;
	pthread_mutex_lock(&dev->lock);
	t->res = dev->screen->resource_from_handle(dev->screen, &templat, &wh, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
	pthread_mutex_unlock(&dev->lock);
	b2l_drm_surface_fd_close(fd);
	if (t->res == NULL) {
		say(why, why_size, "resource_from_handle refused surface %u, %s %ux%u rows %zu", IOSurfaceGetID(surface),
		    util_format_short_name(templat.format), templat.width0, templat.height0, stride);
		free(t);
		return NULL;
	}
	t->surface = (IOSurfaceRef)CFRetain(surface);
	return t;
}

/* The layer (array slice, cube face) or depth plane a command names. */
static unsigned
texture_layer(const struct b2l_mtl_texture *t, uint32_t slice, uint32_t depth_plane)
{
	return t->res->target == PIPE_TEXTURE_3D ? depth_plane : slice;
}

static void
texture_box(const struct b2l_mtl_texture *t, const struct b2l_mtl_region *r, uint32_t slice, struct pipe_box *box)
{
	bool is3d = t->res->target == PIPE_TEXTURE_3D;
	u_box_3d(r->x, r->y, is3d ? r->z : slice, MAX2(r->width, 1), MAX2(r->height, 1), is3d ? MAX2(r->depth, 1) : 1,
	    box);
}

static void
texture_replace(struct b2l_mtl_texture *t, const struct b2l_mtl_region *region, uint32_t level, uint32_t slice,
    const void *bytes, uint64_t bytes_per_row, uint64_t bytes_per_image)
{
	if (t == NULL || bytes == NULL || level > t->res->last_level) {
		return;
	}
	struct pipe_box box;
	texture_box(t, region, slice, &box);
	uint64_t image = bytes_per_image != 0 ? bytes_per_image : bytes_per_row * (uint64_t)box.height;
	pthread_mutex_lock(&t->dev->lock);
	t->dev->pipe->texture_subdata(t->dev->pipe, t->res, level, PIPE_MAP_WRITE, &box, bytes,
	    (unsigned)bytes_per_row, (uintptr_t)image);
	pthread_mutex_unlock(&t->dev->lock);
}

/* Rows of a mapped texture into memory laid out as the caller asked. */
static void
texture_read_locked(struct b2l_mtl_texture *t, const struct pipe_box *box, uint32_t level, uint8_t *out,
    uint64_t bytes_per_row, uint64_t bytes_per_image)
{
	struct pipe_context *pipe = t->dev->pipe;
	struct pipe_transfer *transfer = NULL;
	const uint8_t *map = pipe_texture_map_3d(pipe, t->res, level, PIPE_MAP_READ, box->x, box->y, box->z,
	    box->width, box->height, box->depth, &transfer);
	if (map == NULL) {
		return;
	}
	size_t row = (size_t)box->width * util_format_get_blocksize(t->format);
	uint64_t image = bytes_per_image != 0 ? bytes_per_image : bytes_per_row * (uint64_t)box->height;
	for (int z = 0; z < box->depth; z++) {
		for (int y = 0; y < box->height; y++) {
			memcpy(out + (uint64_t)z * image + (uint64_t)y * bytes_per_row,
			    map + (size_t)z * transfer->layer_stride + (size_t)y * transfer->stride, row);
		}
	}
	pipe_texture_unmap(pipe, transfer);
}

static void
texture_get(struct b2l_mtl_texture *t, const struct b2l_mtl_region *region, uint32_t level, uint32_t slice,
    void *bytes, uint64_t bytes_per_row, uint64_t bytes_per_image)
{
	if (t == NULL || bytes == NULL || level > t->res->last_level) {
		return;
	}
	struct pipe_box box;
	texture_box(t, region, slice, &box);
	pthread_mutex_lock(&t->dev->lock);
	texture_read_locked(t, &box, level, bytes, bytes_per_row, bytes_per_image);
	pthread_mutex_unlock(&t->dev->lock);
}

static uint64_t
texture_allocated_size(struct b2l_mtl_texture *t)
{
	if (t == NULL) {
		return 0;
	}
	uint64_t bytes = 0;
	for (unsigned l = 0; l <= t->res->last_level; l++) {
		bytes += (uint64_t)u_minify(t->res->width0, l) * u_minify(t->res->height0, l) *
		    u_minify(t->res->depth0, l) * t->res->array_size * util_format_get_blocksize(t->format) *
		    MAX2(t->res->nr_samples, 1);
	}
	return bytes;
}

static void
texture_release(struct b2l_mtl_texture *t)
{
	obj_unref(t);
}

/* ---- samplers and depth-stencil states ---- */

struct b2l_mtl_sampler {
	struct obj base;
	struct b2l_mtl_device *dev;
	void *cso;
};

static void
sampler_destroy(void *self)
{
	struct b2l_mtl_sampler *s = self;
	pthread_mutex_lock(&s->dev->lock);
	if (s->cso != NULL) {
		s->dev->pipe->delete_sampler_state(s->dev->pipe, s->cso);
	}
	pthread_mutex_unlock(&s->dev->lock);
	free(s);
}

static unsigned
sampler_wrap(uint32_t mode)
{
	switch (mode) {
	case 1: return PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE;
	case 2: return PIPE_TEX_WRAP_REPEAT;
	case 3: return PIPE_TEX_WRAP_MIRROR_REPEAT;
	case 4: case 5: return PIPE_TEX_WRAP_CLAMP_TO_BORDER;
	default: return PIPE_TEX_WRAP_CLAMP_TO_EDGE;
	}
}

static struct b2l_mtl_sampler *
sampler_new(struct b2l_mtl_device *dev, const struct b2l_mtl_sampler_desc *d)
{
	struct pipe_sampler_state s;
	memset(&s, 0, sizeof(s));
	s.wrap_s = sampler_wrap(d->address[0]);
	s.wrap_t = sampler_wrap(d->address[1]);
	s.wrap_r = sampler_wrap(d->address[2]);
	s.min_img_filter = d->min_filter == 1 ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
	s.mag_img_filter = d->mag_filter == 1 ? PIPE_TEX_FILTER_LINEAR : PIPE_TEX_FILTER_NEAREST;
	s.min_mip_filter = d->mip_filter == 2 ? PIPE_TEX_MIPFILTER_LINEAR :
	    d->mip_filter == 1 ? PIPE_TEX_MIPFILTER_NEAREST : PIPE_TEX_MIPFILTER_NONE;
	s.compare_mode = d->compare != 0 ? PIPE_TEX_COMPARE_R_TO_TEXTURE : PIPE_TEX_COMPARE_NONE;
	s.compare_func = d->compare & 7;      /* Metal's order is Gallium's */
	s.unnormalized_coords = !d->normalized;
	s.max_anisotropy = d->max_anisotropy > 1 ? MIN2(d->max_anisotropy, 16) : 0;
	s.seamless_cube_map = 1;
	s.min_lod = d->lod_min;
	s.max_lod = d->mip_filter == 0 ? d->lod_min : d->lod_max;
	/* ClampToZero is transparent black; ClampToBorderColor the given one */
	bool border = d->address[0] == 5 || d->address[1] == 5 || d->address[2] == 5;
	float alpha = border && d->border != 0 ? 1.0f : 0.0f;
	float colour = border && d->border == 2 ? 1.0f : 0.0f;
	s.border_color.f[0] = s.border_color.f[1] = s.border_color.f[2] = colour;
	s.border_color.f[3] = alpha;
	struct b2l_mtl_sampler *out = calloc(1, sizeof(*out));
	if (out == NULL) {
		return NULL;
	}
	obj_init(&out->base, sampler_destroy);
	out->dev = dev;
	pthread_mutex_lock(&dev->lock);
	out->cso = dev->pipe->create_sampler_state(dev->pipe, &s);
	pthread_mutex_unlock(&dev->lock);
	if (out->cso == NULL) {
		free(out);
		return NULL;
	}
	return out;
}

static void
sampler_release(struct b2l_mtl_sampler *s)
{
	obj_unref(s);
}

struct b2l_mtl_depth_stencil {
	struct obj base;
	struct b2l_mtl_device *dev;
	void *cso;
};

static void
depth_stencil_destroy(void *self)
{
	struct b2l_mtl_depth_stencil *s = self;
	pthread_mutex_lock(&s->dev->lock);
	if (s->cso != NULL) {
		s->dev->pipe->delete_depth_stencil_alpha_state(s->dev->pipe, s->cso);
	}
	pthread_mutex_unlock(&s->dev->lock);
	free(s);
}

static unsigned
stencil_op(uint32_t op)
{
	switch (op) {
	case 1: return PIPE_STENCIL_OP_ZERO;
	case 2: return PIPE_STENCIL_OP_REPLACE;
	case 3: return PIPE_STENCIL_OP_INCR;
	case 4: return PIPE_STENCIL_OP_DECR;
	case 5: return PIPE_STENCIL_OP_INVERT;
	case 6: return PIPE_STENCIL_OP_INCR_WRAP;
	case 7: return PIPE_STENCIL_OP_DECR_WRAP;
	default: return PIPE_STENCIL_OP_KEEP;
	}
}

static struct b2l_mtl_depth_stencil *
depth_stencil_new(struct b2l_mtl_device *dev, const struct b2l_mtl_depth_stencil_desc *d)
{
	struct pipe_depth_stencil_alpha_state s;
	memset(&s, 0, sizeof(s));
	s.depth_enabled = d->depth_compare != 7 || d->depth_write;
	s.depth_writemask = d->depth_write;
	s.depth_func = d->depth_compare & 7;
	for (unsigned i = 0; i < 2 && d->stencil; i++) {
		const struct b2l_mtl_stencil_desc *f = i == 0 ? &d->front : &d->back;
		s.stencil[i].enabled = 1;
		s.stencil[i].func = f->compare & 7;
		s.stencil[i].fail_op = stencil_op(f->fail);
		s.stencil[i].zfail_op = stencil_op(f->depth_fail);
		s.stencil[i].zpass_op = stencil_op(f->pass);
		s.stencil[i].valuemask = f->read_mask & 0xff;
		s.stencil[i].writemask = f->write_mask & 0xff;
	}
	struct b2l_mtl_depth_stencil *out = calloc(1, sizeof(*out));
	if (out == NULL) {
		return NULL;
	}
	obj_init(&out->base, depth_stencil_destroy);
	out->dev = dev;
	pthread_mutex_lock(&dev->lock);
	out->cso = dev->pipe->create_depth_stencil_alpha_state(dev->pipe, &s);
	pthread_mutex_unlock(&dev->lock);
	if (out->cso == NULL) {
		free(out);
		return NULL;
	}
	return out;
}

static void
depth_stencil_release(struct b2l_mtl_depth_stencil *s)
{
	obj_unref(s);
}

/* ---- libraries and functions ---- */

struct b2l_mtl_library {
	struct obj base;
	struct b2l_mtl_device *dev;
	void *mem;                      /* ralloc: the parsed library */
	const uint8_t *map;             /* the file, mapped, or ... */
	size_t map_size;
	uint8_t *copy;                  /* ... the bytes given, copied */
	struct air_metallib lib;
};

static void
library_destroy(void *self)
{
	struct b2l_mtl_library *l = self;
	ralloc_free(l->mem);
	if (l->map != NULL) {
		munmap((void *)l->map, l->map_size);
	}
	free(l->copy);
	free(l);
}

/* The AIR64 slice of a fat (0xCAFEBABE, big-endian) metallib, or the whole
 * file when it is thin; NULL when it has no AIR64 slice. */
static const uint8_t *
thin_slice(const uint8_t *data, size_t size, size_t *out_size)
{
	if (size < 8 || data[0] != 0xca || data[1] != 0xfe || data[2] != 0xba || data[3] != 0xbe) {
		*out_size = size;
		return data;
	}
	uint32_t n = (uint32_t)data[4] << 24 | (uint32_t)data[5] << 16 | (uint32_t)data[6] << 8 | data[7];
	for (uint32_t i = 0; i < n && 8 + 20 * (size_t)i + 20 <= size; i++) {
		const uint8_t *e = data + 8 + 20 * i;
		uint32_t cpu = (uint32_t)e[0] << 24 | (uint32_t)e[1] << 16 | (uint32_t)e[2] << 8 | e[3];
		uint32_t off = (uint32_t)e[8] << 24 | (uint32_t)e[9] << 16 | (uint32_t)e[10] << 8 | e[11];
		uint32_t len = (uint32_t)e[12] << 24 | (uint32_t)e[13] << 16 | (uint32_t)e[14] << 8 | e[15];
		if (cpu == MTL_AIR64_CPU && (size_t)off + len <= size) {
			*out_size = len;
			return data + off;
		}
	}
	return NULL;
}

static struct b2l_mtl_library *
library_parse(struct b2l_mtl_library *l, const uint8_t *data, size_t size, char *why, size_t why_size)
{
	size_t thin_size = 0;
	const uint8_t *thin = thin_slice(data, size, &thin_size);
	const char *error = NULL;
	l->mem = ralloc_context(NULL);
	if (thin == NULL || !air_metallib_parse(l->mem, thin, thin_size, &l->lib, &error)) {
		say(why, why_size, "%s", thin == NULL ? "no AIR64 slice" : error != NULL ? error : "not a metallib");
		obj_unref(l);
		return NULL;
	}
	return l;
}

static struct b2l_mtl_library *
library_open(struct b2l_mtl_device *dev, const char *path, char *why, size_t why_size)
{
	int fd = open(path, O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0) {
		say(why, why_size, "%s: cannot open (errno %d)", path, errno);
		if (fd >= 0) {
			close(fd);
		}
		return NULL;
	}
	size_t size = (size_t)st.st_size;
	const uint8_t *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (data == MAP_FAILED) {
		say(why, why_size, "%s: mmap failed (errno %d)", path, errno);
		return NULL;
	}
	struct b2l_mtl_library *l = calloc(1, sizeof(*l));
	if (l == NULL) {
		munmap((void *)data, size);
		say(why, why_size, "out of memory");
		return NULL;
	}
	obj_init(&l->base, library_destroy);
	l->dev = dev;
	l->map = data;
	l->map_size = size;
	return library_parse(l, data, size, why, why_size);
}

static struct b2l_mtl_library *
library_new_data(struct b2l_mtl_device *dev, const void *bytes, size_t size, char *why, size_t why_size)
{
	struct b2l_mtl_library *l = calloc(1, sizeof(*l));
	if (l == NULL || bytes == NULL || size == 0 || (l->copy = malloc(size)) == NULL) {
		free(l);
		say(why, why_size, bytes == NULL || size == 0 ? "no bytes" : "out of memory");
		return NULL;
	}
	obj_init(&l->base, library_destroy);
	l->dev = dev;
	memcpy(l->copy, bytes, size);
	return library_parse(l, l->copy, size, why, why_size);
}

static uint32_t
library_count(struct b2l_mtl_library *l)
{
	return l != NULL ? l->lib.num_functions : 0;
}

static const char *
library_name(struct b2l_mtl_library *l, uint32_t index)
{
	return l != NULL && index < l->lib.num_functions ? l->lib.functions[index].name : NULL;
}

static int32_t
library_find(struct b2l_mtl_library *l, const char *name)
{
	for (uint32_t i = 0; l != NULL && name != NULL && i < l->lib.num_functions; i++) {
		if (l->lib.functions[i].name != NULL && strcmp(l->lib.functions[i].name, name) == 0) {
			return (int32_t)i;
		}
	}
	return -1;
}

static void
library_release(struct b2l_mtl_library *l)
{
	obj_unref(l);
}

struct b2l_mtl_function {
	struct obj base;
	struct b2l_mtl_library *library;        /* held: the module is in its bytes */
	uint32_t index;
	void *mem;
	struct air_constant_value *constants;
	unsigned num_constants;
	int32_t stage;
	pthread_mutex_t lock;
	nir_shader *nir;                        /* translated and cleaned up, kept for every pipeline */
	struct air_shader_info info;
	bool failed;
	char error[192];
	uint32_t translate_ms, instructions;
};

static void
function_destroy(void *self)
{
	struct b2l_mtl_function *f = self;
	ralloc_free(f->mem);
	pthread_mutex_destroy(&f->lock);
	obj_unref(f->library);
	free(f);
}

static struct b2l_mtl_function *
function_new(struct b2l_mtl_library *l, uint32_t index, const struct b2l_mtl_constant *given, uint32_t count,
    char *why, size_t why_size)
{
	if (l == NULL || index >= l->lib.num_functions) {
		say(why, why_size, "no function %u", index);
		return NULL;
	}
	const struct air_metallib_function *mf = &l->lib.functions[index];
	struct b2l_mtl_function *f = calloc(1, sizeof(*f));
	if (f == NULL) {
		say(why, why_size, "out of memory");
		return NULL;
	}
	obj_init(&f->base, function_destroy);
	pthread_mutex_init(&f->lock, NULL);
	f->library = obj_ref(l);
	f->index = index;
	f->mem = ralloc_context(NULL);
	const char *error = NULL;
	f->stage = air_module_stage(f->mem, mf->bitcode, mf->bitcode_size, NULL, &error);
	f->num_constants = air_module_function_constants(f->mem, mf->bitcode, mf->bitcode_size, &f->constants);
	struct air_constant_decl *decls = NULL;
	unsigned num_decls = air_module_function_constant_decls(f->mem, mf->bitcode, mf->bitcode_size, &decls);
	unsigned unmatched = 0;
	for (uint32_t i = 0; i < count; i++) {
		int32_t want = given[i].index;
		if (want < 0 && given[i].name != NULL) {
			for (unsigned k = 0; k < num_decls; k++) {
				if (decls[k].name != NULL && strcmp(decls[k].name, given[i].name) == 0) {
					want = (int32_t)decls[k].index;
					break;
				}
			}
		}
		bool found = false;
		for (unsigned k = 0; want >= 0 && k < f->num_constants; k++) {
			if (f->constants[k].index == (unsigned)want) {
				uint64_t bits = 0;
				memcpy(&bits, given[i].bytes, MIN2(given[i].size, sizeof(bits)));
				f->constants[k].bits = bits;
				f->constants[k].supplied = true;
				found = true;
			}
		}
		unmatched += !found;
	}
	if (unmatched != 0) {
		/* Metal ignores a constant the function does not declare. */
		say(why, why_size, "%u of %u constants given are not this function's", unmatched, count);
	}
	return f;
}

static int32_t
function_stage(struct b2l_mtl_function *f)
{
	return f != NULL ? f->stage : -1;
}

static const char *
function_name(struct b2l_mtl_function *f)
{
	return f != NULL ? f->library->lib.functions[f->index].name : NULL;
}

static void
function_release(struct b2l_mtl_function *f)
{
	obj_unref(f);
}

static unsigned
count_instrs(nir_shader *nir)
{
	unsigned n = 0;
	nir_foreach_function_impl(impl, nir) {
		nir_foreach_block(block, impl) {
			nir_foreach_instr(instr, block) {
				n++;
			}
		}
	}
	return n;
}

/* NIR's usual clean-up to a fixed point: what folding the constants left. */
static void
tidy(nir_shader *nir)
{
	bool progress = true;
	for (unsigned i = 0; progress && i < 16; i++) {
		progress = false;
		progress |= nir_copy_prop(nir);
		progress |= nir_opt_constant_folding(nir);
		progress |= nir_opt_dce(nir);
		progress |= nir_opt_dead_cf(nir);
		progress |= nir_opt_cse(nir);
	}
}

/* Translated once, with the function's constants, and kept. */
static bool
function_translate(struct b2l_mtl_device *dev, struct b2l_mtl_function *f)
{
	pthread_mutex_lock(&f->lock);
	if (f->nir == NULL && !f->failed) {
		const struct air_metallib_function *mf = &f->library->lib.functions[f->index];
		struct air_to_nir_options options;
		memset(&options, 0, sizeof(options));
		options.constants = f->constants;
		options.num_constants = f->num_constants;
		const char *error = NULL;
		uint64_t t0 = now_us();
		f->nir = air_to_nir(f->mem, mf->bitcode, mf->bitcode_size, NULL, &options, dev->nir_options, &f->info,
		    &error);
		if (f->nir == NULL) {
			f->failed = true;
			snprintf(f->error, sizeof(f->error), "%s: %s", mf->name, error != NULL ? error : "refused");
		} else {
			tidy(f->nir);
			f->instructions = count_instrs(f->nir);
		}
		f->translate_ms = (uint32_t)((now_us() - t0) / 1000);
	}
	bool ok = f->nir != NULL;
	pthread_mutex_unlock(&f->lock);
	return ok;
}

/* ---- render pipelines ---- */

/* What a stage of a pipeline reads, from its functions' interface (M2b):
 * Metal's [[buffer(n)]] constant is UBO n, a device buffer SSBO n,
 * [[texture(n)]] sampler view n, [[sampler(n)]] sampler state n. */
struct stage_bindings {
	unsigned ubos;                          /* the shader's UBO count, before Panfrost's sysvals */
	uint32_t ssbo_mask, ssbo_written;
	uint32_t texture_mask, null_texture_mask;
	uint32_t sampler_mask;
	void *inline_samplers[B2L_MTL_MAX_SAMPLERS];    /* constexpr samplers, made here */
	unsigned images, argument_members;      /* not bound yet: logged when made */
};

struct b2l_mtl_render_pipeline {
	struct obj base;
	struct b2l_mtl_device *dev;
	struct b2l_mtl_function *vertex, *fragment;     /* held */
	void *vs, *fs, *blend, *velems;
	unsigned num_elements;
	uint32_t vb_mask;                       /* vertex buffer slots the elements read */
	unsigned vb_count;                      /* the highest of them + 1 */
	bool point_size;                        /* the vertex function writes [[point_size]] */
	struct stage_bindings bind[2];          /* B2L_MTL_VERTEX, B2L_MTL_FRAGMENT */
	struct b2l_mtl_render_pipeline_desc desc;
};

static void
render_pipeline_destroy(void *self)
{
	struct b2l_mtl_render_pipeline *p = self;
	struct pipe_context *pipe = p->dev->pipe;
	pthread_mutex_lock(&p->dev->lock);
	for (unsigned s = 0; s < 2; s++) {
		for (unsigned i = 0; i < B2L_MTL_MAX_SAMPLERS; i++) {
			if (p->bind[s].inline_samplers[i] != NULL) {
				pipe->delete_sampler_state(pipe, p->bind[s].inline_samplers[i]);
			}
		}
	}
	if (p->vs != NULL) {
		pipe->delete_vs_state(pipe, p->vs);
	}
	if (p->fs != NULL) {
		pipe->delete_fs_state(pipe, p->fs);
	}
	if (p->blend != NULL) {
		pipe->delete_blend_state(pipe, p->blend);
	}
	if (p->velems != NULL) {
		pipe->delete_vertex_elements_state(pipe, p->velems);
	}
	pthread_mutex_unlock(&p->dev->lock);
	obj_unref(p->vertex);
	obj_unref(p->fragment);
	free(p);
}

/* Metal's [[position]] has its origin at the top left, as the Mali's pixel
 * coordinate does, and its centre at the half pixel; Bifrost lowers
 * load_frag_coord to the integer pixel coordinate, so the half is added. */
static bool
frag_coord_center(nir_builder *b, nir_intrinsic_instr *intr, UNUSED void *data)
{
	if (intr->intrinsic != nir_intrinsic_load_frag_coord) {
		return false;
	}
	b->cursor = nir_after_instr(&intr->instr);
	nir_def *wpos = nir_fadd(b, &intr->def, nir_imm_vec4(b, 0.5f, 0.5f, 0.0f, 0.0f));
	nir_def_rewrite_uses_after(&intr->def, wpos, wpos->parent_instr);
	return true;
}

/*
 * Each fragment input takes the slot of the vertex output with the same link
 * name (the frontend numbers each stage's varyings in its own order); one the
 * vertex function does not write gets a slot of its own nothing writes.
 */
static void
link_varyings(nir_shader *fs, const struct air_shader_info *vi, const struct air_shader_info *fi,
    struct b2l_mtl_pipeline_report *report)
{
	uint64_t written = 0;
	for (unsigned i = 0; i < vi->num_varyings; i++) {
		if (vi->varyings[i].slot < 64) {
			written |= BITFIELD64_BIT(vi->varyings[i].slot);
		}
	}
	uint64_t taken = written, old_bits = 0, new_bits = 0;
	unsigned spare = VARYING_SLOT_VAR31;
	nir_foreach_shader_in_variable(var, fs) {
		const struct air_varying *in = NULL;
		for (unsigned j = 0; j < fi->num_varyings; j++) {
			if ((int)fi->varyings[j].slot == var->data.location) {
				in = &fi->varyings[j];
				break;
			}
		}
		if (in == NULL) {
			continue;
		}
		int to = -1;
		for (unsigned k = 0; in->link != NULL && k < vi->num_varyings; k++) {
			if (vi->varyings[k].link != NULL && strcmp(vi->varyings[k].link, in->link) == 0) {
				to = (int)vi->varyings[k].slot;
				break;
			}
		}
		if (to < 0) {
			while (spare > VARYING_SLOT_VAR0 && (taken & BITFIELD64_BIT(spare))) {
				spare--;
			}
			to = (int)spare;
			taken |= BITFIELD64_BIT(spare);
			report->varyings_unmatched++;
		} else {
			report->varyings_linked++;
		}
		old_bits |= BITFIELD64_BIT(var->data.location);
		new_bits |= BITFIELD64_BIT(to);
		var->data.location = to;
	}
	fs->info.inputs_read = (fs->info.inputs_read & ~old_bits) | new_bits;
}

/* What a Gallium frontend gives the driver: system values lowered, the
 * fragment position at the half pixel, driver locations. */
static void
prepare_for_gallium(nir_shader *nir)
{
	nir_lower_system_values(nir);
	if (nir->info.stage == MESA_SHADER_COMPUTE) {
		nir_lower_compute_system_values(nir, NULL);
	}
	if (nir->info.stage == MESA_SHADER_FRAGMENT) {
		nir_shader_intrinsics_pass(nir, frag_coord_center, nir_metadata_control_flow, NULL);
	}
	/* The frontend sets the framebuffer-fetch facts itself (no gather runs on
	 * HakiOS's path); kept across the gather Panfrost's CSOs rely on. */
	bool fs = nir->info.stage == MESA_SHADER_FRAGMENT;
	bool fbfetch = fs && nir->info.fs.uses_fbfetch_output, coherent = fs && nir->info.fs.fbfetch_coherent;
	uint64_t outputs_read = nir->info.outputs_read;
	nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
	nir->info.outputs_read |= outputs_read;
	if (fbfetch) {
		nir->info.fs.uses_fbfetch_output = true;
	}
	if (coherent) {
		nir->info.fs.fbfetch_coherent = true;
	}
	nir_assign_io_var_locations(nir, nir_var_shader_in, &nir->num_inputs, nir->info.stage);
	nir_assign_io_var_locations(nir, nir_var_shader_out, &nir->num_outputs, nir->info.stage);
	/* Metal's [[buffer(0)]] is UBO 0, which Panfrost pushes as uniforms. */
	nir->info.first_ubo_is_default_ubo = true;
}

static unsigned
blend_factor(uint32_t f)
{
	switch (f) {
	case 0: return PIPE_BLENDFACTOR_ZERO;
	case 1: return PIPE_BLENDFACTOR_ONE;
	case 2: return PIPE_BLENDFACTOR_SRC_COLOR;
	case 3: return PIPE_BLENDFACTOR_INV_SRC_COLOR;
	case 4: return PIPE_BLENDFACTOR_SRC_ALPHA;
	case 5: return PIPE_BLENDFACTOR_INV_SRC_ALPHA;
	case 6: return PIPE_BLENDFACTOR_DST_COLOR;
	case 7: return PIPE_BLENDFACTOR_INV_DST_COLOR;
	case 8: return PIPE_BLENDFACTOR_DST_ALPHA;
	case 9: return PIPE_BLENDFACTOR_INV_DST_ALPHA;
	case 10: return PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE;
	case 11: return PIPE_BLENDFACTOR_CONST_COLOR;
	case 12: return PIPE_BLENDFACTOR_INV_CONST_COLOR;
	case 13: return PIPE_BLENDFACTOR_CONST_ALPHA;
	case 14: return PIPE_BLENDFACTOR_INV_CONST_ALPHA;
	case 15: return PIPE_BLENDFACTOR_SRC1_COLOR;
	case 16: return PIPE_BLENDFACTOR_INV_SRC1_COLOR;
	case 17: return PIPE_BLENDFACTOR_SRC1_ALPHA;
	case 18: return PIPE_BLENDFACTOR_INV_SRC1_ALPHA;
	default: return PIPE_BLENDFACTOR_ONE;
	}
}

static unsigned
blend_op(uint32_t op)
{
	switch (op) {
	case 1: return PIPE_BLEND_SUBTRACT;
	case 2: return PIPE_BLEND_REVERSE_SUBTRACT;
	case 3: return PIPE_BLEND_MIN;
	case 4: return PIPE_BLEND_MAX;
	default: return PIPE_BLEND_ADD;
	}
}

/* What one stage reads (struct stage_bindings), from its interface. */
static void
collect_bindings(const struct air_shader_info *info, unsigned ubos, struct stage_bindings *b)
{
	memset(b, 0, sizeof(*b));
	b->ubos = MIN2(ubos, B2L_MTL_MAX_BUFFERS);
	for (unsigned i = 0; i < info->num_resources; i++) {
		const struct air_resource *r = &info->resources[i];
		if (r->argbuf_set != ~0u) {
			b->argument_members++;
			continue;
		}
		switch (r->kind) {
		case AIR_RESOURCE_DEVICE_BUFFER:
			if (r->index < B2L_MTL_MAX_BUFFERS) {
				b->ssbo_mask |= 1u << r->index;
				if (r->ir_write) {
					b->ssbo_written |= 1u << r->index;
				}
			}
			break;
		case AIR_RESOURCE_TEXTURE:
			if (r->index < B2L_MTL_MAX_TEXTURES) {
				b->texture_mask |= 1u << r->index;
				if (r->null_texture) {
					b->null_texture_mask |= 1u << r->index;
				}
			}
			break;
		case AIR_RESOURCE_SAMPLER:
			if (r->index < B2L_MTL_MAX_SAMPLERS && !r->inline_sampler) {
				b->sampler_mask |= 1u << r->index;
			}
			break;
		case AIR_RESOURCE_IMAGE:
			b->images++;
			break;
		default:
			break;
		}
	}
}

static unsigned
inline_wrap(enum air_sampler_address a)
{
	switch (a) {
	case AIR_SAMPLER_CLAMP_TO_EDGE: return PIPE_TEX_WRAP_CLAMP_TO_EDGE;
	case AIR_SAMPLER_REPEAT: return PIPE_TEX_WRAP_REPEAT;
	case AIR_SAMPLER_MIRRORED_REPEAT: return PIPE_TEX_WRAP_MIRROR_REPEAT;
	default: return PIPE_TEX_WRAP_CLAMP_TO_BORDER;      /* clamp to zero, clamp to border */
	}
}

/* A constexpr sampler of the shader's own as a CSO (the device lock held).
 * The compare function is left off: AIR numbers it as metal_stdlib does,
 * not as the API does, and none of QuartzCore's three samplers sets it. */
static void *
inline_sampler(struct b2l_mtl_device *dev, const struct air_sampler_state *a)
{
	struct pipe_sampler_state s;
	memset(&s, 0, sizeof(s));
	s.wrap_s = inline_wrap(a->address[0]);
	s.wrap_t = inline_wrap(a->address[1]);
	s.wrap_r = inline_wrap(a->address[2]);
	s.min_img_filter = a->min_filter == AIR_SAMPLER_NEAREST ? PIPE_TEX_FILTER_NEAREST : PIPE_TEX_FILTER_LINEAR;
	s.mag_img_filter = a->mag_filter == AIR_SAMPLER_NEAREST ? PIPE_TEX_FILTER_NEAREST : PIPE_TEX_FILTER_LINEAR;
	s.min_mip_filter = a->mip_filter == AIR_SAMPLER_MIP_LINEAR ? PIPE_TEX_MIPFILTER_LINEAR :
	    a->mip_filter == AIR_SAMPLER_MIP_NEAREST ? PIPE_TEX_MIPFILTER_NEAREST : PIPE_TEX_MIPFILTER_NONE;
	s.unnormalized_coords = a->pixel_coord;
	s.max_anisotropy = a->max_anisotropy > 1 ? MIN2(a->max_anisotropy, 16) : 0;
	s.seamless_cube_map = 1;
	s.min_lod = _mesa_half_to_float(a->lod_min);
	s.max_lod = _mesa_half_to_float(a->lod_max);
	/* clamp to zero: transparent black; clamp to border: metal_stdlib's
	 * border_color (transparent black, opaque black, opaque white) */
	bool to_border = false;
	for (unsigned i = 0; i < 3; i++) {
		to_border |= a->address[i] == AIR_SAMPLER_CLAMP_TO_BORDER;
	}
	if (to_border) {
		float colour = a->border_color == 2 ? 1.0f : 0.0f;
		s.border_color.f[0] = s.border_color.f[1] = s.border_color.f[2] = colour;
		s.border_color.f[3] = a->border_color != 0 ? 1.0f : 0.0f;
	}
	return dev->pipe->create_sampler_state(dev->pipe, &s);
}

/* MTLColorWriteMask: red 8, green 4, blue 2, alpha 1. */
static unsigned
write_mask(uint32_t m)
{
	return ((m & 8) ? PIPE_MASK_R : 0) | ((m & 4) ? PIPE_MASK_G : 0) | ((m & 2) ? PIPE_MASK_B : 0) |
	    ((m & 1) ? PIPE_MASK_A : 0);
}

static struct b2l_mtl_render_pipeline *
render_pipeline_new(struct b2l_mtl_device *dev, const struct b2l_mtl_render_pipeline_desc *desc,
    struct b2l_mtl_pipeline_report *report, char *why, size_t why_size)
{
	struct b2l_mtl_pipeline_report scratch;
	if (report == NULL) {
		report = &scratch;
	}
	memset(report, 0, sizeof(*report));
	if (desc->vertex == NULL) {
		say(why, why_size, "no vertex function");
		return NULL;
	}
	if (desc->vertex->stage != MESA_SHADER_VERTEX ||
	    (desc->fragment != NULL && desc->fragment->stage != MESA_SHADER_FRAGMENT)) {
		say(why, why_size, "stages %d and %d are not a vertex and a fragment function", desc->vertex->stage,
		    desc->fragment != NULL ? desc->fragment->stage : -1);
		return NULL;
	}
	uint64_t t0 = now_us();
	bool translated = function_translate(dev, desc->vertex) &&
	    (desc->fragment == NULL || function_translate(dev, desc->fragment));
	report->translate_ms = (uint32_t)((now_us() - t0) / 1000);
	if (!translated) {
		say(why, why_size, "%s", desc->vertex->failed ? desc->vertex->error : desc->fragment->error);
		return NULL;
	}
	report->vs_instructions = desc->vertex->instructions;
	report->fs_instructions = desc->fragment != NULL ? desc->fragment->instructions : 0;

	struct b2l_mtl_render_pipeline *p = calloc(1, sizeof(*p));
	if (p == NULL) {
		say(why, why_size, "out of memory");
		return NULL;
	}
	obj_init(&p->base, render_pipeline_destroy);
	p->dev = dev;
	p->vertex = obj_ref(desc->vertex);
	p->fragment = obj_ref(desc->fragment);
	p->desc = *desc;
	p->desc.vertex = p->desc.fragment = NULL;

	nir_shader *vs = nir_shader_clone(NULL, desc->vertex->nir);
	nir_shader *fs = desc->fragment != NULL ? nir_shader_clone(NULL, desc->fragment->nir) : NULL;
	if (fs != NULL) {
		link_varyings(fs, &desc->vertex->info, &desc->fragment->info, report);
	}
	prepare_for_gallium(vs);
	if (fs != NULL) {
		prepare_for_gallium(fs);
	}

	/* Vertex elements in driver-location order, from the descriptor. */
	struct pipe_vertex_element elements[PIPE_MAX_ATTRIBS];
	memset(elements, 0, sizeof(elements));
	unsigned num_elements = 0;
	nir_foreach_shader_in_variable(var, vs) {
		unsigned slot = var->data.driver_location;
		int index = var->data.location - VERT_ATTRIB_GENERIC0;
		if (slot >= PIPE_MAX_ATTRIBS) {
			continue;
		}
		struct pipe_vertex_element *e = &elements[slot];
		enum pipe_format format = index >= 0 && index < B2L_MTL_MAX_ATTRIBUTES ?
		    vertex_format(desc->attribute[index].format) : PIPE_FORMAT_NONE;
		if (format == PIPE_FORMAT_NONE) {
			/* not described: a zero read from buffer 0 */
			e->src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;
		} else {
			uint32_t buffer = desc->attribute[index].buffer;
			e->src_format = format;
			e->src_offset = (uint16_t)desc->attribute[index].offset;
			e->vertex_buffer_index = buffer < B2L_MTL_MAX_BUFFERS ? buffer : 0;
			if (buffer < B2L_MTL_MAX_BUFFERS) {
				uint32_t step = desc->layout[buffer].step_function;
				e->src_stride = step == 0 ? 0 : (uint16_t)desc->layout[buffer].stride;
				e->instance_divisor = step == 2 ? MAX2(desc->layout[buffer].step_rate, 1) : 0;
			}
		}
		num_elements = MAX2(num_elements, slot + 1);
	}
	report->attributes = num_elements;
	for (unsigned i = 0; i < num_elements; i++) {
		p->vb_mask |= 1u << elements[i].vertex_buffer_index;
		p->vb_count = MAX2(p->vb_count, elements[i].vertex_buffer_index + 1u);
	}
	p->point_size = (vs->info.outputs_written & VARYING_BIT_PSIZ) != 0;
	collect_bindings(&desc->vertex->info, vs->info.num_ubos, &p->bind[B2L_MTL_VERTEX]);
	if (fs != NULL) {
		collect_bindings(&desc->fragment->info, fs->info.num_ubos, &p->bind[B2L_MTL_FRAGMENT]);
	}

	struct pipe_blend_state blend;
	memset(&blend, 0, sizeof(blend));
	blend.independent_blend_enable = 1;
	blend.alpha_to_coverage = desc->alpha_to_coverage;
	for (unsigned i = 0; i < B2L_MTL_MAX_COLORS; i++) {
		const struct b2l_mtl_color_desc *c = &desc->color[i];
		if (c->pixel_format != 0) {
			blend.max_rt = i;
		}
		blend.rt[i].colormask = write_mask(c->write_mask);
		if (c->blend) {
			blend.rt[i].blend_enable = 1;
			blend.rt[i].rgb_func = blend_op(c->rgb_op);
			blend.rt[i].alpha_func = blend_op(c->alpha_op);
			blend.rt[i].rgb_src_factor = blend_factor(c->src_rgb);
			blend.rt[i].rgb_dst_factor = blend_factor(c->dst_rgb);
			blend.rt[i].alpha_src_factor = blend_factor(c->src_alpha);
			blend.rt[i].alpha_dst_factor = blend_factor(c->dst_alpha);
		}
	}

	struct pipe_context *pipe = dev->pipe;
	pthread_mutex_lock(&dev->lock);
	uint64_t t1 = now_us();
	struct pipe_shader_state state;
	memset(&state, 0, sizeof(state));
	state.type = PIPE_SHADER_IR_NIR;
	state.ir.nir = vs;
	p->vs = pipe->create_vs_state(pipe, &state);    /* the driver owns the NIR now */
	if (fs != NULL) {
		state.ir.nir = fs;
		p->fs = pipe->create_fs_state(pipe, &state);
	}
	report->compile_ms = (uint32_t)((now_us() - t1) / 1000);
	p->blend = pipe->create_blend_state(pipe, &blend);
	if (num_elements != 0) {
		p->velems = pipe->create_vertex_elements_state(pipe, num_elements, elements);
	}
	p->num_elements = num_elements;
	unsigned inline_count = 0;
	for (unsigned s = 0; s < 2; s++) {
		const struct air_shader_info *info = s == B2L_MTL_VERTEX ? &desc->vertex->info :
		    desc->fragment != NULL ? &desc->fragment->info : NULL;
		for (unsigned i = 0; info != NULL && i < info->num_resources; i++) {
			const struct air_resource *r = &info->resources[i];
			if (r->kind == AIR_RESOURCE_SAMPLER && r->inline_sampler && r->index < B2L_MTL_MAX_SAMPLERS &&
			    p->bind[s].inline_samplers[r->index] == NULL) {
				p->bind[s].inline_samplers[r->index] = inline_sampler(dev, &r->sampler);
				p->bind[s].sampler_mask |= 1u << r->index;
				inline_count++;
			}
		}
	}
	pthread_mutex_unlock(&dev->lock);
	if (p->vs == NULL || (fs != NULL && p->fs == NULL) || p->blend == NULL ||
	    (num_elements != 0 && p->velems == NULL)) {
		say(why, why_size, "Panfrost refused the %s", p->vs == NULL ? "vertex shader" :
		    fs != NULL && p->fs == NULL ? "fragment shader" : p->blend == NULL ? "blend state" :
		    "vertex elements");
		obj_unref(p);
		return NULL;
	}
	b2l_drm_note("mtl M2: pipeline %s + %s: AIR %u ms, Panfrost %u ms; %u + %u NIR instructions, "
	    "varyings %u linked %u unmatched, %u vertex elements",
	    function_name(desc->vertex), desc->fragment != NULL ? function_name(desc->fragment) : "-",
	    report->translate_ms, report->compile_ms, report->vs_instructions, report->fs_instructions,
	    report->varyings_linked, report->varyings_unmatched, report->attributes);
	const struct stage_bindings *bv = &p->bind[B2L_MTL_VERTEX], *bf = &p->bind[B2L_MTL_FRAGMENT];
	b2l_drm_note("mtl M2: pipeline %s binds: UBOs %u + %u, SSBOs 0x%x + 0x%x, textures 0x%x + 0x%x (null 0x%x), "
	    "samplers 0x%x + 0x%x (%u inline), vertex buffers 0x%x%s%s",
	    function_name(desc->vertex), bv->ubos, bf->ubos, bv->ssbo_mask, bf->ssbo_mask, bv->texture_mask,
	    bf->texture_mask, bv->null_texture_mask | bf->null_texture_mask, bv->sampler_mask, bf->sampler_mask,
	    inline_count, p->vb_mask, bv->images + bf->images != 0 ? "; storage images NOT bound" : "",
	    bv->argument_members + bf->argument_members != 0 ? "; argument buffers NOT bound" : "");
	return p;
}

static void
render_pipeline_release(struct b2l_mtl_render_pipeline *p)
{
	obj_unref(p);
}

/* ---- command buffers ---- */

enum op_kind {
	OP_PASS_BEGIN,
	OP_PASS_END,
	OP_COPY_BUFFER,
	OP_FILL_BUFFER,
	OP_COPY_TEXTURE,
	OP_BUFFER_TO_TEXTURE,
	OP_TEXTURE_TO_BUFFER,
	OP_UNSUPPORTED,
	/* M2b: the render encoder */
	OP_SET_PIPELINE,
	OP_SET_BUFFER,
	OP_SET_BUFFER_OFFSET,
	OP_SET_BYTES,
	OP_SET_TEXTURE,
	OP_SET_SAMPLER,
	OP_SET_VIEWPORT,
	OP_SET_SCISSOR,
	OP_SET_RASTER,
	OP_SET_DEPTH_STENCIL,
	OP_SET_STENCIL_REF,
	OP_SET_BLEND_COLOR,
	OP_DRAW,
};

struct op {
	enum op_kind kind;
	union {
		struct b2l_mtl_pass_desc pass;
		struct {
			struct b2l_mtl_buffer *from, *to;
			uint64_t from_offset, to_offset, size;
		} copy_buffer;
		struct {
			struct b2l_mtl_buffer *buffer;
			uint64_t offset, length;
			uint8_t value;
		} fill;
		struct {
			struct b2l_mtl_texture *from, *to;
			uint32_t from_slice, from_level, to_slice, to_level, to_x, to_y, to_z;
			struct b2l_mtl_region region;
		} copy_texture;
		struct {
			struct b2l_mtl_buffer *buffer;
			struct b2l_mtl_texture *texture;
			uint64_t offset, bytes_per_row, bytes_per_image;
			struct b2l_mtl_region region;
			uint32_t slice, level;
		} buffer_texture;
		const char *what;
		struct b2l_mtl_render_pipeline *pipeline;
		struct {
			uint32_t stage, index;
			struct b2l_mtl_buffer *buffer;
			uint64_t offset;
			uint8_t *bytes;                 /* OP_SET_BYTES: a copy, freed with the op */
			uint32_t length;
		} bind;
		struct {
			uint32_t stage, index;
			struct b2l_mtl_texture *texture;
			struct b2l_mtl_sampler *sampler;
		} resource;
		struct b2l_mtl_viewport viewport;
		struct {
			uint32_t x, y, width, height;
		} scissor;
		struct {
			uint32_t which, value;
		} raster;
		struct b2l_mtl_depth_stencil *dsa;
		struct {
			uint32_t front, back;
		} stencil_ref;
		float blend_color[4];
		struct b2l_mtl_draw draw;
	} u;
};

struct b2l_mtl_cmdbuf {
	struct obj base;
	struct b2l_mtl_device *dev;
	struct util_dynarray ops;
	struct util_dynarray surfaces;  /* pipe_surface *, while executing */
	bool executed;
};

static void
op_unref(struct op *op)
{
	switch (op->kind) {
	case OP_PASS_BEGIN:
		for (unsigned i = 0; i < B2L_MTL_MAX_COLORS; i++) {
			obj_unref(op->u.pass.color[i].texture);
		}
		obj_unref(op->u.pass.depth.texture);
		obj_unref(op->u.pass.stencil.texture);
		break;
	case OP_COPY_BUFFER:
		obj_unref(op->u.copy_buffer.from);
		obj_unref(op->u.copy_buffer.to);
		break;
	case OP_FILL_BUFFER:
		obj_unref(op->u.fill.buffer);
		break;
	case OP_COPY_TEXTURE:
		obj_unref(op->u.copy_texture.from);
		obj_unref(op->u.copy_texture.to);
		break;
	case OP_BUFFER_TO_TEXTURE:
	case OP_TEXTURE_TO_BUFFER:
		obj_unref(op->u.buffer_texture.buffer);
		obj_unref(op->u.buffer_texture.texture);
		break;
	case OP_SET_PIPELINE:
		obj_unref(op->u.pipeline);
		break;
	case OP_SET_BUFFER:
		obj_unref(op->u.bind.buffer);
		break;
	case OP_SET_BYTES:
		free(op->u.bind.bytes);
		break;
	case OP_SET_TEXTURE:
		obj_unref(op->u.resource.texture);
		break;
	case OP_SET_SAMPLER:
		obj_unref(op->u.resource.sampler);
		break;
	case OP_SET_DEPTH_STENCIL:
		obj_unref(op->u.dsa);
		break;
	case OP_DRAW:
		obj_unref(op->u.draw.index_buffer);
		break;
	default:
		break;
	}
}

static void
cmdbuf_destroy(void *self)
{
	struct b2l_mtl_cmdbuf *cb = self;
	util_dynarray_foreach(&cb->ops, struct op, op) {
		op_unref(op);
	}
	util_dynarray_fini(&cb->ops);
	util_dynarray_fini(&cb->surfaces);
	free(cb);
}

static struct b2l_mtl_cmdbuf *
cmdbuf_new(struct b2l_mtl_device *dev)
{
	struct b2l_mtl_cmdbuf *cb = calloc(1, sizeof(*cb));
	if (cb == NULL) {
		return NULL;
	}
	obj_init(&cb->base, cmdbuf_destroy);
	cb->dev = dev;
	util_dynarray_init(&cb->ops, NULL);
	util_dynarray_init(&cb->surfaces, NULL);
	return cb;
}

static struct op *
cmdbuf_push(struct b2l_mtl_cmdbuf *cb, enum op_kind kind)
{
	if (cb == NULL || cb->executed) {
		return NULL;
	}
	struct op *op = util_dynarray_grow(&cb->ops, struct op, 1);
	if (op != NULL) {
		memset(op, 0, sizeof(*op));
		op->kind = kind;
	}
	return op;
}

static void
cmdbuf_pass_begin(struct b2l_mtl_cmdbuf *cb, const struct b2l_mtl_pass_desc *desc)
{
	struct op *op = cmdbuf_push(cb, OP_PASS_BEGIN);
	if (op == NULL) {
		return;
	}
	op->u.pass = *desc;
	for (unsigned i = 0; i < B2L_MTL_MAX_COLORS; i++) {
		obj_ref(op->u.pass.color[i].texture);
	}
	obj_ref(op->u.pass.depth.texture);
	obj_ref(op->u.pass.stencil.texture);
}

static void
cmdbuf_pass_end(struct b2l_mtl_cmdbuf *cb)
{
	cmdbuf_push(cb, OP_PASS_END);
}

static void
cmdbuf_copy_buffer(struct b2l_mtl_cmdbuf *cb, struct b2l_mtl_buffer *from, uint64_t from_offset,
    struct b2l_mtl_buffer *to, uint64_t to_offset, uint64_t size)
{
	struct op *op = cmdbuf_push(cb, OP_COPY_BUFFER);
	if (op == NULL) {
		return;
	}
	op->u.copy_buffer.from = obj_ref(from);
	op->u.copy_buffer.to = obj_ref(to);
	op->u.copy_buffer.from_offset = from_offset;
	op->u.copy_buffer.to_offset = to_offset;
	op->u.copy_buffer.size = size;
}

static void
cmdbuf_fill_buffer(struct b2l_mtl_cmdbuf *cb, struct b2l_mtl_buffer *buffer, uint64_t offset, uint64_t length,
    uint8_t value)
{
	struct op *op = cmdbuf_push(cb, OP_FILL_BUFFER);
	if (op == NULL) {
		return;
	}
	op->u.fill.buffer = obj_ref(buffer);
	op->u.fill.offset = offset;
	op->u.fill.length = length;
	op->u.fill.value = value;
}

static void
cmdbuf_copy_texture(struct b2l_mtl_cmdbuf *cb, struct b2l_mtl_texture *from, uint32_t from_slice,
    uint32_t from_level, const struct b2l_mtl_region *from_region, struct b2l_mtl_texture *to, uint32_t to_slice,
    uint32_t to_level, uint32_t to_x, uint32_t to_y, uint32_t to_z)
{
	struct op *op = cmdbuf_push(cb, OP_COPY_TEXTURE);
	if (op == NULL) {
		return;
	}
	op->u.copy_texture.from = obj_ref(from);
	op->u.copy_texture.to = obj_ref(to);
	op->u.copy_texture.from_slice = from_slice;
	op->u.copy_texture.from_level = from_level;
	op->u.copy_texture.region = *from_region;
	op->u.copy_texture.to_slice = to_slice;
	op->u.copy_texture.to_level = to_level;
	op->u.copy_texture.to_x = to_x;
	op->u.copy_texture.to_y = to_y;
	op->u.copy_texture.to_z = to_z;
}

static void
cmdbuf_buffer_texture(struct b2l_mtl_cmdbuf *cb, enum op_kind kind, struct b2l_mtl_buffer *buffer,
    uint64_t offset, uint64_t bytes_per_row, uint64_t bytes_per_image, const struct b2l_mtl_region *region,
    struct b2l_mtl_texture *texture, uint32_t slice, uint32_t level)
{
	struct op *op = cmdbuf_push(cb, kind);
	if (op == NULL) {
		return;
	}
	op->u.buffer_texture.buffer = obj_ref(buffer);
	op->u.buffer_texture.texture = obj_ref(texture);
	op->u.buffer_texture.offset = offset;
	op->u.buffer_texture.bytes_per_row = bytes_per_row;
	op->u.buffer_texture.bytes_per_image = bytes_per_image;
	op->u.buffer_texture.region = *region;
	op->u.buffer_texture.slice = slice;
	op->u.buffer_texture.level = level;
}

static void
cmdbuf_copy_buffer_to_texture(struct b2l_mtl_cmdbuf *cb, struct b2l_mtl_buffer *from, uint64_t offset,
    uint64_t bytes_per_row, uint64_t bytes_per_image, const struct b2l_mtl_region *region,
    struct b2l_mtl_texture *to, uint32_t slice, uint32_t level)
{
	cmdbuf_buffer_texture(cb, OP_BUFFER_TO_TEXTURE, from, offset, bytes_per_row, bytes_per_image, region, to, slice,
	    level);
}

static void
cmdbuf_copy_texture_to_buffer(struct b2l_mtl_cmdbuf *cb, struct b2l_mtl_texture *from, uint32_t slice,
    uint32_t level, const struct b2l_mtl_region *region, struct b2l_mtl_buffer *to, uint64_t offset,
    uint64_t bytes_per_row, uint64_t bytes_per_image)
{
	cmdbuf_buffer_texture(cb, OP_TEXTURE_TO_BUFFER, to, offset, bytes_per_row, bytes_per_image, region, from, slice,
	    level);
}

static void
cmdbuf_unsupported(struct b2l_mtl_cmdbuf *cb, const char *what)
{
	struct op *op = cmdbuf_push(cb, OP_UNSUPPORTED);
	if (op != NULL) {
		op->u.what = what;
	}
}

/* ---- the render encoder's commands (M2b) ---- */

static void
cmdbuf_set_pipeline(struct b2l_mtl_cmdbuf *cb, struct b2l_mtl_render_pipeline *pipeline)
{
	struct op *op = cmdbuf_push(cb, OP_SET_PIPELINE);
	if (op != NULL) {
		op->u.pipeline = obj_ref(pipeline);
	}
}

static void
cmdbuf_set_buffer(struct b2l_mtl_cmdbuf *cb, uint32_t stage, uint32_t index, struct b2l_mtl_buffer *buffer,
    uint64_t offset)
{
	struct op *op = cmdbuf_push(cb, OP_SET_BUFFER);
	if (op != NULL) {
		op->u.bind.stage = stage;
		op->u.bind.index = index;
		op->u.bind.buffer = obj_ref(buffer);
		op->u.bind.offset = offset;
	}
}

static void
cmdbuf_set_buffer_offset(struct b2l_mtl_cmdbuf *cb, uint32_t stage, uint32_t index, uint64_t offset)
{
	struct op *op = cmdbuf_push(cb, OP_SET_BUFFER_OFFSET);
	if (op != NULL) {
		op->u.bind.stage = stage;
		op->u.bind.index = index;
		op->u.bind.offset = offset;
	}
}

/* setVertexBytes / setFragmentBytes: copied now (Metal allows 4 KB). */
static void
cmdbuf_set_bytes(struct b2l_mtl_cmdbuf *cb, uint32_t stage, uint32_t index, const void *bytes, uint64_t length)
{
	if (bytes == NULL || length == 0 || length > 65536) {
		cmdbuf_unsupported(cb, "setBytes: no bytes, or more than 64 KB");
		return;
	}
	uint8_t *copy = malloc((size_t)length);
	if (copy == NULL) {
		cmdbuf_unsupported(cb, "setBytes: out of memory");
		return;
	}
	memcpy(copy, bytes, (size_t)length);
	struct op *op = cmdbuf_push(cb, OP_SET_BYTES);
	if (op == NULL) {
		free(copy);
		return;
	}
	op->u.bind.stage = stage;
	op->u.bind.index = index;
	op->u.bind.bytes = copy;
	op->u.bind.length = (uint32_t)length;
}

static void
cmdbuf_set_texture(struct b2l_mtl_cmdbuf *cb, uint32_t stage, uint32_t index, struct b2l_mtl_texture *texture)
{
	struct op *op = cmdbuf_push(cb, OP_SET_TEXTURE);
	if (op != NULL) {
		op->u.resource.stage = stage;
		op->u.resource.index = index;
		op->u.resource.texture = obj_ref(texture);
	}
}

static void
cmdbuf_set_sampler(struct b2l_mtl_cmdbuf *cb, uint32_t stage, uint32_t index, struct b2l_mtl_sampler *sampler)
{
	struct op *op = cmdbuf_push(cb, OP_SET_SAMPLER);
	if (op != NULL) {
		op->u.resource.stage = stage;
		op->u.resource.index = index;
		op->u.resource.sampler = obj_ref(sampler);
	}
}

static void
cmdbuf_set_viewport(struct b2l_mtl_cmdbuf *cb, const struct b2l_mtl_viewport *viewport)
{
	struct op *op = cmdbuf_push(cb, OP_SET_VIEWPORT);
	if (op != NULL) {
		op->u.viewport = *viewport;
	}
}

static void
cmdbuf_set_scissor(struct b2l_mtl_cmdbuf *cb, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	struct op *op = cmdbuf_push(cb, OP_SET_SCISSOR);
	if (op != NULL) {
		op->u.scissor.x = x;
		op->u.scissor.y = y;
		op->u.scissor.width = width;
		op->u.scissor.height = height;
	}
}

static void
cmdbuf_set_raster(struct b2l_mtl_cmdbuf *cb, uint32_t which, uint32_t value)
{
	struct op *op = cmdbuf_push(cb, OP_SET_RASTER);
	if (op != NULL) {
		op->u.raster.which = which;
		op->u.raster.value = value;
	}
}

static void
cmdbuf_set_depth_stencil(struct b2l_mtl_cmdbuf *cb, struct b2l_mtl_depth_stencil *state)
{
	struct op *op = cmdbuf_push(cb, OP_SET_DEPTH_STENCIL);
	if (op != NULL) {
		op->u.dsa = obj_ref(state);
	}
}

static void
cmdbuf_set_stencil_ref(struct b2l_mtl_cmdbuf *cb, uint32_t front, uint32_t back)
{
	struct op *op = cmdbuf_push(cb, OP_SET_STENCIL_REF);
	if (op != NULL) {
		op->u.stencil_ref.front = front;
		op->u.stencil_ref.back = back;
	}
}

static void
cmdbuf_set_blend_color(struct b2l_mtl_cmdbuf *cb, const float rgba[4])
{
	struct op *op = cmdbuf_push(cb, OP_SET_BLEND_COLOR);
	if (op != NULL) {
		memcpy(op->u.blend_color, rgba, sizeof(op->u.blend_color));
	}
}

static void
cmdbuf_draw(struct b2l_mtl_cmdbuf *cb, const struct b2l_mtl_draw *draw)
{
	struct op *op = cmdbuf_push(cb, OP_DRAW);
	if (op != NULL) {
		op->u.draw = *draw;
		obj_ref(draw->index_buffer);
	}
}

/* ---- executing a render encoder ---- */

struct bound {
	struct b2l_mtl_buffer *buffer;          /* or ... */
	uint64_t offset;
	const uint8_t *bytes;                   /* ... setBytes's copy */
	uint32_t length;
};

/* The encoder's state while a command buffer runs; afresh at each pass.
 * Pointers only: the ops hold the references. */
struct encoder_state {
	bool in_pass;
	unsigned width, height, samples;
	struct b2l_mtl_render_pipeline *pipeline;
	struct bound buffers[2][B2L_MTL_MAX_BUFFERS];
	struct b2l_mtl_texture *textures[2][B2L_MTL_MAX_TEXTURES];
	struct b2l_mtl_sampler *samplers[2][B2L_MTL_MAX_SAMPLERS];
	struct b2l_mtl_viewport viewport;
	struct pipe_scissor_state scissor;
	uint32_t cull, winding, fill, depth_clip;
	struct b2l_mtl_depth_stencil *dsa;
	struct pipe_stencil_ref stencil_ref;
	struct pipe_blend_color blend_color;
	unsigned draws;
};

/* Metal's defaults for a new render encoder. */
static void
encoder_reset(struct encoder_state *st, unsigned width, unsigned height, unsigned samples)
{
	unsigned draws = st->draws;
	memset(st, 0, sizeof(*st));
	st->draws = draws;
	st->in_pass = true;
	st->width = width;
	st->height = height;
	st->samples = samples;
	st->viewport.width = width;
	st->viewport.height = height;
	st->viewport.zfar = 1.0;
	st->scissor.maxx = width;
	st->scissor.maxy = height;
}

static struct pipe_sampler_view *
texture_view(struct b2l_mtl_device *dev, struct b2l_mtl_texture *t)
{
	if (t->view == NULL) {
		struct pipe_sampler_view templ;
		u_sampler_view_default_template(&templ, t->res, t->format);
		t->view = dev->pipe->create_sampler_view(dev->pipe, t->res, &templ);
	}
	return t->view;
}

static void *
rasterizer(struct b2l_mtl_device *dev, const struct encoder_state *st, const struct b2l_mtl_render_pipeline *p)
{
	unsigned key = (st->cull & 3) | (st->winding & 1) << 2 | (st->fill & 1) << 3 | (st->depth_clip & 1) << 4 |
	    (st->samples > 1 ? 1u : 0u) << 5 | (p->point_size ? 1u : 0u) << 6 | (p->desc.rasterization ? 0u : 1u) << 7;
	if (dev->rasterizers[key] != NULL) {
		return dev->rasterizers[key];
	}
	struct pipe_rasterizer_state r;
	memset(&r, 0, sizeof(r));
	r.cull_face = (st->cull & 3) == 1 ? PIPE_FACE_FRONT : (st->cull & 3) == 2 ? PIPE_FACE_BACK : PIPE_FACE_NONE;
	/* Metal's clockwise is seen on a screen whose y grows downwards, as the
	 * Mali's rows do: the same turn as Gallium's counter-clockwise, a
	 * positive area in (x, row). MTLWindingClockwise is 0. */
	r.front_ccw = (st->winding & 1) == 0;
	r.fill_front = r.fill_back = (st->fill & 1) ? PIPE_POLYGON_MODE_LINE : PIPE_POLYGON_MODE_FILL;
	r.scissor = 1;                          /* Metal's scissor is always on */
	r.half_pixel_center = 1;
	r.bottom_edge_rule = 0;                 /* top-left rule, y down */
	r.flatshade_first = 1;                  /* Metal's provoking vertex is the first */
	r.clip_halfz = 1;                       /* NDC depth 0 to 1 */
	r.depth_clip_near = r.depth_clip_far = (st->depth_clip & 1) == 0;
	r.depth_clamp = (st->depth_clip & 1) != 0;
	r.multisample = st->samples > 1;
	r.point_quad_rasterization = 1;
	r.sprite_coord_mode = PIPE_SPRITE_COORD_UPPER_LEFT;
	r.point_size_per_vertex = p->point_size;
	r.point_size = 1.0f;
	r.line_width = 1.0f;
	r.rasterizer_discard = !p->desc.rasterization;
	dev->rasterizers[key] = dev->pipe->create_rasterizer_state(dev->pipe, &r);
	return dev->rasterizers[key];
}

/* MTLViewport to Gallium's: NDC +y is up in Metal and row 0 is the top, so
 * the y scale is negative; depth 0 to 1 goes to znear..zfar. */
static void
viewport_state(const struct b2l_mtl_viewport *v, struct pipe_viewport_state *out)
{
	memset(out, 0, sizeof(*out));
	out->scale[0] = (float)(v->width * 0.5);
	out->scale[1] = (float)(-v->height * 0.5);
	out->scale[2] = (float)(v->zfar - v->znear);
	out->translate[0] = (float)(v->x + v->width * 0.5);
	out->translate[1] = (float)(v->y + v->height * 0.5);
	out->translate[2] = (float)v->znear;
	out->swizzle_x = PIPE_VIEWPORT_SWIZZLE_POSITIVE_X;
	out->swizzle_y = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Y;
	out->swizzle_z = PIPE_VIEWPORT_SWIZZLE_POSITIVE_Z;
	out->swizzle_w = PIPE_VIEWPORT_SWIZZLE_POSITIVE_W;
}

/* The vertex buffers the pipeline's elements read, at Metal's indices. */
static void
bind_vertex(struct b2l_mtl_device *dev, const struct encoder_state *st, const struct b2l_mtl_render_pipeline *p)
{
	struct pipe_context *pipe = dev->pipe;
	pipe->bind_vertex_elements_state(pipe, p->velems != NULL ? p->velems : dev->default_velems);
	uint32_t mask = p->velems != NULL ? p->vb_mask : 1u;
	unsigned count = p->velems != NULL ? p->vb_count : 1;
	struct pipe_vertex_buffer vb[B2L_MTL_MAX_BUFFERS];
	memset(vb, 0, sizeof(vb));
	for (unsigned i = 0; i < count && i < B2L_MTL_MAX_BUFFERS; i++) {
		if ((mask & (1u << i)) == 0) {
			continue;
		}
		const struct bound *b = &st->buffers[B2L_MTL_VERTEX][i];
		if (b->bytes != NULL) {
			u_upload_data(pipe->stream_uploader, 0, b->length, 16, b->bytes, &vb[i].buffer_offset,
			    &vb[i].buffer.resource);
		} else if (b->buffer != NULL && b->offset < b->buffer->length) {
			pipe_resource_reference(&vb[i].buffer.resource, b->buffer->res);
			vb[i].buffer_offset = (unsigned)b->offset;
		} else {
			pipe_resource_reference(&vb[i].buffer.resource, dev->zero_buffer);
		}
	}
	/* the driver takes these references (p_context.h, set_vertex_buffers) */
	pipe->set_vertex_buffers(pipe, MIN2(count, B2L_MTL_MAX_BUFFERS), vb);
}

/* One stage's buffers, textures and samplers; a zero stand-in for what the
 * shader reads and the encoder did not bind. */
static void
bind_stage(struct b2l_mtl_device *dev, const struct encoder_state *st, enum pipe_shader_type shader, unsigned s,
    const struct stage_bindings *b)
{
	struct pipe_context *pipe = dev->pipe;
	for (unsigned i = 0; i < b->ubos; i++) {
		const struct bound *bb = &st->buffers[s][i];
		struct pipe_constant_buffer c;
		memset(&c, 0, sizeof(c));
		if (bb->bytes != NULL) {
			c.user_buffer = bb->bytes;
			c.buffer_size = bb->length;
		} else if (bb->buffer != NULL && bb->offset < bb->buffer->length) {
			c.buffer = bb->buffer->res;
			c.buffer_offset = (unsigned)bb->offset;
			c.buffer_size = (unsigned)MIN2(bb->buffer->length - bb->offset, 65536);
		} else {
			c.buffer = dev->zero_buffer;
			c.buffer_size = ZERO_BYTES;
		}
		pipe->set_constant_buffer(pipe, shader, i, false, &c);
	}
	unsigned n = util_last_bit(b->ssbo_mask);
	if (n != 0) {
		struct pipe_shader_buffer sb[B2L_MTL_MAX_BUFFERS];
		memset(sb, 0, sizeof(sb));
		for (unsigned i = 0; i < n; i++) {
			const struct bound *bb = &st->buffers[s][i];
			if (bb->buffer != NULL && bb->offset < bb->buffer->length) {
				sb[i].buffer = bb->buffer->res;
				sb[i].buffer_offset = (unsigned)bb->offset;
				sb[i].buffer_size = (unsigned)(bb->buffer->length - bb->offset);
			} else {
				sb[i].buffer = dev->zero_buffer;
				sb[i].buffer_size = ZERO_BYTES;
			}
		}
		pipe->set_shader_buffers(pipe, shader, 0, n, sb, b->ssbo_written);
	}
	n = util_last_bit(b->texture_mask);
	if (n != 0) {
		struct pipe_sampler_view *views[B2L_MTL_MAX_TEXTURES];
		for (unsigned i = 0; i < n; i++) {
			struct b2l_mtl_texture *t = st->textures[s][i];
			views[i] = (b->null_texture_mask & (1u << i)) != 0 || t == NULL ? dev->zero_view : texture_view(dev, t);
			if (views[i] == NULL) {
				views[i] = dev->zero_view;
			}
		}
		pipe->set_sampler_views(pipe, shader, 0, n, 0, false, views);
	}
	n = util_last_bit(b->sampler_mask);
	if (n != 0) {
		void *states[B2L_MTL_MAX_SAMPLERS];
		for (unsigned i = 0; i < n; i++) {
			struct b2l_mtl_sampler *bound_sampler = st->samplers[s][i];
			states[i] = b->inline_samplers[i] != NULL ? b->inline_samplers[i] :
			    bound_sampler != NULL ? bound_sampler->cso : dev->default_sampler;
		}
		pipe->bind_sampler_states(pipe, shader, 0, n, states);
	}
}

/* The lowest and highest index a draw reads, from the buffer's mapping (the
 * CPU wrote it), so that Panfrost needs no index cache of its own. */
static bool
index_bounds(const struct b2l_mtl_draw *d, unsigned size, bool restart, unsigned *min, unsigned *max)
{
	const struct b2l_mtl_buffer *b = d->index_buffer;
	if (d->index_offset % size != 0 || d->index_offset + (uint64_t)d->count * size > b->length) {
		return false;
	}
	const uint8_t *p = b->map + d->index_offset;
	uint32_t lo = UINT32_MAX, hi = 0, restart_index = size == 2 ? 0xffffu : UINT32_MAX;
	for (uint32_t i = 0; i < d->count; i++) {
		uint32_t v = size == 2 ? ((const uint16_t *)p)[i] : ((const uint32_t *)p)[i];
		if (restart && v == restart_index) {
			continue;
		}
		lo = MIN2(lo, v);
		hi = MAX2(hi, v);
	}
	if (lo > hi) {
		lo = hi = 0;
	}
	*min = lo;
	*max = hi;
	return true;
}

static void
exec_draw(struct b2l_mtl_device *dev, struct encoder_state *st, const struct b2l_mtl_draw *d, char *first,
    size_t first_size)
{
	struct pipe_context *pipe = dev->pipe;
	const struct b2l_mtl_render_pipeline *p = st->pipeline;
	const char *problem = !st->in_pass ? "a draw outside a render pass" :
	    p == NULL ? "a draw with no render pipeline state" : NULL;
	enum mesa_prim mode = MESA_PRIM_TRIANGLES;
	switch (d->primitive) {
	case 0: mode = MESA_PRIM_POINTS; break;
	case 1: mode = MESA_PRIM_LINES; break;
	case 2: mode = MESA_PRIM_LINE_STRIP; break;
	case 3: mode = MESA_PRIM_TRIANGLES; break;
	case 4: mode = MESA_PRIM_TRIANGLE_STRIP; break;
	default:
		problem = "a primitive type Metal does not have";
		break;
	}
	if (problem != NULL) {
		if (first != NULL && first[0] == 0) {
			say(first, first_size, "%s", problem);
		}
		return;
	}
	if (d->count == 0 || d->instances == 0) {
		return;
	}
	struct pipe_draw_info info;
	memset(&info, 0, sizeof(info));
	info.mode = mode;
	info.instance_count = d->instances;
	info.start_instance = d->base_instance;
	struct pipe_draw_start_count_bias draw = { d->start, d->count, 0 };
	if (d->index_buffer != NULL) {
		unsigned size = d->index_type == 1 ? 4 : 2;
		bool strip = mode == MESA_PRIM_LINE_STRIP || mode == MESA_PRIM_TRIANGLE_STRIP;
		unsigned lo = 0, hi = 0;
		if (!index_bounds(d, size, strip, &lo, &hi)) {
			if (first != NULL && first[0] == 0) {
				say(first, first_size, "drawIndexedPrimitives: indices outside the index buffer");
			}
			return;
		}
		info.index_size = (uint16_t)size;
		info.index.resource = d->index_buffer->res;
		info.index_bounds_valid = true;
		info.min_index = lo;
		info.max_index = hi;
		/* Metal restarts indexed strips at the all-ones index, always */
		info.primitive_restart = strip;
		info.restart_index = size == 2 ? 0xffffu : UINT32_MAX;
		draw.start = (unsigned)(d->index_offset / size);
		draw.index_bias = d->base_vertex;
	}
	pipe->bind_vs_state(pipe, p->vs);
	pipe->bind_fs_state(pipe, p->fs);
	pipe->bind_blend_state(pipe, p->blend);
	pipe->bind_depth_stencil_alpha_state(pipe, st->dsa != NULL ? st->dsa->cso : dev->default_dsa);
	pipe->set_stencil_ref(pipe, st->stencil_ref);
	pipe->set_blend_color(pipe, &st->blend_color);
	pipe->bind_rasterizer_state(pipe, rasterizer(dev, st, p));
	struct pipe_viewport_state viewport;
	viewport_state(&st->viewport, &viewport);
	pipe->set_viewport_states(pipe, 0, 1, &viewport);
	pipe->set_scissor_states(pipe, 0, 1, &st->scissor);
	bind_vertex(dev, st, p);
	bind_stage(dev, st, PIPE_SHADER_VERTEX, B2L_MTL_VERTEX, &p->bind[B2L_MTL_VERTEX]);
	if (p->fs != NULL) {
		bind_stage(dev, st, PIPE_SHADER_FRAGMENT, B2L_MTL_FRAGMENT, &p->bind[B2L_MTL_FRAGMENT]);
	}
	u_upload_unmap(pipe->stream_uploader);
	pipe->draw_vbo(pipe, &info, 0, NULL, &draw, 1);
	st->draws++;
	dev->pending = true;
}

/* After a command buffer: nothing of its stays bound in the context, so a
 * state it held may be deleted (Panfrost links the bound vertex and
 * fragment shaders whenever either is bound again). */
static void
unbind_all(struct b2l_mtl_device *dev)
{
	struct pipe_context *pipe = dev->pipe;
	static const enum pipe_shader_type stages[2] = { PIPE_SHADER_VERTEX, PIPE_SHADER_FRAGMENT };
	void *no_samplers[B2L_MTL_MAX_SAMPLERS];
	memset(no_samplers, 0, sizeof(no_samplers));
	pipe->bind_fs_state(pipe, NULL);
	pipe->bind_vs_state(pipe, NULL);
	pipe->bind_blend_state(pipe, dev->default_blend);
	pipe->bind_depth_stencil_alpha_state(pipe, dev->default_dsa);
	pipe->bind_vertex_elements_state(pipe, dev->default_velems);
	pipe->set_vertex_buffers(pipe, 0, NULL);
	for (unsigned s = 0; s < 2; s++) {
		for (unsigned i = 0; i < B2L_MTL_MAX_BUFFERS; i++) {
			pipe->set_constant_buffer(pipe, stages[s], i, false, NULL);
		}
		pipe->set_shader_buffers(pipe, stages[s], 0, B2L_MTL_MAX_BUFFERS, NULL, 0);
		pipe->set_sampler_views(pipe, stages[s], 0, 0, B2L_MTL_MAX_TEXTURES, false, NULL);
		pipe->bind_sampler_states(pipe, stages[s], 0, B2L_MTL_MAX_SAMPLERS, no_samplers);
	}
}

/* An encoder state command into `st` (the stage and index checked). */
static void
exec_state(struct encoder_state *st, const struct op *op)
{
	switch (op->kind) {
	case OP_SET_PIPELINE:
		st->pipeline = op->u.pipeline;
		break;
	case OP_SET_BUFFER:
	case OP_SET_BUFFER_OFFSET:
	case OP_SET_BYTES:
		if (op->u.bind.stage < 2 && op->u.bind.index < B2L_MTL_MAX_BUFFERS) {
			struct bound *b = &st->buffers[op->u.bind.stage][op->u.bind.index];
			if (op->kind == OP_SET_BUFFER_OFFSET) {
				b->offset = op->u.bind.offset;
			} else {
				b->buffer = op->u.bind.buffer;
				b->offset = op->u.bind.offset;
				b->bytes = op->u.bind.bytes;
				b->length = op->u.bind.length;
			}
		}
		break;
	case OP_SET_TEXTURE:
		if (op->u.resource.stage < 2 && op->u.resource.index < B2L_MTL_MAX_TEXTURES) {
			st->textures[op->u.resource.stage][op->u.resource.index] = op->u.resource.texture;
		}
		break;
	case OP_SET_SAMPLER:
		if (op->u.resource.stage < 2 && op->u.resource.index < B2L_MTL_MAX_SAMPLERS) {
			st->samplers[op->u.resource.stage][op->u.resource.index] = op->u.resource.sampler;
		}
		break;
	case OP_SET_VIEWPORT:
		st->viewport = op->u.viewport;
		break;
	case OP_SET_SCISSOR: {
		uint32_t x = MIN2(op->u.scissor.x, st->width), y = MIN2(op->u.scissor.y, st->height);
		st->scissor.minx = x;
		st->scissor.miny = y;
		st->scissor.maxx = (unsigned)MIN2((uint64_t)op->u.scissor.x + op->u.scissor.width, st->width);
		st->scissor.maxy = (unsigned)MIN2((uint64_t)op->u.scissor.y + op->u.scissor.height, st->height);
		break;
	}
	case OP_SET_RASTER:
		switch (op->u.raster.which) {
		case B2L_MTL_CULL: st->cull = op->u.raster.value; break;
		case B2L_MTL_WINDING: st->winding = op->u.raster.value; break;
		case B2L_MTL_FILL: st->fill = op->u.raster.value; break;
		case B2L_MTL_DEPTH_CLIP: st->depth_clip = op->u.raster.value; break;
		default: break;
		}
		break;
	case OP_SET_DEPTH_STENCIL:
		st->dsa = op->u.dsa;
		break;
	case OP_SET_STENCIL_REF:
		st->stencil_ref.ref_value[0] = (uint8_t)op->u.stencil_ref.front;
		st->stencil_ref.ref_value[1] = (uint8_t)op->u.stencil_ref.back;
		break;
	case OP_SET_BLEND_COLOR:
		memcpy(st->blend_color.color, op->u.blend_color, sizeof(st->blend_color.color));
		break;
	default:
		break;
	}
}

static struct pipe_surface *
pass_surface(struct b2l_mtl_cmdbuf *cb, const struct b2l_mtl_attachment *a)
{
	struct b2l_mtl_texture *t = a->texture;
	if (t == NULL || a->level > t->res->last_level) {
		return NULL;
	}
	struct pipe_surface templ;
	memset(&templ, 0, sizeof(templ));
	templ.format = t->format;
	templ.u.tex.level = a->level;
	templ.u.tex.first_layer = templ.u.tex.last_layer = texture_layer(t, a->slice, a->depth_plane);
	struct pipe_surface *s = cb->dev->pipe->create_surface(cb->dev->pipe, t->res, &templ);
	if (s != NULL) {
		util_dynarray_append(&cb->surfaces, struct pipe_surface *, s);
	}
	return s;
}

static void
exec_pass_begin(struct b2l_mtl_cmdbuf *cb, struct encoder_state *st, const struct b2l_mtl_pass_desc *pass,
    char *why, size_t why_size)
{
	struct pipe_context *pipe = cb->dev->pipe;
	struct pipe_framebuffer_state fb;
	memset(&fb, 0, sizeof(fb));
	unsigned width = pass->width, height = pass->height, samples = 0;
	for (unsigned i = 0; i < B2L_MTL_MAX_COLORS; i++) {
		const struct b2l_mtl_attachment *a = &pass->color[i];
		if (a->texture == NULL) {
			continue;
		}
		fb.cbufs[i] = pass_surface(cb, a);
		if (fb.cbufs[i] == NULL) {
			say(why, why_size, "colour attachment %u: no surface", i);
			continue;
		}
		fb.nr_cbufs = i + 1;
		unsigned w = u_minify(a->texture->res->width0, a->level), h = u_minify(a->texture->res->height0, a->level);
		width = width != 0 ? MIN2(width, w) : w;
		height = height != 0 ? MIN2(height, h) : h;
		samples = MAX2(samples, a->texture->res->nr_samples);
	}
	const struct b2l_mtl_attachment *zs = pass->depth.texture != NULL ? &pass->depth : &pass->stencil;
	if (zs->texture != NULL) {
		fb.zsbuf = pass_surface(cb, zs);
		unsigned w = u_minify(zs->texture->res->width0, zs->level), h = u_minify(zs->texture->res->height0, zs->level);
		width = width != 0 ? MIN2(width, w) : w;
		height = height != 0 ? MIN2(height, h) : h;
	}
	fb.width = (uint16_t)width;
	fb.height = (uint16_t)height;
	fb.layers = 1;
	fb.samples = (uint8_t)MAX2(samples, 1);
	pipe->set_framebuffer_state(pipe, &fb);
	for (unsigned i = 0; i < fb.nr_cbufs; i++) {
		const struct b2l_mtl_attachment *a = &pass->color[i];
		if (fb.cbufs[i] == NULL || a->load != 2) {
			continue;
		}
		union pipe_color_union color;
		memset(&color, 0, sizeof(color));
		bool is_sint = util_format_is_pure_sint(a->texture->format);
		bool is_uint = util_format_is_pure_uint(a->texture->format);
		for (unsigned c = 0; c < 4; c++) {
			if (is_sint) {
				color.i[c] = (int32_t)a->clear[c];
			} else if (is_uint) {
				color.ui[c] = (uint32_t)a->clear[c];
			} else {
				color.f[c] = (float)a->clear[c];
			}
		}
		/* one buffer at a time: Panfrost keeps a clear colour per target */
		pipe->clear(pipe, PIPE_CLEAR_COLOR0 << i, NULL, &color, 0.0, 0);
		cb->dev->pending = true;
	}
	if (fb.zsbuf != NULL) {
		unsigned buffers = 0;
		if (pass->depth.texture != NULL && pass->depth.load == 2) {
			buffers |= PIPE_CLEAR_DEPTH;
		}
		if (pass->stencil.texture != NULL && pass->stencil.load == 2) {
			buffers |= PIPE_CLEAR_STENCIL;
		}
		if (buffers != 0) {
			union pipe_color_union none;
			memset(&none, 0, sizeof(none));
			pipe->clear(pipe, buffers, NULL, &none, pass->depth.clear[0], pass->stencil.clear_stencil & 0xff);
			cb->dev->pending = true;
		}
	}
	encoder_reset(st, width, height, MAX2(samples, 1));
}

static void
exec_buffer_texture(struct b2l_mtl_cmdbuf *cb, const struct op *op)
{
	struct b2l_mtl_texture *t = op->u.buffer_texture.texture;
	struct b2l_mtl_buffer *b = op->u.buffer_texture.buffer;
	uint32_t level = op->u.buffer_texture.level;
	if (t == NULL || b == NULL || level > t->res->last_level) {
		return;
	}
	struct pipe_box box;
	texture_box(t, &op->u.buffer_texture.region, op->u.buffer_texture.slice, &box);
	uint64_t row = op->u.buffer_texture.bytes_per_row;
	uint64_t image = op->u.buffer_texture.bytes_per_image != 0 ? op->u.buffer_texture.bytes_per_image :
	    row * (uint64_t)box.height;
	uint64_t need = op->u.buffer_texture.offset + image * (uint64_t)box.depth;
	if (need > b->length + 16) {
		return;
	}
	/* the buffer is read or written by the CPU here, through its mapping */
	settle(cb->dev);
	uint8_t *bytes = b->map + op->u.buffer_texture.offset;
	if (op->kind == OP_BUFFER_TO_TEXTURE) {
		cb->dev->pipe->texture_subdata(cb->dev->pipe, t->res, level, PIPE_MAP_WRITE, &box, bytes, (unsigned)row,
		    (uintptr_t)image);
	} else {
		texture_read_locked(t, &box, level, bytes, row, image);
	}
}

static int
cmdbuf_execute(struct b2l_mtl_cmdbuf *cb, uint64_t *gpu_us, char *why, size_t why_size)
{
	if (cb == NULL || cb->executed) {
		say(why, why_size, "%s", cb == NULL ? "no command buffer" : "already executed");
		return -1;
	}
	cb->executed = true;
	struct b2l_mtl_device *dev = cb->dev;
	struct pipe_context *pipe = dev->pipe;
	char first[160] = "";
	struct encoder_state *st = calloc(1, sizeof(*st));
	if (st == NULL) {
		say(why, why_size, "out of memory");
		return -1;
	}
	pthread_mutex_lock(&dev->lock);
	uint64_t t0 = now_us();
	util_dynarray_foreach(&cb->ops, struct op, op) {
		struct pipe_box box;
		switch (op->kind) {
		case OP_PASS_BEGIN:
			exec_pass_begin(cb, st, &op->u.pass, first[0] == 0 ? first : NULL, sizeof(first));
			break;
		case OP_PASS_END:
			st->in_pass = false;
			break;
		case OP_SET_PIPELINE:
		case OP_SET_BUFFER:
		case OP_SET_BUFFER_OFFSET:
		case OP_SET_BYTES:
		case OP_SET_TEXTURE:
		case OP_SET_SAMPLER:
		case OP_SET_VIEWPORT:
		case OP_SET_SCISSOR:
		case OP_SET_RASTER:
		case OP_SET_DEPTH_STENCIL:
		case OP_SET_STENCIL_REF:
		case OP_SET_BLEND_COLOR:
			exec_state(st, op);
			break;
		case OP_DRAW:
			exec_draw(dev, st, &op->u.draw, first, sizeof(first));
			break;
		case OP_COPY_BUFFER:
			if (op->u.copy_buffer.from_offset + op->u.copy_buffer.size > op->u.copy_buffer.from->length ||
			    op->u.copy_buffer.to_offset + op->u.copy_buffer.size > op->u.copy_buffer.to->length) {
				if (first[0] == 0) {
					say(first, sizeof(first), "copyFromBuffer: outside a buffer");
				}
				break;
			}
			u_box_1d((unsigned)op->u.copy_buffer.from_offset, (unsigned)op->u.copy_buffer.size, &box);
			pipe->resource_copy_region(pipe, op->u.copy_buffer.to->res, 0, (unsigned)op->u.copy_buffer.to_offset,
			    0, 0, op->u.copy_buffer.from->res, 0, &box);
			break;
		case OP_FILL_BUFFER:
			if (op->u.fill.offset + op->u.fill.length > op->u.fill.buffer->length) {
				if (first[0] == 0) {
					say(first, sizeof(first), "fillBuffer: outside the buffer");
				}
				break;
			}
			pipe->clear_buffer(pipe, op->u.fill.buffer->res, (unsigned)op->u.fill.offset,
			    (unsigned)op->u.fill.length, &op->u.fill.value, 1);
			break;
		case OP_COPY_TEXTURE: {
			struct b2l_mtl_texture *from = op->u.copy_texture.from, *to = op->u.copy_texture.to;
			texture_box(from, &op->u.copy_texture.region, op->u.copy_texture.from_slice, &box);
			unsigned z = to->res->target == PIPE_TEXTURE_3D ? op->u.copy_texture.to_z : op->u.copy_texture.to_slice;
			pipe->resource_copy_region(pipe, to->res, op->u.copy_texture.to_level, op->u.copy_texture.to_x,
			    op->u.copy_texture.to_y, z, from->res, op->u.copy_texture.from_level, &box);
			break;
		}
		case OP_BUFFER_TO_TEXTURE:
		case OP_TEXTURE_TO_BUFFER:
			exec_buffer_texture(cb, op);
			break;
		case OP_UNSUPPORTED:
			if (first[0] == 0) {
				say(first, sizeof(first), "not implemented yet: %s", op->u.what);
			}
			break;
		}
	}
	settle(dev);
	if (st->draws != 0) {
		unbind_all(dev);
	}
	free(st);
	/* Let go of the surfaces: the context keeps its own references in its
	 * framebuffer state until it is replaced. */
	struct pipe_framebuffer_state none;
	memset(&none, 0, sizeof(none));
	pipe->set_framebuffer_state(pipe, &none);
	util_dynarray_foreach(&cb->surfaces, struct pipe_surface *, s) {
		pipe_surface_reference(s, NULL);
	}
	util_dynarray_clear(&cb->surfaces);
	uint64_t took = now_us() - t0;
	pthread_mutex_unlock(&dev->lock);
	if (gpu_us != NULL) {
		*gpu_us = took;
	}
	if (first[0] != 0) {
		say(why, why_size, "%s", first);
		return 1;
	}
	return 0;
}

static void
cmdbuf_release(struct b2l_mtl_cmdbuf *cb)
{
	obj_unref(cb);
}

/* ---- the table ---- */

static const struct b2l_mtl_api api = {
	.version = B2L_MTL_API_VERSION,
	.device_open = device_open,
	.device_max_texture_2d = device_max_texture_2d,
	.buffer_new = buffer_new,
	.buffer_contents = buffer_contents,
	.buffer_length = buffer_length,
	.buffer_release = buffer_release,
	.texture_new = texture_new,
	.texture_from_iosurface = texture_from_iosurface,
	.texture_replace = texture_replace,
	.texture_get = texture_get,
	.texture_allocated_size = texture_allocated_size,
	.texture_release = texture_release,
	.sampler_new = sampler_new,
	.sampler_release = sampler_release,
	.depth_stencil_new = depth_stencil_new,
	.depth_stencil_release = depth_stencil_release,
	.library_open = library_open,
	.library_new_data = library_new_data,
	.library_count = library_count,
	.library_name = library_name,
	.library_find = library_find,
	.library_release = library_release,
	.function_new = function_new,
	.function_stage = function_stage,
	.function_name = function_name,
	.function_release = function_release,
	.render_pipeline_new = render_pipeline_new,
	.render_pipeline_release = render_pipeline_release,
	.cmdbuf_new = cmdbuf_new,
	.cmdbuf_pass_begin = cmdbuf_pass_begin,
	.cmdbuf_pass_end = cmdbuf_pass_end,
	.cmdbuf_copy_buffer = cmdbuf_copy_buffer,
	.cmdbuf_fill_buffer = cmdbuf_fill_buffer,
	.cmdbuf_copy_texture = cmdbuf_copy_texture,
	.cmdbuf_copy_buffer_to_texture = cmdbuf_copy_buffer_to_texture,
	.cmdbuf_copy_texture_to_buffer = cmdbuf_copy_texture_to_buffer,
	.cmdbuf_unsupported = cmdbuf_unsupported,
	.cmdbuf_execute = cmdbuf_execute,
	.cmdbuf_release = cmdbuf_release,
	.cmdbuf_set_pipeline = cmdbuf_set_pipeline,
	.cmdbuf_set_buffer = cmdbuf_set_buffer,
	.cmdbuf_set_buffer_offset = cmdbuf_set_buffer_offset,
	.cmdbuf_set_bytes = cmdbuf_set_bytes,
	.cmdbuf_set_texture = cmdbuf_set_texture,
	.cmdbuf_set_sampler = cmdbuf_set_sampler,
	.cmdbuf_set_viewport = cmdbuf_set_viewport,
	.cmdbuf_set_scissor = cmdbuf_set_scissor,
	.cmdbuf_set_raster = cmdbuf_set_raster,
	.cmdbuf_set_depth_stencil = cmdbuf_set_depth_stencil,
	.cmdbuf_set_stencil_ref = cmdbuf_set_stencil_ref,
	.cmdbuf_set_blend_color = cmdbuf_set_blend_color,
	.cmdbuf_draw = cmdbuf_draw,
};

B2L_EXPORT const struct b2l_mtl_api *
b2l_mtl_api(uint32_t version)
{
	return version == B2L_MTL_API_VERSION ? &api : NULL;
}
