#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Compile the actual b2l_hid_touch_display_y from B2LHID.cpp and check the camera-band remap."""
import pathlib, re, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
src = (ROOT / 'xnu-overlay/iokit/Kernel/arm/hid/B2LHID.cpp').read_text()
m = re.search(r'extern "C" int\nb2l_hid_touch_display_y\(.*?\n}\n', src, re.S)
assert m, 'function not found'
PROGRAM = '#include <assert.h>\n#include <stdint.h>\n#include <stdio.h>\n' + m.group(0).replace('extern "C" ', '') + r'''
int main(void){
 /* No band: identity. */
 for(int y=0;y<=4095;y+=7)assert(b2l_hid_touch_display_y(y,0,3040)==y);
 /* Band of 136 rows on a 3040-row panel. */
 assert(b2l_hid_touch_display_y(0,136,3040)==0);
 assert(b2l_hid_touch_display_y(100,136,3040)==0);          /* inside the band: top edge */
 assert(b2l_hid_touch_display_y(4095,136,3040)==4095);      /* bottom stays bottom */
 int prev=0;
 for(int y=0;y<=4095;y++){
  int d=b2l_hid_touch_display_y(y,136,3040);
  assert(d>=0&&d<=4095&&d>=prev);prev=d;                  /* monotonic, in range */
  double pixel=y*3040.0/4095-136, want=pixel<0?0:pixel*4095/2904;
  assert(d>=(int)want-2&&d<=(int)want+2);                 /* matches the pixel math */
 }
 /* Bad configurations fall back to the raw value. */
 assert(b2l_hid_touch_display_y(1234,3040,3040)==1234&&b2l_hid_touch_display_y(1234,136,0)==1234);
 puts("Touch inset: identity without band, band clamps to top, monotonic remap to the presented area PASS");
}
'''
with tempfile.TemporaryDirectory() as td:
    d = pathlib.Path(td); (d / 't.c').write_text(PROGRAM)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(d / 't.c'), '-o', str(d / 't')], check=True)
    subprocess.run([str(d / 't')], check=True)
