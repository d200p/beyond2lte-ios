#!/usr/bin/env python3
"""The cleaned part of a texture must hold every texel a draw can sample."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

SOURCE = r"""
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "b2l_flush_rect.h"

int main(void) {
 uint32_t first = 1440, last = 0;
 b2l_flush_span(0.25f, 0.5f, 1440, &first, &last);
 assert(first == 358 && last == 723);
 b2l_flush_span(0.75f, 0.5f, 1440, &first, &last);          /* flipped, and a union */
 assert(first == 358 && last == 1083);
 first = 100; last = 0;
 b2l_flush_span(0.0f, 1.0f, 100, &first, &last);
 assert(first == 0 && last == 100);
 first = 100; last = 0;
 b2l_flush_span(NAN, 0.5f, 100, &first, &last);
 assert(first == 0 && last == 100);
 first = 100; last = 0;
 b2l_flush_span(0.0f, 9.0f, 100, &first, &last);
 assert(first == 0 && last == 100);
 /* Every sampled texel is inside, with the linear filter's neighbour. */
 for (int size = 16; size < 3000; size += 37) {
  for (float a = 0; a < 1; a += 0.0625f) {
   for (float b = a; b <= 1; b += 0.0625f) {
    first = (uint32_t)size; last = 0;
    b2l_flush_span(a, b, (uint32_t)size, &first, &last);
    int low = (int)floorf(a * size) - 1, high = (int)ceilf(b * size) + 1;
    assert((int)first <= (low < 0 ? 0 : low));
    assert((int)last >= (high > size ? size : high));
   }
  }
 }
 puts("PASS");
 return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory() as work:
        work = pathlib.Path(work)
        (work / 'flush_test.c').write_text(SOURCE)
        built = subprocess.run(['cc', '-Wall', '-I', str(ROOT / 'userspace'), '-o', str(work / 'flush_test'),
                                str(work / 'flush_test.c')], capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        run = subprocess.run([str(work / 'flush_test')], capture_output=True, text=True)
        assert run.returncode == 0 and run.stdout.strip() == 'PASS', run.stdout + run.stderr[-800:]
    print('flush rect tests passed')


if __name__ == '__main__':
    main()
