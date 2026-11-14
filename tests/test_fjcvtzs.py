#!/usr/bin/env python3
"""FJCVTZS against the ToInt32 definition, computed exactly with Python integers."""
import math
import pathlib
import random
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent


def expected(bits):
    value = struct.unpack('<d', struct.pack('<Q', bits))[0]
    if math.isnan(value) or math.isinf(value):
        return 0, 0
    if value == 0:
        return 0, 0 if math.copysign(1, value) < 0 else 1
    whole = math.trunc(value)
    exact = int(whole == value and -2**31 <= whole <= 2**31 - 1)
    return whole & 0xffffffff, exact


def main():
    rng = random.Random(0x9820)
    cases = [struct.unpack('<Q', struct.pack('<d', v))[0] for v in (
        0.0, -0.0, 1.0, -1.0, 1.5, -1.5, 0.999, 2147483647.0, 2147483648.0, -2147483648.0, -2147483649.0,
        4294967295.0, 4294967296.0, 4294967297.5, 1e10, -1e10, 1e300, -1e300, 5e-324, 2.0 ** 52, 2.0 ** 53, 2.0 ** 62,
        2.0 ** 63, 2.0 ** 64, 2.0 ** 84, 2.0 ** 85, -(2.0 ** 83), float('inf'), float('-inf'), float('nan'))]
    cases += [rng.getrandbits(64) for _ in range(20000)]
    cases += [(rng.getrandbits(1) << 63) | (rng.randrange(1000, 1200) << 52) | rng.getrandbits(52) for _ in range(40000)]
    table = ',\n'.join('{0x%xull,0x%xu,%d}' % ((b,) + expected(b)) for b in cases)
    source = '''
#include <stdio.h>
#include "b2l_fjcvtzs.h"
static const struct { uint64_t bits; uint32_t value; int exact; } cases[] = { %s };
int main(void) {
 for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
  int exact; uint32_t value = b2l_fjcvtzs(cases[i].bits, &exact);
  if (value != cases[i].value || exact != cases[i].exact) {
   printf("0x%%llx: got 0x%%x %%d want 0x%%x %%d\\n", cases[i].bits, value, exact, cases[i].value, cases[i].exact);
   return 1;
  }
 }
 puts("PASS");
 return 0;
}
''' % table
    with tempfile.TemporaryDirectory() as work:
        work = pathlib.Path(work)
        (work / 't.c').write_text(source)
        built = subprocess.run(['cc', '-O1', '-fsanitize=undefined', '-I', str(ROOT / 'xnu-overlay/osfmk/arm64'), '-o',
                                str(work / 't'), str(work / 't.c')], capture_output=True, text=True)
        assert built.returncode == 0, built.stderr[-1500:]
        run = subprocess.run([str(work / 't')], capture_output=True, text=True)
        assert run.returncode == 0 and run.stdout.strip() == 'PASS', run.stdout + run.stderr[-800:]
    print('fjcvtzs tests passed')


if __name__ == '__main__':
    main()
