#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Prove the precompiler's keyless PACGA routine equals the kernel's C function.

1. Encoding: for every (d, n, m) the routine supports, the words produced by
   b2l_pacga_keyless_encode() equal the words the host assembler produces for
   the intended instruction text.
2. Semantics: the encoded routine is executed natively (Apple Silicon host,
   MAP_JIT page) for register aliasing cases and compared against
   b2l_pacga_keyless() for random and edge inputs.
"""
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

repo = pathlib.Path(__file__).resolve().parents[1]
header_dir = repo / 'xnu-overlay/osfmk/arm64'


def reg(r, zr='xzr'):
    return zr if r == 31 else f'x{r}'


def expected_asm(d, n, m):
    D = reg(d)
    return [f'eor {D}, {reg(n)}, {reg(m)}, ror #29', f'eor {D}, {D}, {D}, lsr #31',
            f'add {D}, {D}, {D}, lsl #21', f'eor {D}, {D}, {D}, lsr #29',
            f'add {D}, {D}, {D}, lsl #13', f'eor {D}, {D}, {D}, lsr #33',
            f'and {D}, {D}, #0xffffffff00000000']


def assemble(lines, tmp):
    src, obj = tmp / 'a.s', tmp / 'a.o'
    src.write_text('.text\n' + '\n'.join(lines) + '\n')
    subprocess.run(['clang', '-arch', 'arm64', '-c', str(src), '-o', str(obj)], check=True)
    data = obj.read_bytes()
    ncmds = struct.unpack_from('<I', data, 16)[0]
    at = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, at)
        if cmd == 0x19:
            for j in range(struct.unpack_from('<I', data, at + 64)[0]):
                s = at + 72 + j * 80
                if data[s:s + 16].rstrip(b'\0') == b'__text':
                    n, off = struct.unpack_from('<QI', data, s + 40)
                    return list(struct.unpack('<%dI' % (n // 4), data[off:off + n]))
        at += size
    raise AssertionError('no __text')


PROGRAM = r'''
#include <assert.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "b2l_pacga_keyless.h"
typedef uint64_t (*fn)(uint64_t, uint64_t);
static uint32_t *page;
/* Run the routine for PACGA x0, Xn, Xm where the inputs arrive in x0/x1 per the
 * C ABI; the cases cover n and m aliasing the destination and XZR for Rn. */
static uint64_t run(unsigned n, unsigned m, uint64_t a, uint64_t b)
{
    uint32_t seq[B2L_PACGA_KEYLESS_WORDS];
    assert(b2l_pacga_keyless_encode(0, n, m, seq));
    pthread_jit_write_protect_np(0);
    memcpy(page, seq, sizeof(seq));
    page[B2L_PACGA_KEYLESS_WORDS] = 0xd65f03c0u; /* ret */
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(page, 4096);
    return ((fn)page)(a, b);
}
int main(void)
{
    page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    assert(page != MAP_FAILED);
    uint64_t edge[] = {0, 1, 0x82c, UINT64_MAX, UINT64_C(0x8000000000000000), UINT64_C(0x00000001ffffffff),
                       UINT64_C(0x0000000180000000), UINT64_C(0x123456789abcdef0)};
    uint64_t rnd = UINT64_C(0x243f6a8885a308d3);
    unsigned checked = 0, distinct_low = 0;
    for (unsigned i = 0; i < 20000; i++) {
        uint64_t a, b;
        if (i < 64) { a = edge[i % 8]; b = edge[i / 8]; }
        else { rnd = rnd * UINT64_C(6364136223846793005) + 1442695040888963407u; a = rnd ^ (rnd >> 29);
               rnd = rnd * UINT64_C(6364136223846793005) + 1442695040888963407u; b = rnd; }
        /* PACGA x0, x0, x1: n aliases d. */
        uint64_t v = run(0, 1, a, b);
        assert(v == b2l_pacga_keyless(a, b) && !(v & UINT32_MAX));
        /* PACGA x0, x1, x0: m aliases d. */
        assert(run(1, 0, a, b) == b2l_pacga_keyless(b, a));
        /* PACGA x0, xzr, x1: Rn 31 reads as zero. */
        assert(run(31, 1, a, b) == b2l_pacga_keyless(0, b));
        if ((v >> 32) & 1) distinct_low++;
        checked += 3;
    }
    assert(distinct_low > 9000 && distinct_low < 11000); /* output bit 32 is balanced */
    uint32_t seq[B2L_PACGA_KEYLESS_WORDS];
    assert(!b2l_pacga_keyless_encode(31, 1, 2, seq) && !b2l_pacga_keyless_encode(0, 1, 31, seq));
    printf("native keyless routine matched C on %u executions\n", checked);
    return 0;
}
'''


def main():
    sys.path.insert(0, str(repo / 'tests'))
    with tempfile.TemporaryDirectory(prefix='b2l-keyless-') as name:
        tmp = pathlib.Path(name)
        # Encoder vs assembler for every supported register triple.
        enc_src = tmp / 'enc.c'
        enc_src.write_text('#include <stdio.h>\n#include "b2l_pacga_keyless.h"\nint main(void){uint32_t s[7];'
                           'for(unsigned d=0;d<32;d++)for(unsigned n=0;n<32;n++)for(unsigned m=0;m<32;m++){'
                           'if(!b2l_pacga_keyless_encode(d,n,m,s))continue;printf("%u %u %u",d,n,m);'
                           'for(unsigned k=0;k<7;k++)printf(" %08x",s[k]);printf("\\n");}return 0;}\n')
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(header_dir),
                        str(enc_src), '-o', str(tmp / 'enc')], check=True)
        rows = subprocess.run([str(tmp / 'enc')], capture_output=True, text=True, check=True).stdout.split('\n')
        rows = [r.split() for r in rows if r]
        assert len(rows) == 31 * 32 * 31, len(rows)
        lines = []
        for r in rows:
            lines += expected_asm(*map(int, r[:3]))
        words = assemble(lines, tmp)
        got = [int(w, 16) for r in rows for w in r[3:]]
        assert words == got, next((i, hex(a), hex(b)) for i, (a, b) in enumerate(zip(words, got)) if a != b)
        print(f'encoder matched assembler for {len(rows)} register triples ({len(words)} words)')
        # Native execution vs C.
        prog = tmp / 'run.c'
        prog.write_text(PROGRAM)
        subprocess.run(['clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-I' + str(header_dir),
                        str(prog), '-o', str(tmp / 'run')], check=True)
        subprocess.run([str(tmp / 'run')], check=True)


if __name__ == '__main__':
    if os.uname().machine != 'arm64':
        raise SystemExit('native execution requires an arm64 host')
    main()
