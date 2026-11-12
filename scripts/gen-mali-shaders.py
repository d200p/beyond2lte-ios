#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Encode the fixed Bifrost fragment shaders Core Animation needs.

There is no shader compiler in this project. The GPU path only needs a few
fragment programs, so they are written out here as clauses of 78-bit tuples.
Encodings follow the Bifrost clause format: a 45-bit header, per-tuple register
blocks where a tuple's register writes are carried by the following tuple, and
the quadword layouts selected by the clause's tuple count.

Writes xnu-overlay/iokit/Kernel/arm/b2l_mali_shaders.h. --check verifies that
the committed header matches this script.
"""
import pathlib
import sys

OUT = pathlib.Path(__file__).resolve().parents[1] / 'xnu-overlay/iokit/Kernel/arm/b2l_mali_shaders.h'

FMA_NOP = 0x701963
FMA_MOV = 0x701968
FMA_F32 = 0x000000             # a * b + c, sources at bits 0, 3 and 6
ADD_NOP = 0x3D964
ADD_MOV = 0x3D968
ADD_BLEND = 0xCA800

PORT0, ZERO_FMA, FAU_LO, FAU_HI = 0, 3, 4, 5
FAU_BLEND0 = 0x08
FAU_UNIFORM = 0x80
COVERAGE_REG = 60               # preloaded by the "coverage" RSD bit

MODE_W_W_ADD = 15               # slot 2 written from FMA, slot 3 from ADD
MODE_IDLE_1 = 16
MODE_I_W_FMA = 17               # slot 3 written from FMA, slots 2 and 3 equal
MODE_IDLE = 27

FLOW_END, FLOW_NBTB = 0, 3
MESSAGE_NONE, MESSAGE_BLEND = 0, 9
WAIT_TILEBUFFER = (1 << 6) | (1 << 7)


def registers(mode, first=False, fau=0, read=None, slot2=0, slot3=0):
    """A 35-bit register block; only one read port is ever needed here."""
    ctrl = ((mode & 7) | ((mode & 0x10) >> 1)) if first else mode & 0xF
    if read is None:
        reg0, reg1 = 0, (ctrl << 2) | 2     # bit 1: port 0 disabled
    else:
        reg0, reg1 = read & 31, (ctrl << 2) | (read >> 5)
    return fau | slot3 << 8 | slot2 << 14 | reg0 << 20 | reg1 << 25


def tuple78(regs, fma=FMA_NOP, add=ADD_NOP):
    return regs | fma << 35 | add << 58


def header(flow, message=MESSAGE_NONE, next_message=MESSAGE_NONE, wait=0, staging=0,
           prefetch=False):
    td = 1      # no derivatives in these shaders, discarded threads may end
    return (flow << 11 | td << 15 | int(prefetch) << 16 | staging << 18 | wait << 24 |
            message << 35 | next_message << 40)


def bits(value, start, count):
    return (value >> start) & ((1 << count) - 1)


def upper(t):
    return bits(t, 75, 3)


def quad(sync, s0_s3, s4, s5_s6, s7):
    lo = sync | (s0_s3 & ((1 << 56) - 1)) << 8
    hi = s0_s3 >> 56 | s4 << 4 | s5_s6 << 19 | s7 << 49
    return lo, hi


def clause_1(h, t0):
    """One tuple: format 0 at position 0, ending the clause."""
    sync = upper(t0) | 1 << 3 | 1 << 6
    return [quad(sync, bits(t0, 0, 60), bits(t0, 60, 15), bits(h, 0, 30), bits(h, 30, 15))]


def clause_3(h, t0, t1, t2):
    """Three tuples: formats 0, 2 and 3, with an unused embedded constant."""
    return [
        quad(upper(t0) | 5 << 3, bits(t0, 0, 60), bits(t0, 60, 15), bits(h, 0, 30), bits(h, 30, 15)),
        quad(upper(t1) | 4 << 3, bits(t1, 0, 60), bits(t1, 60, 15), bits(t2, 0, 30), bits(t2, 30, 15)),
        quad(4 | 1 << 6, 0, 0, bits(t2, 45, 30), upper(t2) << 12),
    ]


def clause_4(h, t0, t1, t2, t3):
    """Four tuples: formats 0, 2 and 4; no embedded constant."""
    return [
        quad(upper(t0) | 5 << 3, bits(t0, 0, 60), bits(t0, 60, 15), bits(h, 0, 30), bits(h, 30, 15)),
        quad(upper(t1) | 4 << 3, bits(t1, 0, 60), bits(t1, 60, 15), bits(t2, 0, 30), bits(t2, 30, 15)),
        quad(5 | 1 << 6, bits(t3, 0, 60), bits(t3, 60, 15), bits(t2, 45, 30),
             upper(t2) << 12 | upper(t3) << 9),
    ]


def blend_tuple():
    """BLEND r0..r3 with the coverage in r60, descriptor from FAU."""
    return tuple78(registers(MODE_IDLE_1, first=True, fau=FAU_BLEND0, read=COVERAGE_REG),
                   add=ADD_BLEND | PORT0 | FAU_LO << 3 | FAU_HI << 6)


def blend_clause():
    return clause_1(header(FLOW_END, MESSAGE_BLEND), blend_tuple())


def solid():
    """r0..r3 = push uniforms 0..3 (two 64-bit FAU slots), then blend."""
    t0 = tuple78(registers(MODE_IDLE_1, first=True, fau=FAU_UNIFORM | 0),
                 FMA_MOV | FAU_LO, ADD_MOV | FAU_HI)
    t1 = tuple78(registers(MODE_W_W_ADD, fau=FAU_UNIFORM | 1, slot2=0, slot3=1),
                 FMA_MOV | FAU_LO, ADD_MOV | FAU_HI)
    t2 = tuple78(registers(MODE_W_W_ADD, slot2=2, slot3=3))
    h = header(FLOW_NBTB, next_message=MESSAGE_BLEND, wait=WAIT_TILEBUFFER, prefetch=True)
    return clause_3(h, t0, t1, t2) + blend_clause()


def textured():
    """Blend r0..r3, which the RSD message preload fills from the texture."""
    return blend_clause()


def modulated():
    """r0..r3 from the texture, each times push uniform 0, then blend.

    The uniform slot holds (opacity, 0.0): FMA reads the factor from its low
    word and the zero addend from its high word. Each tuple reads one register
    and its register block carries the previous tuple's write; the first tuple
    carries the last one's.
    """
    fau = FAU_UNIFORM | 0
    multiply = FMA_F32 | PORT0 | FAU_LO << 3 | FAU_HI << 6
    t0 = tuple78(registers(MODE_I_W_FMA, first=True, fau=fau, read=0, slot2=3, slot3=3), multiply)
    t1 = tuple78(registers(MODE_I_W_FMA, fau=fau, read=1, slot2=0, slot3=0), multiply)
    t2 = tuple78(registers(MODE_I_W_FMA, fau=fau, read=2, slot2=1, slot3=1), multiply)
    t3 = tuple78(registers(MODE_I_W_FMA, fau=fau, read=3, slot2=2, slot3=2), multiply)
    h = header(FLOW_NBTB, next_message=MESSAGE_BLEND, wait=WAIT_TILEBUFFER, prefetch=True)
    return clause_4(h, t0, t1, t2, t3) + blend_clause()


def tinted():
    """r0..r3 from the texture, each times its own push uniform, then blend.

    Like modulated, but all four words of the two uniform slots are factors, so
    the addend is FMA's constant zero.
    """
    def multiply(half):
        return FMA_F32 | PORT0 | half << 3 | ZERO_FMA << 6
    t0 = tuple78(registers(MODE_I_W_FMA, first=True, fau=FAU_UNIFORM | 0, read=0, slot2=3, slot3=3), multiply(FAU_LO))
    t1 = tuple78(registers(MODE_I_W_FMA, fau=FAU_UNIFORM | 0, read=1, slot2=0, slot3=0), multiply(FAU_HI))
    t2 = tuple78(registers(MODE_I_W_FMA, fau=FAU_UNIFORM | 1, read=2, slot2=1, slot3=1), multiply(FAU_LO))
    t3 = tuple78(registers(MODE_I_W_FMA, fau=FAU_UNIFORM | 1, read=3, slot2=2, slot3=2), multiply(FAU_HI))
    h = header(FLOW_NBTB, next_message=MESSAGE_BLEND, wait=WAIT_TILEBUFFER, prefetch=True)
    return clause_4(h, t0, t1, t2, t3) + blend_clause()


SHADERS = {'solid': solid, 'textured': textured, 'modulated': modulated, 'tinted': tinted}


def render():
    out = ['/* SPDX-License-Identifier: BSD-2-Clause',
           ' * Generated by scripts/gen-mali-shaders.py; do not edit.',
           ' * Bifrost fragment shaders, each a list of 128-bit quadwords. */',
           '#ifndef _B2L_MALI_SHADERS_H', '#define _B2L_MALI_SHADERS_H', '', '#include <stdint.h>', '']
    for name, build in SHADERS.items():
        out.append(f'static const uint64_t b2l_mali_shader_{name}[] = {{')
        out += [f'\t0x{lo:016x}ull, 0x{hi:016x}ull,' for lo, hi in build()]
        out += ['};', '']
    out += ['#endif /* _B2L_MALI_SHADERS_H */', '']
    return '\n'.join(out)


def main():
    text = render()
    if '--check' in sys.argv:
        sys.exit(0 if OUT.read_text() == text else f'{OUT} is stale')
    OUT.write_text(text)
    print(OUT)


if __name__ == '__main__':
    main()
