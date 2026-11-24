/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef B2L_WIFI_CRYPTO_H
#define B2L_WIFI_CRYPTO_H
/* Every function may go unused by one includer (the kernel never wraps a key). */
#define B2L_WIFI_CRYPTO static __attribute__((unused))
/*
 * S10 port, Wi-Fi mode 23: what a WPA2-PSK station computes, written for this
 * port. SHA-1 (FIPS 180-4), HMAC-SHA1 (RFC 2104), PBKDF2-HMAC-SHA1 (RFC 8018,
 * the passphrase-to-PMK mapping of IEEE 802.11 J.4), the 802.11 PRF (12.7.1.2),
 * AES-128 (FIPS 197) and the AES key wrap and unwrap (RFC 3394) that carry the
 * group key in message 3 of the 4-way handshake.
 *
 * Header-only, no allocation, nothing logged. The caller provides uint8_t,
 * uint32_t, uint64_t, size_t and memcpy/memset/memcmp. The AES tables are
 * computed at first use from the field arithmetic, not typed in.
 * tests/test_wifi_crypto.py checks every function against Python's hashlib
 * and hmac and against the FIPS 197 and RFC 3394 vectors.
 */

struct b2l_sha1 {
    uint32_t h[5];
    uint64_t bytes;
    uint8_t block[64];
    unsigned used;
};

static inline uint32_t
b2l_rol32(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32 - n));
}

B2L_WIFI_CRYPTO void
b2l_sha1_compress(uint32_t h[5], const uint8_t b[64])
{
    uint32_t w[80];
    for (unsigned i = 0; i < 16; i++)
        w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
    for (unsigned i = 16; i < 80; i++)
        w[i] = b2l_rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], c1 = h[1], c2 = h[2], d = h[3], e = h[4];
    for (unsigned i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (c1 & c2) | (~c1 & d); k = 0x5a827999u; }
        else if (i < 40) { f = c1 ^ c2 ^ d; k = 0x6ed9eba1u; }
        else if (i < 60) { f = (c1 & c2) | (c1 & d) | (c2 & d); k = 0x8f1bbcdcu; }
        else { f = c1 ^ c2 ^ d; k = 0xca62c1d6u; }
        uint32_t t = b2l_rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c2;
        c2 = b2l_rol32(c1, 30);
        c1 = a;
        a = t;
    }
    h[0] += a;
    h[1] += c1;
    h[2] += c2;
    h[3] += d;
    h[4] += e;
}

B2L_WIFI_CRYPTO void
b2l_sha1_init(struct b2l_sha1 *s)
{
    s->h[0] = 0x67452301u;
    s->h[1] = 0xefcdab89u;
    s->h[2] = 0x98badcfeu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xc3d2e1f0u;
    s->bytes = 0;
    s->used = 0;
}

B2L_WIFI_CRYPTO void
b2l_sha1_update(struct b2l_sha1 *s, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    s->bytes += n;
    while (n) {
        unsigned take = 64 - s->used;
        if (take > n) take = (unsigned)n;
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        n -= take;
        if (s->used == 64) {
            b2l_sha1_compress(s->h, s->block);
            s->used = 0;
        }
    }
}

