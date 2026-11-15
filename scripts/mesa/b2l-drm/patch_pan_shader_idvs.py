#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""S10 port: Panfrost without IDVS on this GPU; B2L_IDVS=1 brings it back.

Usage: patch_pan_shader_idvs.py MESA/src/gallium/drivers/panfrost/pan_shader.c

Boots 41-43 on the Mali-G76 (r0p0): every IDVS draw tiled the positions its
position shader found in memory before it ran (zeros in a fresh buffer, the
previous draw's positions otherwise). JM_CONFIG held the IDVS group size the
Android driver sets (0xf). Boot 44: with IDVS off (separate vertex and tiler
jobs) every probe target came out right, with AFBC and without. So vertex
shaders are compiled without IDVS unless B2L_IDVS is set (read per compile;
BIFROST_MESA_DEBUG=noidvs is read once per process). Idempotent.
"""
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
if 'B2L_IDVS' in text:
    sys.exit(0)
old = 'inputs.no_idvs = s->info.has_transform_feedback_varyings;'
new = ('inputs.no_idvs = s->info.has_transform_feedback_varyings ||\n'
       '                       getenv("B2L_IDVS") == NULL; /* S10 port: no IDVS on the G76 */')
if text.count(old) != 1:
    sys.exit('pan_shader.c: no_idvs assignment not found')
text = text.replace(old, new)
first = text.index('#include')
text = text[:first] + '#include <stdlib.h> /* S10 port: getenv */\n' + text[first:]
path.write_text(text)
print('pan_shader.c: IDVS off unless B2L_IDVS is set')
