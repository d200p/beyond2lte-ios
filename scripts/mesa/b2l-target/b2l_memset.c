// SPDX-License-Identifier: MIT
/*
 * S10 port: memset and bzero for everything linked into libb2l-mesa, which
 * binds them here rather than to libSystem's.
 *
 * Boot 47: Core Animation's first texture upload with AFBC made Panfrost zero
 * the new texture's AFBC headers (panfrost_resource_init_afbc_headers) through
 * the buffer object's CPU mapping, which B2LMali maps write-combined.
 * libSystem's memset/bzero clear large ranges with DC ZVA, and that faulted
 * there (EXC_ARM_DA_ALIGN, 64 bytes into the buffer), taking backboardd down.
 * These use plain stores only: bytes up to 16-byte alignment, 64-byte blocks
 * of aligned STPs, then aligned 8-byte and single-byte stores. The byte loops
 * go through volatile pointers so the compiler cannot turn them back into a
 * memset call.
 */
#include <stddef.h>
#include <stdint.h>

void *memset(void *dst, int c, size_t n);
void *__memset_chk(void *dst, int c, size_t n, size_t dstlen);
void bzero(void *dst, size_t n);
void __bzero(void *dst, size_t n);

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
