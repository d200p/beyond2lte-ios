/* SPDX-License-Identifier: BSD-2-Clause
 * Settings-sized entitlement slots: a 75,435-byte XML and a 52,207-byte DER slot
 * in one signature must both read; a slot over the limit must still fail. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "b2l_amfi_metadata.h"
static uint8_t *data; static size_t bytes;
static void le(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void be(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[3 - i] = (uint8_t)(v >> (8 * i)); }
static int rd(void *o, uint64_t at, void *out, size_t n) { (void)o; if (at > bytes || n > bytes - at) return -1; memcpy(out, data + at, n); return 0; }
static void build(uint32_t xml, uint32_t der) {
    uint32_t sig = 256, total = 12 + 16 + xml + der;
    bytes = sig + total; free(data); data = calloc(1, bytes); assert(data);
    le(data, 0xfeedfacf); le(data + 4, 0x100000c); le(data + 12, 2); le(data + 16, 1); le(data + 20, 16);
    le(data + 32, 0x1d); le(data + 36, 16); le(data + 40, sig); le(data + 44, total);
    uint8_t *s = data + sig; be(s, 0xfade0cc0); be(s + 4, total); be(s + 8, 2);
    be(s + 12, 5); be(s + 16, 28); be(s + 20, 7); be(s + 24, 28 + xml);
    be(s + 28, 0xfade7171); be(s + 32, xml); memset(s + 36, 'x', xml - 8);
    be(s + 28 + xml, 0xfade7172); be(s + 32 + xml, der); memset(s + 36 + xml, 0x30, der - 8);
}
int main(void) {
    struct b2l_amfi_reader r; static uint8_t out[B2L_AMFI_XML_MAX]; size_t n;
    build(75435, 52207); r = (struct b2l_amfi_reader){bytes, rd, NULL};
    n = 1; assert(b2l_amfi_read_xml(&r, 0, out, sizeof(out), &n) == B2L_AMFI_XML_PRESENT && n == 75435 && out[8] == 'x');
    n = 1; assert(b2l_amfi_read_der(&r, 0, out, sizeof(out), &n) == B2L_AMFI_XML_PRESENT && n == 52207 && out[8] == 0x30);
    /* The old 64 KiB caller buffer still fails cleanly instead of overflowing. */
    n = 1; assert(b2l_amfi_read_xml(&r, 0, out, 65536, &n) == B2L_AMFI_XML_INVALID && n == 0);
    /* Over the new limit: rejected, for either slot. */
    build(B2L_AMFI_XML_MAX + 1, 64); r = (struct b2l_amfi_reader){bytes, rd, NULL};
    n = 1; assert(b2l_amfi_read_der(&r, 0, out, sizeof(out), &n) == B2L_AMFI_XML_INVALID && n == 0);
    build(64, B2L_AMFI_DER_MAX + 1); r = (struct b2l_amfi_reader){bytes, rd, NULL};
    n = 1; assert(b2l_amfi_read_xml(&r, 0, out, sizeof(out), &n) == B2L_AMFI_XML_INVALID && n == 0);
    free(data);
    puts("Large entitlements: Settings-sized XML and DER both read; over-limit slots rejected PASS");
    return 0;
}
