// SPDX-License-Identifier: MIT
/*
 * S10 port: memset, bzero, memcpy and memmove for everything linked into
 * libb2l-mesa, which binds them here rather than to libSystem's.
 *
 * The CPU mappings of B2LMali buffer objects behave as device memory: every
 * access must be aligned to its own size and DC ZVA is not allowed.
 * Boot 47: Panfrost zeroing a new texture's AFBC headers through such a
 * mapping (panfrost_resource_init_afbc_headers) faulted in libSystem's
 * memset, which clears large ranges with DC ZVA. Boot 48: a 64-bit store at a
 * 4-byte boundary faulted in panfrost_update_shader_state (the build now
 * passes -mstrict-align); libSystem's memcpy has the same kind of unaligned
 * head and tail accesses. These routines use only accesses aligned to their
 * size, on both sides: bytes up to an aligned destination, then aligned
 * 8-byte words (shifted together from aligned source words when the source is
 * not aligned the same way), then bytes. no_builtin keeps the compiler from
 * turning their loops back into calls to themselves.
 */
#include <stddef.h>
#include <stdint.h>

void *memset(void *dst, int c, size_t n);
void *__memset_chk(void *dst, int c, size_t n, size_t dstlen);
void bzero(void *dst, size_t n);
void __bzero(void *dst, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *__memcpy_chk(void *dst, const void *src, size_t n, size_t dstlen);
void *__memmove_chk(void *dst, const void *src, size_t n, size_t dstlen);
void bcopy(const void *src, void *dst, size_t n);

#define NO_BUILTIN __attribute__((no_builtin))

/* Forward copy; never reads a byte outside an aligned word holding source bytes. */
NO_BUILTIN static void
copy_forward(uint8_t *d, const uint8_t *s, size_t n)
{
	while (n > 0 && ((uintptr_t)d & 7) != 0) {
		*d++ = *s++;
		n--;
	}
	if (n >= 8) {
		unsigned shift = (unsigned)((uintptr_t)s & 7);
		if (shift == 0) {
			while (n >= 8) {
				*(uint64_t *)(void *)d = *(const uint64_t *)(const void *)s;
				d += 8;
				s += 8;
				n -= 8;
			}
		} else {
			const uint64_t *sa = (const uint64_t *)(const void *)(s - shift);
			unsigned rs = shift * 8, ls = 64 - rs;
			uint64_t lo = *sa;
			while (n >= 8) {
				uint64_t hi = sa[1];
				*(uint64_t *)(void *)d = (lo >> rs) | (hi << ls);
				lo = hi;
				sa++;
				d += 8;
				s += 8;
				n -= 8;
			}
		}
	}
	while (n > 0) {
		*d++ = *s++;
		n--;
	}
}

/* Backward copy for overlapping moves: 8-byte words when both ends line up. */
NO_BUILTIN static void
copy_backward(uint8_t *d, const uint8_t *s, size_t n)
{
	d += n;
	s += n;
	if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
		while (n > 0 && ((uintptr_t)d & 7) != 0) {
			*--d = *--s;
			n--;
		}
		while (n >= 8) {
			d -= 8;
			s -= 8;
			*(uint64_t *)(void *)d = *(const uint64_t *)(const void *)s;
			n -= 8;
		}
	}
	while (n > 0) {
		*--d = *--s;
		n--;
	}
}

NO_BUILTIN void *
memcpy(void *dst, const void *src, size_t n)
{
	copy_forward(dst, src, n);
	return dst;
}

NO_BUILTIN void *
memmove(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;
	if (d == s || n == 0) {
		return dst;
	}
	if (d < s || d >= s + n) {
		copy_forward(d, s, n);
	} else {
		copy_backward(d, s, n);
	}
	return dst;
}

NO_BUILTIN void *
__memcpy_chk(void *dst, const void *src, size_t n, size_t dstlen)
{
	if (n > dstlen) {
		__builtin_trap();
	}
	return memcpy(dst, src, n);
}

NO_BUILTIN void *
__memmove_chk(void *dst, const void *src, size_t n, size_t dstlen)
{
	if (n > dstlen) {
		__builtin_trap();
	}
	return memmove(dst, src, n);
}

NO_BUILTIN void
bcopy(const void *src, void *dst, size_t n)
{
	memmove(dst, src, n);
}

static void
fill(void *dst, uint8_t v, size_t n)
{
	volatile uint8_t *b = dst;
	while (n > 0 && ((uintptr_t)b & 15) != 0) {
		*b++ = v;
		n--;
	}
	uint64_t w = 0x0101010101010101ull * v;
	size_t blocks = n / 64;
	if (blocks > 0) {
		uint8_t *q = (uint8_t *)(uintptr_t)b;
		__asm__ volatile(
		    "1:\n"
		    " stp %[w], %[w], [%[q]]\n"
		    " stp %[w], %[w], [%[q], #16]\n"
		    " stp %[w], %[w], [%[q], #32]\n"
		    " stp %[w], %[w], [%[q], #48]\n"
		    " add %[q], %[q], #64\n"
		    " subs %[n], %[n], #1\n"
		    " b.ne 1b\n"
		    : [q] "+r"(q), [n] "+r"(blocks)
		    : [w] "r"(w)
		    : "memory", "cc");
		b = q;
		n &= 63;
	}
	while (n >= 8) {
		*(volatile uint64_t *)b = w;
		b += 8;
		n -= 8;
	}
	while (n > 0) {
		*b++ = v;
		n--;
	}
}

void *
memset(void *dst, int c, size_t n)
{
	fill(dst, (uint8_t)c, n);
	return dst;
}

void *
__memset_chk(void *dst, int c, size_t n, size_t dstlen)
{
	if (n > dstlen) {
		__builtin_trap();
	}
	fill(dst, (uint8_t)c, n);
	return dst;
}

void
bzero(void *dst, size_t n)
{
	fill(dst, 0, n);
}

void
__bzero(void *dst, size_t n)
{
	fill(dst, 0, n);
}
