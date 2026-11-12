/* SPDX-License-Identifier: BSD-2-Clause
 * FJCVTZS, the ARMv8.3 JavaScript conversion, which iOS's arm64e JavaScriptCore
 * uses and these cores lack. Truncate a double, keep the low 32 bits of the
 * integer, and report whether that was exact. Integer arithmetic only, because
 * it runs in the kernel. FPSR's sticky flags are left alone.
 */
#ifndef B2L_FJCVTZS_H
#define B2L_FJCVTZS_H

#include <stdint.h>

#define B2L_FJCVTZS_MASK  0xfffffc00u
#define B2L_FJCVTZS_VALUE 0x1e7e0000u

/* *exact is what the instruction writes to PSTATE.Z. */
static inline uint32_t
b2l_fjcvtzs(uint64_t bits, int *exact)
{
	int sign = (int)(bits >> 63);
	int exponent = (int)((bits >> 52) & 0x7ff);
	uint64_t mantissa = bits & ((1ull << 52) - 1);
	uint64_t whole = 0;

	*exact = 0;
	if (exponent == 0x7ff) {
		return 0;               /* infinity or NaN */
	}
	if (exponent == 0 && mantissa == 0) {
		*exact = !sign;         /* -0 is not representable */
		return 0;
	}
	if (exponent == 0) {
		return 0;               /* subnormal: truncates to zero, inexact */
	}
	mantissa |= 1ull << 52;

	int shift = exponent - 1075;
	int fraction = 0, overflow = 0;
	if (shift >= 0) {
		whole = shift < 64 ? mantissa << shift : 0;
		overflow = shift >= 11;         /* at least 2^63: out of range */
	} else if (shift > -64) {
		whole = mantissa >> -shift;
		fraction = (mantissa & ((1ull << -shift) - 1)) != 0;
	} else {
		fraction = 1;
	}
	overflow |= whole > (sign ? 0x80000000ull : 0x7fffffffull);
	*exact = !fraction && !overflow;
	return (uint32_t)(sign ? 0 - whole : whole);
}

#endif /* B2L_FJCVTZS_H */
