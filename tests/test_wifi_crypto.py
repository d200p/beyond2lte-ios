#!/usr/bin/env python3
"""Check Wi-Fi mode 23's WPA2 primitives (xnu-overlay/osfmk/arm64/b2l_wifi_crypto.h).

SHA-1, HMAC-SHA1, PBKDF2-HMAC-SHA1 and the 802.11 PRF are compared with
Python's own hashlib and hmac on the same inputs; AES-128 with the FIPS 197
vectors (appendices B and C.1) and the key wrap with RFC 3394 4.1. The C side
only says PASS or which check failed.
"""
from pathlib import Path
import hashlib
import hmac
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "xnu-overlay/osfmk/arm64"


def c_bytes(data):
    return "{" + ",".join(f"0x{b:02x}" for b in data) + "}" if data else "{0}"


def prf(key, label, data, n):
    out = b""
    i = 0
    while len(out) < n:
        out += hmac.new(key, label.encode() + b"\0" + data + bytes([i]), hashlib.sha1).digest()
        i += 1
    return out[:n]


checks = []


def check(name, call, expected):
    checks.append((name, call, expected))


# SHA-1: empty, one block, the two-block padding edge, many blocks
for i, message in enumerate([b"", b"abc", b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                             bytes((7 * k + 3) & 0xff for k in range(1000))]):
    check(f"sha1 {i}", f"sha1_of(m{i}, sizeof(m{i}) - {0 if message else 1})", hashlib.sha1(message).digest())
    checks[-1] = checks[-1] + (("m%d" % i, message),)

