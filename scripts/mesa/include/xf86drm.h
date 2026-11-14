/* SPDX-License-Identifier: MIT */
/*
 * S10 port: the part of libdrm's public interface that Mesa's Panfrost uses,
 * for an iOS build with no DRM device. Newly written from the documented
 * libdrm API (function names, argument order, drmVersion layout). The
 * implementation is scripts/mesa/b2l-drm/b2l_drm.c, which turns each call
 * into a B2LMali user-client call instead of a Linux ioctl.
 */
#ifndef B2L_XF86DRM_H
#define B2L_XF86DRM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "drm-uapi/drm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRM_NODE_PRIMARY 0
#define DRM_NODE_CONTROL 1
#define DRM_NODE_RENDER  2
#define DRM_NODE_MAX     3

#define DRM_BUS_PCI      0
#define DRM_BUS_USB      1
#define DRM_BUS_PLATFORM 2
#define DRM_BUS_HOST1X   3

typedef struct _drmVersion {
	int version_major;
	int version_minor;
	int version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

typedef struct _drmPlatformBusInfo {
	char fullname[512];
} drmPlatformBusInfo, *drmPlatformBusInfoPtr;

typedef struct _drmPlatformDeviceInfo {
	char **compatible;
} drmPlatformDeviceInfo, *drmPlatformDeviceInfoPtr;

typedef struct _drmDevice {
	char **nodes;
	int available_nodes;
	int bustype;
	union {
		drmPlatformBusInfoPtr platform;
		void *other;
	} businfo;
	union {
		drmPlatformDeviceInfoPtr platform;
		void *other;
	} deviceinfo;
} drmDevice, *drmDevicePtr;

extern int drmIoctl(int fd, unsigned long request, void *arg);
extern int drmOpenWithType(const char *name, const char *busid, int type);
extern int drmClose(int fd);
extern drmVersionPtr drmGetVersion(int fd);
extern void drmFreeVersion(drmVersionPtr version);
extern int drmGetCap(int fd, uint64_t capability, uint64_t *value);
extern int drmSetClientCap(int fd, uint64_t capability, uint64_t value);
extern int drmGetNodeTypeFromFd(int fd);
extern char *drmGetDeviceNameFromFd2(int fd);
extern char *drmGetRenderDeviceNameFromFd(int fd);
extern int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
extern void drmFreeDevice(drmDevicePtr *device);
extern int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
extern void drmFreeDevices(drmDevicePtr devices[], int count);
extern int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b);

extern int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
extern int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
extern int drmCloseBufferHandle(int fd, uint32_t handle);

extern int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
extern int drmSyncobjDestroy(int fd, uint32_t handle);
extern int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
extern int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
extern int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
extern int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
extern int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled);
extern int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count);
extern int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count);
extern int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t handle_count);
extern int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
    int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
extern int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count);
extern int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count, uint32_t flags);
extern int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
    uint64_t src_point, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif
