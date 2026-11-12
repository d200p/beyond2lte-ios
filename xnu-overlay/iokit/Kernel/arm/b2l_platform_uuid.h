/* SPDX-License-Identifier: BSD-2-Clause
 * The platform UUID. gethostuuid() waits for it with no timeout, and XNU derives
 * it only from ids iBoot leaves in /chosen, which this boot chain does not have.
 * Without it configd's main thread sleeps for good and every SCDynamicStore
 * client stalls behind it.
 *
 * The Wi-Fi address is the one stable id this unit carries, and it is mixed so
 * the UUID stays the same across reboots without showing the address. This runs
 * in the platform expert's start, before corecrypto registers: SHA1Init would
 * dereference a NULL g_crypto_funcs here, so the mixing is plain arithmetic.
 */
#ifndef _B2L_PLATFORM_UUID_H
#define _B2L_PLATFORM_UUID_H

#include <stddef.h>
#include <stdint.h>

static inline uint64_t
b2l_platform_uuid_mix(const uint8_t *id, size_t length, uint64_t hash)
{
	for (size_t i = 0; i < length; i++) {
		hash = (hash ^ id[i]) * 0x100000001b3ull;
	}
	hash ^= hash >> 33;
	hash *= 0xff51afd7ed558ccdull;
	hash ^= hash >> 33;
	hash *= 0xc4ceb9fe1a85ec53ull;
	return hash ^ (hash >> 33);
}

/* id may be NULL: the UUID is then a constant, which still lets the waiters go. */
static inline void
b2l_platform_uuid(const void *id, size_t length, uint8_t uuid[16])
{
	uint64_t low = b2l_platform_uuid_mix((const uint8_t *)id, id != NULL ? length : 0, 0xf7d0d44de75581b1ull);
	uint64_t high = b2l_platform_uuid_mix((const uint8_t *)id, id != NULL ? length : 0, 0xd9befb4efb849d95ull);

	for (unsigned i = 0; i < 8; i++) {
		uuid[i] = (uint8_t)(low >> (i * 8));
		uuid[i + 8] = (uint8_t)(high >> (i * 8));
	}
	uuid[6] = (uuid[6] & 0x0f) | 0x50;
	uuid[8] = (uuid[8] & 0x3f) | 0x80;
}

#endif /* _B2L_PLATFORM_UUID_H */