B2L_WIFI_CRYPTO void
b2l_sha1_final(struct b2l_sha1 *s, uint8_t out[20])
{
    const uint64_t bits = s->bytes * 8;
    uint8_t pad = 0x80;
    b2l_sha1_update(s, &pad, 1);
    pad = 0;
    while (s->used != 56) b2l_sha1_update(s, &pad, 1);
    uint8_t length[8];
    for (unsigned i = 0; i < 8; i++) length[i] = (uint8_t)(bits >> (56 - 8 * i));
    b2l_sha1_update(s, length, 8);
    for (unsigned i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
    memset(s, 0, sizeof(*s));
}

/* HMAC-SHA1 over the concatenation of n parts. Keys over 64 bytes are hashed. */
B2L_WIFI_CRYPTO void
b2l_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *const parts[], const size_t lens[], unsigned n,
    uint8_t out[20])
{
    uint8_t k[64], pad[64], inner[20];
    struct b2l_sha1 s;
    memset(k, 0, sizeof(k));
    if (key_len > 64) {
        b2l_sha1_init(&s);
        b2l_sha1_update(&s, key, key_len);
        b2l_sha1_final(&s, k);
    } else {
        memcpy(k, key, key_len);
    }
    for (unsigned i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    b2l_sha1_init(&s);
    b2l_sha1_update(&s, pad, 64);
    for (unsigned i = 0; i < n; i++) b2l_sha1_update(&s, parts[i], lens[i]);
    b2l_sha1_final(&s, inner);
    for (unsigned i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    b2l_sha1_init(&s);
    b2l_sha1_update(&s, pad, 64);
    b2l_sha1_update(&s, inner, 20);
    b2l_sha1_final(&s, out);
    memset(k, 0, sizeof(k));
    memset(pad, 0, sizeof(pad));
    memset(inner, 0, sizeof(inner));
}

/* PBKDF2-HMAC-SHA1, any output length; WPA2's PMK is (passphrase, SSID, 4096, 32). */
B2L_WIFI_CRYPTO void
b2l_pbkdf2_sha1(const uint8_t *password, size_t password_len, const uint8_t *salt, size_t salt_len,
    unsigned iterations, uint8_t *out, size_t out_len)
{
    for (uint32_t block = 1; out_len; block++) {
        const uint8_t index[4] = { (uint8_t)(block >> 24), (uint8_t)(block >> 16), (uint8_t)(block >> 8),
                                   (uint8_t)block };
        const uint8_t *parts[2] = { salt, index };
        const size_t lens[2] = { salt_len, 4 };
        uint8_t u[20], t[20];
        b2l_hmac_sha1(password, password_len, parts, lens, 2, u);
        memcpy(t, u, 20);
        for (unsigned i = 1; i < iterations; i++) {
            const uint8_t *again[1] = { u };
            const size_t again_len[1] = { 20 };
            b2l_hmac_sha1(password, password_len, again, again_len, 1, u);
            for (unsigned k = 0; k < 20; k++) t[k] ^= u[k];
        }
        const size_t take = out_len < 20 ? out_len : 20;
        memcpy(out, t, take);
        out += take;
        out_len -= take;
        memset(u, 0, sizeof(u));
        memset(t, 0, sizeof(t));
    }
}

/* The 802.11 PRF: HMAC-SHA1(K, label || 0 || data || i) for i = 0, 1, ..., cut to out_len. */
B2L_WIFI_CRYPTO void
b2l_prf_sha1(const uint8_t *key, size_t key_len, const char *label, const uint8_t *data, size_t data_len,
    uint8_t *out, size_t out_len)
{
    size_t label_len = 0;
    while (label[label_len]) label_len++;
    const uint8_t zero = 0;
    for (uint8_t i = 0; out_len; i++) {
        const uint8_t *parts[4] = { (const uint8_t *)label, &zero, data, &i };
        const size_t lens[4] = { label_len, 1, data_len, 1 };
        uint8_t block[20];
        b2l_hmac_sha1(key, key_len, parts, lens, 4, block);
        const size_t take = out_len < 20 ? out_len : 20;
        memcpy(out, block, take);
        out += take;
        out_len -= take;
        memset(block, 0, sizeof(block));
    }
}

/* ---- AES-128 ---- */

static uint8_t b2l_aes_sbox[256], b2l_aes_inv[256];

static inline uint8_t
b2l_gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    while (b) {
        if (b & 1) p ^= a;
        a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1b : 0));
        b >>= 1;
    }
    return p;
}

/* The S-box from its definition: the inverse in GF(2^8), then the affine map. */
B2L_WIFI_CRYPTO void
b2l_aes_tables(void)
{
    if (b2l_aes_sbox[0] == 0x63) return;
    for (unsigned x = 0; x < 256; x++) {
        uint8_t inv = 0;
        for (unsigned y = 1; x && y < 256; y++) {
            if (b2l_gmul((uint8_t)x, (uint8_t)y) == 1) { inv = (uint8_t)y; break; }
        }
        uint8_t s = inv;
        for (unsigned r = 1; r <= 4; r++) s ^= (uint8_t)((inv << r) | (inv >> (8 - r)));
        s ^= 0x63;
        b2l_aes_sbox[x] = s;
        b2l_aes_inv[s] = (uint8_t)x;
    }
}

struct b2l_aes128 {
    uint8_t round[11][16];
};

B2L_WIFI_CRYPTO void
b2l_aes128_key(struct b2l_aes128 *k, const uint8_t key[16])
{
    b2l_aes_tables();
    uint8_t rcon = 1;
    memcpy(k->round[0], key, 16);
    for (unsigned r = 1; r <= 10; r++) {
        const uint8_t *p = k->round[r - 1];
        uint8_t *q = k->round[r];
        uint8_t t[4] = { b2l_aes_sbox[p[13]], b2l_aes_sbox[p[14]], b2l_aes_sbox[p[15]], b2l_aes_sbox[p[12]] };
        t[0] ^= rcon;
        rcon = b2l_gmul(rcon, 2);
        for (unsigned i = 0; i < 4; i++) q[i] = p[i] ^ t[i];
        for (unsigned i = 4; i < 16; i++) q[i] = p[i] ^ q[i - 4];
    }
}

B2L_WIFI_CRYPTO void
b2l_aes128_encrypt(const struct b2l_aes128 *k, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    for (unsigned i = 0; i < 16; i++) s[i] = in[i] ^ k->round[0][i];
    for (unsigned r = 1; r <= 10; r++) {
        /* SubBytes and ShiftRows: byte (row, column) at 4 * column + row */
        for (unsigned c = 0; c < 4; c++)
            for (unsigned row = 0; row < 4; row++) t[4 * c + row] = b2l_aes_sbox[s[4 * ((c + row) % 4) + row]];
        if (r < 10) {
            for (unsigned c = 0; c < 4; c++) {
                const uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                s[4 * c] = (uint8_t)(b2l_gmul(a0, 2) ^ b2l_gmul(a1, 3) ^ a2 ^ a3);
                s[4 * c + 1] = (uint8_t)(a0 ^ b2l_gmul(a1, 2) ^ b2l_gmul(a2, 3) ^ a3);
                s[4 * c + 2] = (uint8_t)(a0 ^ a1 ^ b2l_gmul(a2, 2) ^ b2l_gmul(a3, 3));
                s[4 * c + 3] = (uint8_t)(b2l_gmul(a0, 3) ^ a1 ^ a2 ^ b2l_gmul(a3, 2));
            }
        } else {
            memcpy(s, t, 16);
        }
        for (unsigned i = 0; i < 16; i++) s[i] ^= k->round[r][i];
    }
    memcpy(out, s, 16);
    memset(s, 0, sizeof(s));
    memset(t, 0, sizeof(t));
}

