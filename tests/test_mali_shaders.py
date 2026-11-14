#!/usr/bin/env python3
"""Check the generated Bifrost shaders and the render-pass builder.

There is no Bifrost disassembler in this repository. The clause fields are
decoded here independently of the encoder, from the quadword layout, so an
encoder change that moves a field fails. That the encodings mean what they
should is shown by `b2l-mali-probe draw`, which runs them on the real GPU.
"""
from pathlib import Path
import importlib.util, re, subprocess, sys, tempfile
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('gen', ROOT / 'scripts/gen-mali-shaders.py')
gen = importlib.util.module_from_spec(spec); spec.loader.exec_module(gen)

subprocess.run([sys.executable, str(ROOT / 'scripts/gen-mali-shaders.py'), '--check'], check=True)


def header_of(q):
    """Clause header from a first quadword: 30 bits at 83, 15 bits at 113."""
    lo, hi = q
    return ((hi >> 19) & ((1 << 30) - 1)) | ((hi >> 49) & 0x7fff) << 30


def field(h, start, size):
    return (h >> start) & ((1 << size) - 1)


def tuple_of(q):
    """Tuple from a first quadword, including its top 3 bits in the sync byte."""
    lo, hi = q
    s0_s3 = (lo >> 8) | (hi & 15) << 56
    s4 = (hi >> 4) & 0x7fff
    return s0_s3 | s4 << 60 | (lo & 7) << 75


# textured: one clause, BLEND of r0 with coverage r60 and the FAU descriptor.
quads = gen.textured()
assert len(quads) == 1
h = header_of(quads[0])
assert field(h, 11, 3) == 0, 'flow must end the shader'
assert field(h, 18, 6) == 0, 'staging register'
assert field(h, 35, 5) == 9, 'BLEND message'
t = tuple_of(quads[0])
assert t & 0xff == 8, 'FAU slot of the blend descriptor'
assert field(t, 20, 5) == 60 & 31 and field(t, 25, 1) == 60 >> 5, 'r60 on port 0'
assert field(t, 35, 23) == 0x701963, 'FMA NOP'
assert field(t, 58, 20) == 0xca800 | 4 << 3 | 5 << 6, 'BLEND with FAU lo/hi'
assert (quads[0][0] >> 6) & 3 == 1, 'clause ends after this quadword'

# solid: three tuples then the blend clause, which it waits for.
quads = gen.solid()
assert len(quads) == 4
h = header_of(quads[0])
assert field(h, 11, 3) == 3 and field(h, 16, 1) == 1
assert field(h, 24, 8) == 0xc0, 'wait on the tile buffer slots 6 and 7'
assert field(h, 40, 5) == 9, 'next message is BLEND'
assert field(tuple_of(quads[0]), 0, 8) == 0x80, 'first push uniform slot'
assert field(tuple_of(quads[0]), 35, 23) == 0x701968 | 4, 'r0 from FAU lo'
assert field(tuple_of(quads[0]), 58, 20) == 0x3d968 | 5, 'r1 from FAU hi'
assert (quads[2][0] >> 6) & 3 == 1 and not (quads[0][0] >> 6) & 3
assert quads[3] == gen.textured()[0]

# modulated: four multiplies by uniform 0, then the blend clause.
quads = gen.modulated()
assert len(quads) == 4 and quads[3] == gen.textured()[0]
h = header_of(quads[0])
assert field(h, 11, 3) == 3 and field(h, 24, 8) == 0xc0 and field(h, 40, 5) == 9
t = tuple_of(quads[0])
assert t & 0xff == 0x80, 'uniform slot 0'
assert field(t, 8, 6) == 3 and field(t, 14, 6) == 3, 'first tuple carries the write to r3'
assert field(t, 20, 5) == 0 and field(t, 35, 23) == 0x160, 'r0 * lo + hi'
assert (quads[2][0] >> 6) & 3 == 1, 'clause ends on its third quadword'

# Every quadword reaches the header exactly as the encoder generated it.
text = (ROOT / 'xnu-overlay/iokit/Kernel/arm/b2l_mali_shaders.h').read_text()
assert len(re.findall(r'0x[0-9a-f]{16}ull', text)) == 2 * (len(gen.tinted()) + len(gen.solid()) + len(gen.textured()) + len(gen.modulated()))

