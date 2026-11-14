#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""S10 port: let B2L_NOIDVS turn Panfrost's IDVS off, per shader compile.

Usage: patch_pan_shader_idvs.py MESA/src/gallium/drivers/panfrost/pan_shader.c

Boots 41-43 on the Mali-G76 (r0p0): every IDVS draw tiled the positions its
position shader found in memory before it ran (zeros in a fresh buffer, the
previous draw's positions otherwise). BIFROST_MESA_DEBUG=noidvs is read once
per process, so the probe could not compare; with this, any compile made
while B2L_NOIDVS is set builds separate vertex and tiler jobs instead.
Idempotent.
"""
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
if 'B2L_NOIDVS' in text:
    sys.exit(0)
old = 'inputs.no_idvs = s->info.has_transform_feedback_varyings;'
new = ('inputs.no_idvs = s->info.has_transform_feedback_varyings ||\n'
       '                       getenv("B2L_NOIDVS") != NULL; /* S10 port */')
if text.count(old) != 1:
    sys.exit('pan_shader.c: no_idvs assignment not found')
text = text.replace(old, new)
first = text.index('#include')
text = text[:first] + '#include <stdlib.h> /* S10 port: getenv */\n' + text[first:]
path.write_text(text)
print('pan_shader.c: B2L_NOIDVS turns IDVS off')
