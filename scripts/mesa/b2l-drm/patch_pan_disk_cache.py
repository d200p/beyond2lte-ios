#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""S10 port, M4: Panfrost's disk shader cache without an ELF build-id.

Boot 83: backboardd compiles Core Animation's generic fragment shader
(fixed_frag_lph_cpf, about 26,000 NIR instructions) at every boot, 21 s of
Bifrost compiling before the first frame. Panfrost keeps compiled variants
in Mesa's disk cache (pan_disk_cache.c) when the build has shader-cache
enabled, but it names the driver by the ELF build-id note of its own code
(build_id_find_nhdr_for_addr), which a Mach-O library does not have: the
note is NULL, and a release build (no assertions) would read through it.

The driver's name becomes a SHA-1 the build script computes over the Mesa
release and every file under scripts/mesa (our frontend, target and patches
to Panfrost), so any change to what compiles shaders gives a new cache. As
B2L_IDVS changes the programs Panfrost makes (patch_pan_shader_idvs.py), it
joins the flags Panfrost already folds into the key.

Usage: patch_pan_disk_cache.py src/gallium/drivers/panfrost/pan_disk_cache.c SHA1
"""
import re
import sys

path, cache_id = sys.argv[1], sys.argv[2]
if not re.fullmatch(r'[0-9a-f]{40}', cache_id):
    sys.exit(f'pan_disk_cache.c: the cache id must be 40 hex digits, not {cache_id!r}')
text = open(path, encoding='utf-8', newline='').read()
if 'S10 port: no ELF build-id' in text:
    print('pan_disk_cache.c: already patched')
    sys.exit(0)

old_id = ('   const struct build_id_note *note =\n'
          '      build_id_find_nhdr_for_addr(panfrost_disk_cache_init);\n'
          '   assert(note && build_id_length(note) == 20); /* sha1 */\n'
          '\n'
          '   const uint8_t *id_sha1 = build_id_data(note);\n'
          '   assert(id_sha1);\n'
          '\n'
          '   char timestamp[41];\n'
          '   _mesa_sha1_format(timestamp, id_sha1);\n')
new_id = ('   /* S10 port: no ELF build-id in a Mach-O library; the build names the\n'
          '    * driver by a SHA-1 of the Mesa release and scripts/mesa\n'
          '    * (patch_pan_disk_cache.py). */\n'
          '   const char *timestamp = "%s";\n' % cache_id)
old_flags = ('   driver_flags |= ((uint64_t)(midgard_debug | bifrost_debug) << 32);\n')
new_flags = ('   driver_flags |= ((uint64_t)(midgard_debug | bifrost_debug) << 32);\n'
             '   if (getenv("B2L_IDVS") != NULL) /* S10 port: other programs */\n'
             '      driver_flags |= 1ull << 63;\n')
old_include = '#include <string.h>\n'
new_include = '#include <stdlib.h>\n#include <string.h>\n'
# Boot 85: the cache was written (part0 of mesa_shader_cache_db held 174 KB)
# but the next boot compiled Core Animation's generic shader again: the NIR's
# SHA-1 is not the same from one boot to the next. A shader the Metal layer
# names "b2l:" and 40 hex digits (b2l_mtl.c: the SHA-1 of the functions'
# bitcode and constants and the pipeline's descriptor, which made it) is
# keyed by that name instead.
old_key = '   memcpy(data, uncompiled->nir_sha1, sizeof(uncompiled->nir_sha1));\n'
new_key = ('   memcpy(data, uncompiled->nir_sha1, sizeof(uncompiled->nir_sha1));\n'
           '   /* S10 port: a shader named "b2l:" and 40 hex digits by the Metal layer is\n'
           '    * keyed by that name, what it was made from (patch_pan_disk_cache.py). */\n'
           '   const char *b2l_name = uncompiled->nir != NULL ? uncompiled->nir->info.name : NULL;\n'
           '   if (b2l_name != NULL && strncmp(b2l_name, "b2l:", 4) == 0 && strlen(b2l_name) == 44) {\n'
           '      for (unsigned i = 0; i < 20 && i < sizeof(uncompiled->nir_sha1); i++) {\n'
           '         unsigned v = 0;\n'
           '         for (unsigned h = 0; h < 2; h++) {\n'
           '            const char c = b2l_name[4 + 2 * i + h];\n'
           '            v = v * 16 + (c >= \'a\' ? (unsigned)(c - \'a\' + 10) : (unsigned)(c - \'0\'));\n'
           '         }\n'
           '         data[i] = (uint8_t)v;\n'
           '      }\n'
           '   }\n')
for old, new in ((old_id, new_id), (old_flags, new_flags), (old_include, new_include), (old_key, new_key)):
    if text.count(old) != 1:
        sys.exit(f'pan_disk_cache.c: expected code not found:\n{old}')
    text = text.replace(old, new, 1)
open(path, 'w', encoding='utf-8', newline='').write(text)
print(f'pan_disk_cache.c: driver named {cache_id}')