TEST = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "b2l_mali_draw.h"
static _Alignas(4096) uint8_t frame[B2L_MALI_PASS_BYTES(3)];
int main(void) {
 const uint64_t va = 0x1000000;
 struct b2l_mali_target rt = { 0x2000000, 1440, 2904, 5760, 0xff2040c0, B2L_RT_BGRA, 0x3000000, 1u << 24 };
 struct b2l_mali_quad q = { 1, 2, 3, 4, 0, 0, 1, 1, { 1, 0, 0, 1 }, 0x4000000, 0x5000000,
  256, 128, 1024, B2L_FORMAT_BGRA8_UNORM, B2L_EQ_SRC_OVER, 0, 1, 0, 0 };
 struct b2l_mali_pass pass;
 memset(frame, 0xa5, sizeof frame);
 b2l_mali_pass_begin(&pass, frame, va, &rt);
 uint32_t *fbd = (uint32_t *)(frame + B2L_MALI_PASS_FRAGMENT + B2L_MALI_FBD_OFFSET);
 uint32_t *rtd = (uint32_t *)(frame + B2L_MALI_PASS_FRAGMENT + B2L_MALI_RT_OFFSET);
 uint32_t *fragment = (uint32_t *)(frame + B2L_MALI_PASS_FRAGMENT);
 assert(fbd[14] == va + B2L_F_TILER && ((rtd[1] >> 16) & 0xfff) == B2L_RT_BGRA);
 assert(fragment[10] == ((uint32_t)(va + B2L_MALI_PASS_FRAGMENT + B2L_MALI_FBD_OFFSET) | 1));
 b2l_mali_pass_add(&pass, &q);
 q.texture_va = 0; q.equation = B2L_EQ_COPY; q.x0 = b2l_int_float(50); q.fau_slots = 2; q.first_clause_blends = 0;
 b2l_mali_pass_add(&pass, &q);
 uint32_t *d0 = (uint32_t *)(frame + B2L_MALI_PASS_DRAWS);
 uint32_t *d1 = d0 + B2L_MALI_DRAW_BYTES / 4;
 const uint64_t va0 = va + B2L_MALI_PASS_DRAWS, va1 = va0 + B2L_MALI_DRAW_BYTES;
 /* Chain: job 1 links to job 2, which depends on it; the chain then ends. */
 assert(((d0[4] >> 1) & 0x7f) == B2L_JOB_TILER && (d0[4] >> 16) == 1 && d0[5] == 0);
 assert(d0[6] == va1 && d0[7] == 0);
 assert((d1[4] >> 16) == 2 && (d1[5] & 0xffff) == 1 && d1[6] == 0 && d1[7] == 0);
 assert((d0[10] & 0xff) == B2L_DRAW_STRIP && d0[18] == va + B2L_F_TILER);
 uint32_t *draw = d0 + 32, *draw1 = d1 + 32;
 assert(draw[4] == va0 + B2L_D_POSITIONS && draw[14] == va0 + B2L_D_RSD);
 assert(draw[8] == va0 + B2L_D_TEXTURE && draw[22] == va0 + B2L_D_VARYING);
 assert(draw1[14] == va1 + B2L_D_RSD && draw1[24] == va + B2L_F_VIEWPORT);
 uint32_t *rsd = (uint32_t *)((uint8_t *)d0 + B2L_D_RSD), *blend = (uint32_t *)((uint8_t *)d0 + B2L_D_BLEND);
 assert(blend == rsd + 16 && rsd[0] == 0x4000000 && (rsd[4] >> 30) == 3 && (rsd[15] & 15) == 2);
 assert(blend[0] & 1 && blend[1] == B2L_EQ_SRC_OVER);
 uint32_t *t = (uint32_t *)((uint8_t *)d0 + B2L_D_TEXTURE);
 assert((t[1] & 0xffff) == 255 && (t[1] >> 16) == 127 && (t[0] >> 10) == B2L_FORMAT_BGRA8_UNORM);
 /* A solid draw must not inherit texture state from its neighbour. */
 uint32_t *rsd1 = (uint32_t *)((uint8_t *)d1 + B2L_D_RSD), *blend1 = rsd1 + 16;
 assert(!draw1[8] && !draw1[22] && !rsd1[2] && !rsd1[3] && !rsd1[15] && !(rsd1[4] >> 30) && !(blend1[0] & 1));
 assert(((uint32_t *)((uint8_t *)d1 + B2L_D_PUSH))[0] == 1 && ((float *)((uint8_t *)d1 + B2L_D_POSITIONS))[0] == 50.0f);
 /* The integer conversion agrees with the compiler's floats. */
 for (uint32_t v = 0; v < 70000; v++) { float f = (float)v; assert(!memcmp(&f, &(uint32_t){ b2l_int_float(v) }, 4)); }
 puts("PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'draw_test.c'
    src.write_text(TEST)
    exe = Path(tmp) / 'draw_test'
    subprocess.run(['clang', '-std=gnu11', '-g', '-Wall', '-Wextra', '-Werror', 
                    '-fsanitize=address,undefined',
                    '-I', str(ROOT / 'xnu-overlay/iokit/Kernel/arm'),
                    str(src), '-o', str(exe)], check=True)
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert run.returncode == 0 and run.stdout.strip() == 'PASS', run.stdout + run.stderr[-1200:]
print('mali shader tests passed')
