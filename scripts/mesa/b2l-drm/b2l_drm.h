/* SPDX-License-Identifier: MIT */
/*
 * S10 port: the B2LMali libdrm's own entry points. b2l_drm_open gives a DRM fd
 * for panfrost_create_screen; Mesa's os_mmap/os_munmap are redirected to the
 * other two by the build script (src/util/os_mman.h), so that mapping a buffer
 * object at its MMAP_BO offset maps the kernel's memory for it.
 */
#ifndef B2L_DRM_H
#define B2L_DRM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int b2l_drm_open(void);
void *b2l_drm_mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset);
int b2l_drm_munmap(void *addr, size_t length);
void b2l_drm_note(const char *format, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif
