/* SPDX-License-Identifier: BSD-2-Clause
 * Keyless PACGA for a precompiled stripped-pointer development root.
 *
 * In this ABI PAC/AUT/XPAC are identity operations, so PACGA protects nothing;
 * callers only need a deterministic 64-bit value whose low 32 bits are zero and
 * which every producer and checker computes identically. The host precompiler
 * replaces PACGA sites with a branch to a short routine computing this function
 * in EL0, and the kernel emulator computes the same function for every PACGA it
 * still traps (JIT code, sites outside branch range, SP operands). A root using
 * these routines therefore requires a kernel booted with b2l_pacga_keyless=1.
 *
 * The routine uses only the destination register, sets no flags and touches no
 * memory, so surrounding code observes exactly what PACGA would have done.
 * Shared by the kernel (osfmk/arm64) and tools/b2l-precompile.c.
 */
#ifndef B2L_PACGA_KEYLESS_H
#define B2L_PACGA_KEYLESS_H
#include <stdint.h>

/* n: Rn value (XZR reads as 0). m: Rm value (SP when Rm is 31). */
static inline uint64_t
b2l_pacga_keyless(uint64_t n, uint64_t m)
{
    uint64_t x = n ^ ((m >> 29) | (m << 35));
    x ^= x >> 31;
    x += x << 21;
    x ^= x >> 29;
    x += x << 13;
    x ^= x >> 33;
    return x & UINT64_C(0xffffffff00000000);
}

#define B2L_PACGA_KEYLESS_WORDS 7u

/* EL0 routine for PACGA Xd, Xn, Xm with d, m < 31 (m == 31 means SP, which the
 * EOR below would read as XZR; such sites keep trapping). Rn 31 is XZR in both
 * PACGA and EOR. The first instruction reads n and m before writing d, so any
 * aliasing of d with n or m is safe. Returns 0 when the operands are unsupported. */
static inline int
b2l_pacga_keyless_encode(unsigned d, unsigned n, unsigned m, uint32_t out[B2L_PACGA_KEYLESS_WORDS])
{
    if (d >= 31 || n > 31 || m >= 31) return 0;
#define B2L_EOR(rd, rn, rm, sh, amt) (UINT32_C(0xca000000) | (uint32_t)(sh) << 22 | (uint32_t)(rm) << 16 | (uint32_t)(amt) << 10 | (uint32_t)(rn) << 5 | (uint32_t)(rd))
#define B2L_ADD(rd, rn, rm, amt) (UINT32_C(0x8b000000) | (uint32_t)(rm) << 16 | (uint32_t)(amt) << 10 | (uint32_t)(rn) << 5 | (uint32_t)(rd))
    out[0] = B2L_EOR(d, n, m, 3u, 29u);   /* eor d, n, m, ror #29 */
    out[1] = B2L_EOR(d, d, d, 1u, 31u);   /* eor d, d, d, lsr #31 */
    out[2] = B2L_ADD(d, d, d, 21u);       /* add d, d, d, lsl #21 */
    out[3] = B2L_EOR(d, d, d, 1u, 29u);   /* eor d, d, d, lsr #29 */
    out[4] = B2L_ADD(d, d, d, 13u);       /* add d, d, d, lsl #13 */
    out[5] = B2L_EOR(d, d, d, 1u, 33u);   /* eor d, d, d, lsr #33 */
    out[6] = UINT32_C(0x92607c00) | d << 5 | d; /* and d, d, #0xffffffff00000000 */
#undef B2L_EOR
#undef B2L_ADD
    return 1;
}
#endif
