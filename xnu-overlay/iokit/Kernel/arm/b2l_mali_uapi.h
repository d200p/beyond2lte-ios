/* SPDX-License-Identifier: BSD-2-Clause
 * Interface between B2LMali and its user clients. Type 0 is the one drawing
 * client in backboardd (buffers, surfaces, passes). Type 1 is a buffer-object
 * client for Mesa's Panfrost through the B2LMali libdrm: any number may be
 * open, each owns its buffer objects, all share the GPU address space. Jobs
 * are synchronous: submit returns when the job chain has finished, so no
 * fences are needed.
 */
#ifndef _B2L_MALI_UAPI_H
#define _B2L_MALI_UAPI_H

#include <stdint.h>

enum {
	kB2LMaliLog           = 0,      /* struct in: text, logged with a prefix */
	kB2LMaliCreateBuffer  = 1,      /* in: bytes, flags; out: GPU VA, memory type */
	kB2LMaliMapSurface    = 2,      /* in: surface id; out: GPU VA, stride, w, h, format */
	kB2LMaliSubmit        = 3,      /* struct in: below; out: status, tiler ns, fragment ns */
	kB2LMaliInfo          = 4,      /* out: heap VA, heap bytes, cache clean ns */
	kB2LMaliMapRange      = 5,      /* in: client VA, bytes; out: GPU VA of that VA */
	/* S10 port, Mesa Panfrost (any client type): */
	kB2LMaliProps         = 6,      /* struct out: struct b2l_mali_props */
	kB2LMaliBoCreate      = 7,      /* in: bytes, B2L_MALI_BO_*; out: handle, GPU VA */
	kB2LMaliBoFree        = 8,      /* in: handle */
	kB2LMaliJobSubmit     = 9,      /* in: job chain VA, B2L_MALI_REQ_*; out: status, ns */
};

#define B2L_MALI_CLIENT_DRAW    0u      /* IOServiceOpen types */
#define B2L_MALI_CLIENT_BO      1u

/* Buffer objects, as the Linux panfrost uapi: NOEXEC and HEAP flags, handle
 * 0 is never used, the GPU VA never changes. A HEAP object (the tiler heap)
 * keeps its whole VA range but only B2L_MALI_BO_HEAP_RESIDENT bytes of
 * memory: there is no growing on GPU faults. Map one into the client with
 * IOConnectMapMemory64(B2L_MALI_BO_MEMORY(handle)); its memory is
 * write-combined there, so neither side needs cache maintenance. */
#define B2L_MALI_BO_NOEXEC      1u
#define B2L_MALI_BO_HEAP        2u
#define B2L_MALI_BO_MEMORY(h)   (0x10000u | (h))
#define B2L_MALI_BO_HEAP_RESIDENT (32u << 20)
#define B2L_MALI_MAX_BOS        8192
#define B2L_MALI_REQ_FS         1u      /* = PANFROST_JD_REQ_FS: fragment slot */

#define B2L_MALI_BUFFER_CODE    1u      /* executable by the GPU */
#define B2L_MALI_MAX_LOG        256
#define B2L_MALI_MAX_BUFFERS    8
#define B2L_MALI_MAX_SUBMIT     16

struct b2l_mali_submit {
	uint64_t tiler_va, fragment_va;
	uint32_t pass_buffer;           /* memory type of the pass, cleaned before the job */
	uint32_t pass_bytes;
	uint32_t target;                /* surface id written, 0 for none */
	uint32_t source_count;
	uint32_t sources[B2L_MALI_MAX_SUBMIT];  /* surface ids read */
};

/* GPU registers Panfrost asks for (DRM_PANFROST_PARAM_*), read once at start. */
struct b2l_mali_props {
	uint32_t gpu_id, l2_features, core_features, tiler_features;
	uint32_t mem_features, mmu_features, as_present, js_present;
	uint32_t thread_max_threads, thread_max_workgroup, thread_max_barrier, thread_features;
	uint32_t coherency_features, thread_tls_alloc, afbc_features, reserved;
	uint32_t texture_features[4];
	uint32_t js_features[16];
	uint64_t shader_present, tiler_present, l2_present, stack_present;
};

/* status: 1 done; 0 timeout; otherwise the JS_STATUS exception code. */
#define B2L_MALI_STATUS_DONE    1u

#endif /* _B2L_MALI_UAPI_H */
