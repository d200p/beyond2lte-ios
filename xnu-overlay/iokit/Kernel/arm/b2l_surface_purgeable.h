/* SPDX-License-Identifier: BSD-2-Clause
 * IOSurfaceClientSetPurgeable (IOSurfaceRootUserClient selector 20) for B2L.
 *
 * Core Animation marks idle cached surfaces volatile and, before reuse, sets
 * them non-volatile again and inspects the returned old state. A failed call
 * makes it treat the contents as discarded; the 0105 runtime showed backboardd
 * then asserting `found != slist->end ()` in CA::OGL::Context::push_surface.
 * B2L surfaces are wired kernel memory and are never reclaimed, so the old
 * state reported is exactly the state last requested: Volatile stays Volatile,
 * never silently Empty. A client that explicitly requests Empty gets Empty
 * back, as it asked. Kernel and host tests share this helper.
 */
#ifndef B2L_SURFACE_PURGEABLE_H
#define B2L_SURFACE_PURGEABLE_H
#include <stdint.h>

enum {
	kB2LSurfacePurgeNonVolatile = 0,
	kB2LSurfacePurgeVolatile    = 1,
	kB2LSurfacePurgeEmpty       = 2,
	kB2LSurfacePurgeKeepCurrent = 3,
};

/* Returns 0 and the previous state in *old, or -1 for an unknown request. */
static inline int
b2l_surface_purgeable(uint32_t *state, uint64_t requested, uint64_t *old)
{
	uint32_t previous;
	if (!state || !old || requested > kB2LSurfacePurgeKeepCurrent) return -1;
	if (requested == kB2LSurfacePurgeKeepCurrent)
		previous = __atomic_load_n(state, __ATOMIC_RELAXED);
	else
		previous = __atomic_exchange_n(state, (uint32_t)requested, __ATOMIC_RELAXED);
	*old = previous;
	return 0;
}
#endif
