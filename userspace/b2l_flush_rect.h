/* SPDX-License-Identifier: BSD-2-Clause
 * The rows and columns of a texture a draw can touch, so the CPU cleans those
 * and not the whole surface. Free of platform types for the host test.
 */
#ifndef _B2L_FLUSH_RECT_H
#define _B2L_FLUSH_RECT_H

#include <stdint.h>

#define B2L_FLUSH_MARGIN 2      /* texels past the edge, for linear filtering */

/* Widen [*first, *last) to hold a..b, normalized. Anything odd means all of it. */
static inline void
b2l_flush_span(float a, float b, uint32_t size, uint32_t *first, uint32_t *last)
{
	float low = a < b ? a : b, high = a < b ? b : a;

	if (!(low >= -1.0f) || !(high <= 2.0f)) {
		*first = 0;
		*last = size;
		return;
	}
	int64_t from = (int64_t)(low * (float)size) - B2L_FLUSH_MARGIN;
	int64_t to = (int64_t)(high * (float)size) + 1 + B2L_FLUSH_MARGIN;
	from = from < 0 ? 0 : from;
	to = to > (int64_t)size ? (int64_t)size : to;
	*first = (uint32_t)from < *first ? (uint32_t)from : *first;
	*last = (uint32_t)to > *last ? (uint32_t)to : *last;
}

#endif /* _B2L_FLUSH_RECT_H */