hmac_cases = [
    (b"\x0b" * 20, [b"Hi There"]),
    (b"Jefe", [b"what do ya want ", b"for nothing?"]),
    (b"\xaa" * 80, [b"Test Using Larger Than Block-Size Key - Hash Key First"]),
    (bytes(range(64)), [b"", bytes(range(200)), b"tail"]),
]
pbkdf2_cases = [
    (b"password", b"salt", 1, 20),
    (b"password", b"salt", 4096, 20),
    (b"password", b"IEEE", 4096, 32),           # IEEE 802.11 J.4's first example
    (b"passwordPASSWORDpassword", b"saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096, 25),
]
prf_key = bytes((11 * k + 5) & 0xff for k in range(32))
prf_data = bytes((13 * k + 1) & 0xff for k in range(76))

body = []
for name, call, expected, *extra in checks:
    for var, data in extra:
        body.append(f" static const uint8_t {var}[] = {c_bytes(data)};")
    body.append(f" {{ static const uint8_t want[] = {c_bytes(expected)}; uint8_t got[20]; "
                f"memcpy(got, {call}, 20); CHECK(\"{name}\", !memcmp(got, want, 20)); }}")
for i, (key, parts) in enumerate(hmac_cases):
    expected = hmac.new(key, b"".join(parts), hashlib.sha1).digest()
    decl = [f" static const uint8_t hk{i}[] = {c_bytes(key)};"]
    names = []
    for j, part in enumerate(parts):
        decl.append(f" static const uint8_t hp{i}_{j}[] = {c_bytes(part)};")
        names.append((f"hp{i}_{j}", len(part)))
    body += decl
    body.append(f" {{ const uint8_t *parts[] = {{{','.join(n for n, _ in names)}}}; "
                f"const size_t lens[] = {{{','.join(str(l) for _, l in names)}}}; uint8_t got[20]; "
                f"static const uint8_t want[] = {c_bytes(expected)}; "
                f"b2l_hmac_sha1(hk{i}, sizeof(hk{i}), parts, lens, {len(names)}, got); "
                f"CHECK(\"hmac {i}\", !memcmp(got, want, 20)); }}")
for i, (password, salt, iterations, n) in enumerate(pbkdf2_cases):
    expected = hashlib.pbkdf2_hmac("sha1", password, salt, iterations, n)
    body.append(f" {{ static const uint8_t pw[] = {c_bytes(password)}, sa[] = {c_bytes(salt)}, "
                f"want[] = {c_bytes(expected)}; uint8_t got[{n}]; "
                f"b2l_pbkdf2_sha1(pw, sizeof(pw), sa, sizeof(sa), {iterations}, got, {n}); "
                f"CHECK(\"pbkdf2 {i}\", !memcmp(got, want, {n})); }}")
for n in (16, 48, 64):
    expected = prf(prf_key, "Pairwise key expansion", prf_data, n)
    body.append(f" {{ static const uint8_t k[] = {c_bytes(prf_key)}, d[] = {c_bytes(prf_data)}, "
                f"want[] = {c_bytes(expected)}; uint8_t got[{n}]; "
                f"b2l_prf_sha1(k, sizeof(k), \"Pairwise key expansion\", d, sizeof(d), got, {n}); "
                f"CHECK(\"prf {n}\", !memcmp(got, want, {n})); }}")

aes = [
    ("000102030405060708090a0b0c0d0e0f", "00112233445566778899aabbccddeeff", "69c4e0d86a7b0430d8cdb78070b4c55a"),
    ("2b7e151628aed2a6abf7158809cf4f3c", "3243f6a8885a308d313198a2e0370734", "3925841d02dc09fbdc118597196a0b32"),
]
for i, (key, plain, cipher) in enumerate(aes):
    body.append(f" {{ static const uint8_t k[] = {c_bytes(bytes.fromhex(key))}, p[] = {c_bytes(bytes.fromhex(plain))}, "
                f"c[] = {c_bytes(bytes.fromhex(cipher))}; struct b2l_aes128 x; uint8_t o[16]; b2l_aes128_key(&x, k); "
                f"b2l_aes128_encrypt(&x, p, o); CHECK(\"aes encrypt {i}\", !memcmp(o, c, 16)); "
                f"b2l_aes128_decrypt(&x, c, o); CHECK(\"aes decrypt {i}\", !memcmp(o, p, 16)); }}")
kek = bytes.fromhex("000102030405060708090A0B0C0D0E0F")
key_data = bytes.fromhex("00112233445566778899AABBCCDDEEFF")
wrapped = bytes.fromhex("1FA68B0A8112B447AEF34BD8FB5A7B829D3E862371D2CFE5")
body.append(f" {{ static const uint8_t k[] = {c_bytes(kek)}, p[] = {c_bytes(key_data)}, w[] = {c_bytes(wrapped)}; "
            "uint8_t o[24], back[16], bad[24]; b2l_aes_wrap(k, p, 2, o); CHECK(\"rfc3394 wrap\", !memcmp(o, w, 24)); "
            "CHECK(\"rfc3394 unwrap\", b2l_aes_unwrap(k, w, 2, back) && !memcmp(back, p, 16)); "
            "memcpy(bad, w, 24); bad[23] ^= 1; CHECK(\"rfc3394 tampered\", !b2l_aes_unwrap(k, bad, 2, back)); "
            "uint8_t gtk[32], wrapped32[40], gtk_back[32]; for (unsigned q = 0; q < 32; q++) gtk[q] = (uint8_t)(q * 7 + 1); "
            "b2l_aes_wrap(k, gtk, 4, wrapped32); "
            "CHECK(\"wrap round trip\", b2l_aes_unwrap(k, wrapped32, 4, gtk_back) && !memcmp(gtk, gtk_back, 32)); }")

source = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "b2l_wifi_crypto.h"
static int failed;
#define CHECK(name, ok) do { if (!(ok)) { printf("FAILED %s\n", name); failed = 1; } } while (0)
static uint8_t sha1_out[20];
static const uint8_t *sha1_of(const uint8_t *m, size_t n) {
    struct b2l_sha1 s; b2l_sha1_init(&s);
    /* in uneven pieces, so the block buffer is exercised */
    for (size_t at = 0; at < n; ) { size_t take = (at % 7) + 1; if (take > n - at) take = n - at;
        b2l_sha1_update(&s, m + at, take); at += take; }
    b2l_sha1_final(&s, sha1_out); return sha1_out;
}
int main(void) {
''' + "\n".join(body) + r'''
    if (!failed) puts("PASS");
    return failed;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "wifi_crypto.c"
    src.write_text(source)
    exe = Path(tmp) / "wifi_crypto"
    subprocess.run(["clang", "-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-fsanitize=address,undefined", "-I", str(HEADER), str(src), "-o", str(exe)], check=True)
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
    assert run.returncode == 0 and run.stdout.strip() == "PASS", run.stdout + run.stderr[-1500:]
print("Wi-Fi crypto: SHA-1, HMAC-SHA1, PBKDF2, the 802.11 PRF, AES-128 and RFC 3394 key wrap passed")
