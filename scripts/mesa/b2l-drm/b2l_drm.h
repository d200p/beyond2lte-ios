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
/*
 * An IOSurface as a dma-buf for Mesa (resource_from_handle with
 * WINSYS_HANDLE_TYPE_FD): an fd whose size (lseek to the end) is bytes, which
 * drmPrimeFDToHandle turns into a buffer object mapping the surface. The fd
 * belongs to the caller; b2l_drm_surface_fd_close closes it. -1 on failure.
 */
int b2l_drm_surface_fd(uint32_t surface_id, uint64_t bytes);
void b2l_drm_surface_fd_close(int fd);

#ifdef __cplusplus
}
#endif

#endif
