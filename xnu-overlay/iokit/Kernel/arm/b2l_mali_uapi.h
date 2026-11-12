/* SPDX-License-Identifier: BSD-2-Clause
 * Interface between B2LMali and its one user client in backboardd. Open the
 * B2LMali service with type 0. Jobs are synchronous: submit returns when the
 * fragment job has finished, so a pass needs no fences.
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
};

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

/* status: 1 done; 0 timeout; otherwise the JS_STATUS exception code. */
#define B2L_MALI_STATUS_DONE    1u

#endif /* _B2L_MALI_UAPI_H */
