// SPDX-License-Identifier: MIT
/*
 * S10 port: the C interface between B2LMetal.bundle (userspace/b2l-metal.c,
 * Objective-C on the runtime) and libb2l-mesa (b2l_mtl.c, Gallium over
 * Panfrost). Metal milestone M2 (docs/metal-on-panfrost.md).
 *
 * The plug-in reads Metal's own descriptor objects and fills these plain
 * structures; libb2l-mesa never sees an Objective-C object. One dlsym:
 * b2l_mtl_api() gives the table below. Enumerations are Metal's own numbers
 * (MTLPixelFormat, MTLLoadAction, ...), converted on the Mesa side.
 *
 * Objects are reference counted: every *_new gives one reference, *_release
 * drops one, and a command buffer holds its own on what it records.
 * Everything a device does on the GPU is serialised under that device's lock
 * (one Gallium context per device).
 *
 * Version 2 (M2b): the render encoder's state and draws.
 */
#ifndef B2L_MTL_H
#define B2L_MTL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define B2L_MTL_API_VERSION 2

#define B2L_MTL_MAX_COLORS     8
#define B2L_MTL_MAX_ATTRIBUTES 31
#define B2L_MTL_MAX_BUFFERS    31
#define B2L_MTL_MAX_TEXTURES   32
#define B2L_MTL_MAX_SAMPLERS   16

/* Shader stages of the encoder's set* calls. */
#define B2L_MTL_VERTEX   0
#define B2L_MTL_FRAGMENT 1

/* cmdbuf_set_raster's `which`: MTLCullMode, MTLWinding, MTLTriangleFillMode,
 * MTLDepthClipMode. */
#define B2L_MTL_CULL       0
#define B2L_MTL_WINDING    1
#define B2L_MTL_FILL       2
#define B2L_MTL_DEPTH_CLIP 3

struct b2l_mtl_device;
struct b2l_mtl_buffer;
struct b2l_mtl_texture;
struct b2l_mtl_sampler;
struct b2l_mtl_depth_stencil;
struct b2l_mtl_library;
struct b2l_mtl_function;
struct b2l_mtl_render_pipeline;
struct b2l_mtl_cmdbuf;

struct b2l_mtl_texture_desc {
	uint32_t type;                  /* MTLTextureType */
	uint32_t pixel_format;          /* MTLPixelFormat */
	uint32_t width, height, depth;
	uint32_t mip_levels, array_length, sample_count;
	uint32_t usage;                 /* MTLTextureUsage */
	uint32_t storage_mode;          /* MTLStorageMode */
};

struct b2l_mtl_region {
	uint32_t x, y, z, width, height, depth;
};

struct b2l_mtl_sampler_desc {
	uint32_t min_filter, mag_filter, mip_filter;    /* MTLSamplerMinMagFilter, MTLSamplerMipFilter */
	uint32_t address[3];                            /* MTLSamplerAddressMode s, t, r */
	uint32_t compare;                               /* MTLCompareFunction */
	uint32_t border;                                /* MTLSamplerBorderColor */
	uint32_t max_anisotropy;
	bool normalized;
	float lod_min, lod_max;
};

struct b2l_mtl_stencil_desc {
	uint32_t compare, fail, depth_fail, pass;       /* MTLCompareFunction, MTLStencilOperation */
	uint32_t read_mask, write_mask;
};

struct b2l_mtl_depth_stencil_desc {
	uint32_t depth_compare;                         /* MTLCompareFunction */
	bool depth_write;
	bool stencil;                                   /* either face has a stencil descriptor */
	struct b2l_mtl_stencil_desc front, back;
};

/* A function constant's value as Metal gives it: by index or by name, its
 * MTLDataType and the bytes of its value (at most 8 are used). */
struct b2l_mtl_constant {
	int32_t index;                  /* -1: given by name */
	const char *name;
	uint32_t data_type;             /* MTLDataType */
	uint8_t bytes[16];
	uint32_t size;
};

struct b2l_mtl_color_desc {
	uint32_t pixel_format;          /* MTLPixelFormatInvalid (0): no attachment */
	bool blend;
	uint32_t rgb_op, alpha_op;      /* MTLBlendOperation */
	uint32_t src_rgb, dst_rgb, src_alpha, dst_alpha;        /* MTLBlendFactor */
	uint32_t write_mask;            /* MTLColorWriteMask */
};

