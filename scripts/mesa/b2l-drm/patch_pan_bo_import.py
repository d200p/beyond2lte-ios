#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""S10 port: panfrost_bo_import returns NULL when the import fails.

Boot 58: the kernel refused an IOSurface import (kIOReturnNoMemory, its
buffer-object limit reached) and backboardd died at NULL + 8 in
panfrost_bo_import. Mesa only asserts on drmPrimeFDToHandle's result (no
assertions in release builds), then uses an uninitialised handle, and uses
pan_kmod_bo_import's result without checking it. Its callers already handle a
NULL BO (an import can fail, says pan_resource.c), so both failures now
unlock and return NULL.

Usage: patch_pan_bo_import.py src/gallium/drivers/panfrost/pan_bo.c
"""
import sys

path = sys.argv[1]
text = open(path, encoding='utf-8', newline='').read()
if 'S10 port: a refused import' in text:
    print('pan_bo.c: already patched')
    sys.exit(0)

old_prime = ('   ret = drmPrimeFDToHandle(dev->kmod.dev->fd, fd, &gem_handle);\n'
             '   assert(!ret);\n')
new_prime = ('   ret = drmPrimeFDToHandle(dev->kmod.dev->fd, fd, &gem_handle);\n'
             '   if (ret) { /* S10 port: a refused import fails, as callers expect */\n'
             '      pthread_mutex_unlock(&dev->bo_map_lock);\n'
             '      return NULL;\n'
             '   }\n')
old_kmod = ('      bo->dev = dev;\n'
            '      bo->kmod_bo = pan_kmod_bo_import(dev->kmod.dev, fd, 0);\n')
new_kmod = ('      bo->kmod_bo = pan_kmod_bo_import(dev->kmod.dev, fd, 0);\n'
            '      if (bo->kmod_bo == NULL) {\n'
            '         pthread_mutex_unlock(&dev->bo_map_lock);\n'
            '         return NULL;\n'
            '      }\n'
            '      bo->dev = dev;\n')
for old, new in ((old_prime, new_prime), (old_kmod, new_kmod)):
    if text.count(old) != 1:
        sys.exit(f'pan_bo.c: expected code not found:\n{old}')
    text = text.replace(old, new)
open(path, 'w', encoding='utf-8', newline='').write(text)
print('pan_bo.c: refused imports return NULL')
