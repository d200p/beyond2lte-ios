#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""S10 port: panfrost_ptr_map returns NULL when its staging texture fails.

Boot 91: backboardd died at NULL + 0x148 in panfrost_ptr_map, called from
Core Animation's replaceRegion (texture_replace, u_default_texture_subdata).
An AFBC texture is written through a linear staging texture; with the GPU's
buffer-object memory used up, pan_alloc_staging returned NULL and Mesa only
asserts on it (no assertions in release builds). The callers handle a NULL
map (u_default_texture_subdata returns, b2l_mtl.c checks), so the map now
fails instead: that one upload is lost, the process lives.

Usage: patch_pan_resource_staging.py src/gallium/drivers/panfrost/pan_resource.c
"""
import sys

path = sys.argv[1]
text = open(path, encoding='utf-8', newline='').read()
if 'S10 port: a refused staging texture' in text:
    print('pan_resource.c: already patched')
    sys.exit(0)

old = ('      struct panfrost_resource *staging =\n'
       '         pan_alloc_staging(ctx, rsrc, level, box);\n'
       '      assert(staging);\n')
new = ('      struct panfrost_resource *staging =\n'
       '         pan_alloc_staging(ctx, rsrc, level, box);\n'
       '      if (!staging) { /* S10 port: a refused staging texture fails the map */\n'
       '         pipe_resource_reference(&transfer->base.resource, NULL);\n'
       '         ralloc_free(transfer);\n'
       '         *out_transfer = NULL;\n'
       '         return NULL;\n'
       '      }\n')
if text.count(old) != 1:
    sys.exit('pan_resource.c: the staging allocation in panfrost_ptr_map was not found once')
text = text.replace(old, new)
open(path, 'w', encoding='utf-8', newline='').write(text)
print('pan_resource.c: a refused staging texture fails the map')