struct b2l_mtl_render_pipeline_desc {
	struct b2l_mtl_function *vertex, *fragment;
	struct b2l_mtl_color_desc color[B2L_MTL_MAX_COLORS];
	uint32_t depth_format, stencil_format;          /* MTLPixelFormat */
	uint32_t sample_count;
	uint32_t topology_class;                        /* MTLPrimitiveTopologyClass */
	bool alpha_to_coverage;
	bool rasterization;
	bool vertex_descriptor;                         /* the descriptor gave one */
	struct {
		uint32_t format;                        /* MTLVertexFormat; 0: not described */
		uint32_t offset, buffer;
	} attribute[B2L_MTL_MAX_ATTRIBUTES];
	struct {
		uint32_t stride, step_function, step_rate;
	} layout[B2L_MTL_MAX_BUFFERS];
};

/* What making a pipeline took, for the log. */
struct b2l_mtl_pipeline_report {
	uint32_t translate_ms;          /* AIR to NIR, for the functions not yet translated */
	uint32_t compile_ms;            /* Panfrost's shader CSOs (Bifrost, default variant) */
	uint32_t vs_instructions, fs_instructions;      /* NIR after the frontend's clean-up */
	uint32_t varyings_linked, varyings_unmatched;   /* fragment inputs matched by name */
	uint32_t attributes;                            /* vertex elements made */
};

struct b2l_mtl_attachment {
	struct b2l_mtl_texture *texture;        /* NULL: none */
	uint32_t level, slice, depth_plane;
	uint32_t load, store;                   /* MTLLoadAction, MTLStoreAction */
	double clear[4];                        /* colour; clear[0] is the depth of a depth attachment */
	uint32_t clear_stencil;
};

struct b2l_mtl_pass_desc {
	struct b2l_mtl_attachment color[B2L_MTL_MAX_COLORS];
	struct b2l_mtl_attachment depth, stencil;
	uint32_t width, height;                 /* renderTargetWidth/Height, 0 when not set */
};

/* MTLViewport: the origin at the top left, in pixels; depth near and far. */
struct b2l_mtl_viewport {
	double x, y, width, height, znear, zfar;
};

/* A draw: MTLPrimitiveType; vertices start..start+count, or count indices of
 * index_buffer (MTLIndexType 0 = 16-bit, 1 = 32-bit) from index_offset. */
struct b2l_mtl_draw {
	uint32_t primitive;
	uint32_t start, count, instances, base_instance;
	int32_t base_vertex;
	struct b2l_mtl_buffer *index_buffer;    /* NULL: not indexed */
	uint32_t index_type;
	uint64_t index_offset;
};

struct b2l_mtl_api {
	uint32_t version;

	/* The device: Panfrost's screen (the one the EAGL layer uses in its
	 * process) and a context of its own. NULL with the reason in why. */
	struct b2l_mtl_device *(*device_open)(char *why, size_t why_size);
	uint32_t (*device_max_texture_2d)(struct b2l_mtl_device *device);

	/* Buffers: GPU memory mapped once for the CPU, for good (contents). */
	struct b2l_mtl_buffer *(*buffer_new)(struct b2l_mtl_device *device, uint64_t length, const void *bytes,
	    char *why, size_t why_size);
	void *(*buffer_contents)(struct b2l_mtl_buffer *buffer);
	uint64_t (*buffer_length)(struct b2l_mtl_buffer *buffer);
	void (*buffer_release)(struct b2l_mtl_buffer *buffer);

	/* Textures; from an IOSurface (IOSurfaceRef) the surface's own memory. */
	struct b2l_mtl_texture *(*texture_new)(struct b2l_mtl_device *device, const struct b2l_mtl_texture_desc *desc,
	    char *why, size_t why_size);
	struct b2l_mtl_texture *(*texture_from_iosurface)(struct b2l_mtl_device *device,
	    const struct b2l_mtl_texture_desc *desc, void *iosurface, uint32_t plane, char *why, size_t why_size);
	void (*texture_replace)(struct b2l_mtl_texture *texture, const struct b2l_mtl_region *region, uint32_t level,
	    uint32_t slice, const void *bytes, uint64_t bytes_per_row, uint64_t bytes_per_image);
	void (*texture_get)(struct b2l_mtl_texture *texture, const struct b2l_mtl_region *region, uint32_t level,
	    uint32_t slice, void *bytes, uint64_t bytes_per_row, uint64_t bytes_per_image);
	uint64_t (*texture_allocated_size)(struct b2l_mtl_texture *texture);
	void (*texture_release)(struct b2l_mtl_texture *texture);

	struct b2l_mtl_sampler *(*sampler_new)(struct b2l_mtl_device *device, const struct b2l_mtl_sampler_desc *desc);
	void (*sampler_release)(struct b2l_mtl_sampler *sampler);

	struct b2l_mtl_depth_stencil *(*depth_stencil_new)(struct b2l_mtl_device *device,
	    const struct b2l_mtl_depth_stencil_desc *desc);
	void (*depth_stencil_release)(struct b2l_mtl_depth_stencil *state);

	/* Libraries: a .metallib (fat or thin) from a file, mapped, or from bytes,
	 * copied. Stages are Mesa's gl_shader_stage numbers (0 vertex, 4 fragment,
	 * 5 compute), -1 unknown. */
	struct b2l_mtl_library *(*library_open)(struct b2l_mtl_device *device, const char *path, char *why,
	    size_t why_size);
	struct b2l_mtl_library *(*library_new_data)(struct b2l_mtl_device *device, const void *bytes, size_t size,
	    char *why, size_t why_size);
	uint32_t (*library_count)(struct b2l_mtl_library *library);
	const char *(*library_name)(struct b2l_mtl_library *library, uint32_t index);
	int32_t (*library_find)(struct b2l_mtl_library *library, const char *name);
	void (*library_release)(struct b2l_mtl_library *library);

	/* A function: one entry of a library with the constants given to it;
	 * translated when a pipeline first needs it. */
	struct b2l_mtl_function *(*function_new)(struct b2l_mtl_library *library, uint32_t index,
	    const struct b2l_mtl_constant *constants, uint32_t count, char *why, size_t why_size);
	int32_t (*function_stage)(struct b2l_mtl_function *function);
	const char *(*function_name)(struct b2l_mtl_function *function);
	void (*function_release)(struct b2l_mtl_function *function);

	/* A render pipeline: both functions translated, linked by varying name,
	 * and made into Panfrost's shader, blend and vertex element CSOs. */
	struct b2l_mtl_render_pipeline *(*render_pipeline_new)(struct b2l_mtl_device *device,
	    const struct b2l_mtl_render_pipeline_desc *desc, struct b2l_mtl_pipeline_report *report, char *why,
	    size_t why_size);
	void (*render_pipeline_release)(struct b2l_mtl_render_pipeline *pipeline);

	/* Command buffers: recorded on any thread, executed in order by
	 * cmdbuf_execute (the GPU's work is finished when it returns). */
	struct b2l_mtl_cmdbuf *(*cmdbuf_new)(struct b2l_mtl_device *device);
	void (*cmdbuf_pass_begin)(struct b2l_mtl_cmdbuf *cmdbuf, const struct b2l_mtl_pass_desc *desc);
	void (*cmdbuf_pass_end)(struct b2l_mtl_cmdbuf *cmdbuf);
	void (*cmdbuf_copy_buffer)(struct b2l_mtl_cmdbuf *cmdbuf, struct b2l_mtl_buffer *from, uint64_t from_offset,
	    struct b2l_mtl_buffer *to, uint64_t to_offset, uint64_t size);
	void (*cmdbuf_fill_buffer)(struct b2l_mtl_cmdbuf *cmdbuf, struct b2l_mtl_buffer *buffer, uint64_t offset,
	    uint64_t length, uint8_t value);
	void (*cmdbuf_copy_texture)(struct b2l_mtl_cmdbuf *cmdbuf, struct b2l_mtl_texture *from, uint32_t from_slice,
	    uint32_t from_level, const struct b2l_mtl_region *from_region, struct b2l_mtl_texture *to,
	    uint32_t to_slice, uint32_t to_level, uint32_t to_x, uint32_t to_y, uint32_t to_z);
	void (*cmdbuf_copy_buffer_to_texture)(struct b2l_mtl_cmdbuf *cmdbuf, struct b2l_mtl_buffer *from,
	    uint64_t offset, uint64_t bytes_per_row, uint64_t bytes_per_image, const struct b2l_mtl_region *region,
	    struct b2l_mtl_texture *to, uint32_t slice, uint32_t level);
	void (*cmdbuf_copy_texture_to_buffer)(struct b2l_mtl_cmdbuf *cmdbuf, struct b2l_mtl_texture *from,
	    uint32_t slice, uint32_t level, const struct b2l_mtl_region *region, struct b2l_mtl_buffer *to,
	    uint64_t offset, uint64_t bytes_per_row, uint64_t bytes_per_image);
	/* Notes a command the recorder could not express (logged at execution,
	 * the buffer's error). */
	void (*cmdbuf_unsupported)(struct b2l_mtl_cmdbuf *cmdbuf, const char *what);
	/* 0 when everything ran; otherwise the first error is in why. gpu_us:
	 * the time from the first submission to the end. */
	int (*cmdbuf_execute)(struct b2l_mtl_cmdbuf *cmdbuf, uint64_t *gpu_us, char *why, size_t why_size);
	void (*cmdbuf_release)(struct b2l_mtl_cmdbuf *cmdbuf);

	/* Version 2: the render encoder (between cmdbuf_pass_begin and _end). Its
	 * state starts afresh with each pass, as Metal's encoders do: viewport and
	 * scissor the whole target, no cull, clockwise front faces, fill, depth
	 * clip, no depth-stencil state (no depth test), blend colour zero. */
	void (*cmdbuf_set_pipeline)(struct b2l_mtl_cmdbuf *cmdbuf, struct b2l_mtl_render_pipeline *pipeline);
	void (*cmdbuf_set_buffer)(struct b2l_mtl_cmdbuf *cmdbuf, uint32_t stage, uint32_t index,
	    struct b2l_mtl_buffer *buffer, uint64_t offset);
	void (*cmdbuf_set_buffer_offset)(struct b2l_mtl_cmdbuf *cmdbuf, uint32_t stage, uint32_t index,
	    uint64_t offset);
	void (*cmdbuf_set_bytes)(struct b2l_mtl_cmdbuf *cmdbuf, uint32_t stage, uint32_t index, const void *bytes,
	    uint64_t length);
	void (*cmdbuf_set_texture)(struct b2l_mtl_cmdbuf *cmdbuf, uint32_t stage, uint32_t index,
	    struct b2l_mtl_texture *texture);
	void (*cmdbuf_set_sampler)(struct b2l_mtl_cmdbuf *cmdbuf, uint32_t stage, uint32_t index,
	    struct b2l_mtl_sampler *sampler);
	void (*cmdbuf_set_viewport)(struct b2l_mtl_cmdbuf *cmdbuf, const struct b2l_mtl_viewport *viewport);
	void (*cmdbuf_set_scissor)(struct b2l_mtl_cmdbuf *cmdbuf, uint32_t x, uint32_t y, uint32_t width,
	    uint32_t height);
	void (*cmdbuf_set_raster)(struct b2l_mtl_cmdbuf *cmdbuf, uint32_t which, uint32_t value);
	void (*cmdbuf_set_depth_stencil)(struct b2l_mtl_cmdbuf *cmdbuf, struct b2l_mtl_depth_stencil *state);
	void (*cmdbuf_set_stencil_ref)(struct b2l_mtl_cmdbuf *cmdbuf, uint32_t front, uint32_t back);
	void (*cmdbuf_set_blend_color)(struct b2l_mtl_cmdbuf *cmdbuf, const float rgba[4]);
	void (*cmdbuf_draw)(struct b2l_mtl_cmdbuf *cmdbuf, const struct b2l_mtl_draw *draw);
};

/* The table, or NULL if this library does not have that version. */
const struct b2l_mtl_api *b2l_mtl_api(uint32_t version);

#endif