B2L_WIFI_CRYPTO void
b2l_aes128_decrypt(const struct b2l_aes128 *k, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], t[16];
    for (unsigned i = 0; i < 16; i++) s[i] = in[i] ^ k->round[10][i];
    for (unsigned r = 10; r >= 1; r--) {
        /* InvShiftRows and InvSubBytes */
        for (unsigned c = 0; c < 4; c++)
            for (unsigned row = 0; row < 4; row++) t[4 * ((c + row) % 4) + row] = b2l_aes_inv[s[4 * c + row]];
        for (unsigned i = 0; i < 16; i++) t[i] ^= k->round[r - 1][i];
        if (r > 1) {
            for (unsigned c = 0; c < 4; c++) {
                const uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
                s[4 * c] = (uint8_t)(b2l_gmul(a0, 14) ^ b2l_gmul(a1, 11) ^ b2l_gmul(a2, 13) ^ b2l_gmul(a3, 9));
                s[4 * c + 1] = (uint8_t)(b2l_gmul(a0, 9) ^ b2l_gmul(a1, 14) ^ b2l_gmul(a2, 11) ^ b2l_gmul(a3, 13));
                s[4 * c + 2] = (uint8_t)(b2l_gmul(a0, 13) ^ b2l_gmul(a1, 9) ^ b2l_gmul(a2, 14) ^ b2l_gmul(a3, 11));
                s[4 * c + 3] = (uint8_t)(b2l_gmul(a0, 11) ^ b2l_gmul(a1, 13) ^ b2l_gmul(a2, 9) ^ b2l_gmul(a3, 14));
            }
        } else {
            memcpy(s, t, 16);
        }
    }
    memcpy(out, s, 16);
    memset(s, 0, sizeof(s));
    memset(t, 0, sizeof(t));
}

/* RFC 3394 with the default IV: n 64-bit blocks of plain text, n + 1 out. */
B2L_WIFI_CRYPTO void
b2l_aes_wrap(const uint8_t kek[16], const uint8_t *plain, unsigned n, uint8_t *out)
{
    struct b2l_aes128 k;
    uint8_t a[8], b[16];
    b2l_aes128_key(&k, kek);
    memset(a, 0xa6, 8);
    memcpy(out + 8, plain, 8 * (size_t)n);
    for (unsigned j = 0; j < 6; j++) {
        for (unsigned i = 1; i <= n; i++) {
            memcpy(b, a, 8);
            memcpy(b + 8, out + 8 * i, 8);
            b2l_aes128_encrypt(&k, b, b);
            const uint64_t t = (uint64_t)n * j + i;
            for (unsigned q = 0; q < 8; q++) a[q] = b[q] ^ (uint8_t)(t >> (56 - 8 * q));
            memcpy(out + 8 * i, b + 8, 8);
        }
    }
    memcpy(out, a, 8);
    memset(&k, 0, sizeof(k));
    memset(b, 0, sizeof(b));
}

/* RFC 3394 unwrap of n + 1 blocks into n; 0 unless the integrity check holds. */
B2L_WIFI_CRYPTO int
b2l_aes_unwrap(const uint8_t kek[16], const uint8_t *wrapped, unsigned n, uint8_t *out)
{
    struct b2l_aes128 k;
    uint8_t a[8], b[16];
    if (n < 2) return 0;
    b2l_aes128_key(&k, kek);
    memcpy(a, wrapped, 8);
    memcpy(out, wrapped + 8, 8 * (size_t)n);
    for (int j = 5; j >= 0; j--) {
        for (unsigned i = n; i >= 1; i--) {
            const uint64_t t = (uint64_t)n * (unsigned)j + i;
            for (unsigned q = 0; q < 8; q++) b[q] = a[q] ^ (uint8_t)(t >> (56 - 8 * q));
            memcpy(b + 8, out + 8 * (i - 1), 8);
            b2l_aes128_decrypt(&k, b, b);
            memcpy(a, b, 8);
            memcpy(out + 8 * (i - 1), b + 8, 8);
        }
    }
    memset(&k, 0, sizeof(k));
    memset(b, 0, sizeof(b));
    int ok = 1;
    for (unsigned q = 0; q < 8; q++) ok &= a[q] == 0xa6;
    if (!ok) memset(out, 0, 8 * (size_t)n);
    return ok;
}

#endif /* B2L_WIFI_CRYPTO_H */
