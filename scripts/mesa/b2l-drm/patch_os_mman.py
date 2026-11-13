#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""S10 port: send Mesa's os_mmap/os_munmap to the B2LMali libdrm.

Usage: patch_os_mman.py MESA/src/util/os_mman.h
Appends, inside the header's include guard, macros that redirect both to
b2l_drm_mmap/b2l_drm_munmap (scripts/mesa/b2l-drm/b2l_drm.c). Idempotent.
"""
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
if 'b2l_drm_mmap' in text:
    sys.exit(0)
hook = """
/* S10 port: buffer objects are mapped by the B2LMali libdrm (b2l-drm). */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void *b2l_drm_mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset);
int b2l_drm_munmap(void *addr, size_t length);
#ifdef __cplusplus
}
#endif
#undef os_mmap
#undef os_munmap
#define os_mmap(addr, length, prot, flags, fd, offset) b2l_drm_mmap(addr, length, prot, flags, fd, offset)
#define os_munmap(addr, length) b2l_drm_munmap(addr, length)
"""
end = text.rfind('#endif')
text = text[:end] + hook + text[end:] if end >= 0 else text + hook
path.write_text(text)
print('os_mman.h: os_mmap/os_munmap go to the B2LMali libdrm')
