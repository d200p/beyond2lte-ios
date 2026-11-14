// SPDX-License-Identifier: MIT
/*
 * S10 port: libdrm for Mesa's Panfrost on iOS, over the B2LMali user client
 * (type 1, buffer objects). Newly written; it implements the libdrm calls and
 * DRM ioctls Panfrost makes (the list the Mesa build prints) and nothing else.
 *
 * - The "DRM fd" is a pipe: a real descriptor Mesa can dup and close, which
 *   fstat identifies (same pipe, same inode). One B2LMali connection serves
 *   every such fd of the process.
 * - Buffer objects are the kernel's (kB2LMaliBoCreate/Free); MMAP_BO hands out
 *   handle << 32 as the offset, and Mesa's os_mmap/os_munmap (patched by the
 *   build script) come here to map them with IOConnectMapMemory64. They are
 *   write-combined, as on non-coherent Linux: no cache maintenance anywhere.
 * - Jobs are synchronous in the kernel: when SUBMIT returns the chain is done,
 *   so every sync object is signaled at once and waits return immediately.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#include "xf86drm.h"
#include "drm-uapi/panfrost_drm.h"
#include "b2l_mali_uapi.h"
#include "b2l_drm.h"

typedef mach_port_t io_object_t, io_service_t, io_connect_t;
extern const mach_port_t kIOMainPortDefault;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t port, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOServiceClose(io_connect_t connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallMethod(io_connect_t c, uint32_t selector, const uint64_t *in, uint32_t in_count,
    const void *struct_in, size_t struct_in_size, uint64_t *out, uint32_t *out_count, void *struct_out,
    size_t *struct_out_size);
extern kern_return_t IOConnectMapMemory64(io_connect_t c, uint32_t type, task_port_t task,
    mach_vm_address_t *address, mach_vm_size_t *size, uint32_t options);
extern kern_return_t IOConnectUnmapMemory64(io_connect_t c, uint32_t type, task_port_t task,
    mach_vm_address_t address);

#define MAP_OPTIONS     0x401u          /* kIOMapAnywhere | kIOMapWriteCombineCache */
#define SYNC_FREE       0
#define SYNC_UNSIGNALED 1
#define SYNC_SIGNALED   2
#define MAX_LOGGED      16

struct bo {
	uint64_t va, size;
	uint32_t flags, maps;
	uint8_t *cpu;
	int live;
};

static struct {
	pthread_mutex_t lock;
	io_connect_t connection;
	int anchor;                     /* read end of the pipe; every DRM fd is a dup */
	dev_t dev;
	ino_t ino;
	struct b2l_mali_props props;
	struct bo bos[B2L_MALI_MAX_BOS + 1];
	uint8_t *syncs;
	uint32_t sync_capacity;
	uint64_t jobs, faults;
} drm = { .lock = PTHREAD_MUTEX_INITIALIZER, .anchor = -1 };

/* One line in the kernel log, "B2L GPU UI: ..." (B2LMali log selector). */
void
b2l_drm_note(const char *format, ...)
{
	char text[B2L_MALI_MAX_LOG - 1];
	va_list ap;
	va_start(ap, format);
	int n = vsnprintf(text, sizeof(text), format, ap);
	va_end(ap);
	if (drm.connection != MACH_PORT_NULL && n > 0) {
		IOConnectCallMethod(drm.connection, kB2LMaliLog, NULL, 0, text,
		    (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1, NULL, NULL, NULL, NULL);
	}
}

static int
fail(int error)
{
	errno = error;
	return -1;
}

static int
ours(int fd)
{
	struct stat st;
	return drm.anchor >= 0 && fd >= 0 && fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode) &&
	    st.st_dev == drm.dev && st.st_ino == drm.ino;
}

int
b2l_drm_open(void)
{
	int fd = -1;

	pthread_mutex_lock(&drm.lock);
	if (drm.connection == MACH_PORT_NULL) {
		io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("B2LMali"));
		io_connect_t connection = MACH_PORT_NULL;
		kern_return_t kr = service != MACH_PORT_NULL ?
		    IOServiceOpen(service, mach_task_self(), B2L_MALI_CLIENT_BO, &connection) : KERN_FAILURE;
		if (service != MACH_PORT_NULL) {
			IOObjectRelease(service);
		}
		size_t size = sizeof(drm.props);
		if (kr == KERN_SUCCESS) {
			kr = IOConnectCallMethod(connection, kB2LMaliProps, NULL, 0, NULL, 0, NULL, NULL, &drm.props, &size);
		}
		if (kr != KERN_SUCCESS || size != sizeof(drm.props)) {
			if (connection != MACH_PORT_NULL) {
				IOServiceClose(connection);
			}
			pthread_mutex_unlock(&drm.lock);
			return fail(ENODEV);
		}
		drm.connection = connection;
	}
	if (drm.anchor < 0) {
		int p[2];
		struct stat st;
		if (pipe(p) != 0) {
			pthread_mutex_unlock(&drm.lock);
			return -1;
		}
		close(p[1]);
		fcntl(p[0], F_SETFD, FD_CLOEXEC);
		if (fstat(p[0], &st) != 0) {
			close(p[0]);
			pthread_mutex_unlock(&drm.lock);
			return -1;
		}
		drm.anchor = p[0];
		drm.dev = st.st_dev;
		drm.ino = st.st_ino;
		b2l_drm_note("libdrm: panfrost device open in pid %d, gpu 0x%x", getpid(), drm.props.gpu_id);
	}
	fd = fcntl(drm.anchor, F_DUPFD_CLOEXEC, 0);
	pthread_mutex_unlock(&drm.lock);
	return fd;
}

/* Device identity: Panfrost on a platform bus. */

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)busid;
	(void)type;
	if (name != NULL && strcmp(name, "panfrost") != 0) {
		return fail(ENODEV);
	}
	return b2l_drm_open();
}

int
drmClose(int fd)
{
	return close(fd);
}

drmVersionPtr
drmGetVersion(int fd)
{
	if (!ours(fd)) {
		errno = ENODEV;
		return NULL;
	}
	drmVersionPtr v = calloc(1, sizeof(*v));
	if (v == NULL) {
		return NULL;
	}
	/* 1.2: HEAP/NOEXEC objects, MADVISE, AFBC_FEATURES; nothing newer. */
	v->version_major = 1;
	v->version_minor = 2;
	v->name = strdup("panfrost");
	v->name_len = 8;
	v->date = strdup("20251113");
	v->date_len = 8;
	v->desc = strdup("panfrost DRM over B2LMali");
	v->desc_len = (int)strlen("panfrost DRM over B2LMali");
	return v;
}

void
drmFreeVersion(drmVersionPtr v)
{
	if (v != NULL) {
		free(v->name);
		free(v->date);
		free(v->desc);
		free(v);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	if (!ours(fd)) {
		return fail(EBADF);
	}
	*value = capability == DRM_CAP_SYNCOBJ;
	return 0;
}

int
drmSetClientCap(int fd, uint64_t capability, uint64_t value)
{
	(void)capability;
	(void)value;
	return ours(fd) ? 0 : fail(EBADF);
}

int
drmGetNodeTypeFromFd(int fd)
{
	return ours(fd) ? DRM_NODE_RENDER : fail(EBADF);
}

char *
drmGetDeviceNameFromFd2(int fd)
{
	return ours(fd) ? strdup("/dev/dri/renderD128") : NULL;
}

char *
drmGetRenderDeviceNameFromFd(int fd)
{
	return drmGetDeviceNameFromFd2(fd);
}

static drmDevicePtr
new_device(void)
{
	drmDevicePtr d = calloc(1, sizeof(*d));
	char **nodes = calloc(DRM_NODE_MAX, sizeof(char *));
	drmPlatformBusInfoPtr bus = calloc(1, sizeof(*bus));
	drmPlatformDeviceInfoPtr info = calloc(1, sizeof(*info));
	char **compatible = calloc(2, sizeof(char *));
	if (d == NULL || nodes == NULL || bus == NULL || info == NULL || compatible == NULL) {
		free(d);
		free(nodes);
		free(bus);
		free(info);
		free(compatible);
		return NULL;
	}
	nodes[DRM_NODE_RENDER] = strdup("/dev/dri/renderD128");
	compatible[0] = strdup("arm,mali-bifrost");
	snprintf(bus->fullname, sizeof(bus->fullname), "/gpu@18500000");
	info->compatible = compatible;
	d->nodes = nodes;
	d->available_nodes = 1 << DRM_NODE_RENDER;
	d->bustype = DRM_BUS_PLATFORM;
	d->businfo.platform = bus;
	d->deviceinfo.platform = info;
	return d;
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)flags;
	if (!ours(fd)) {
		return -ENODEV;
	}
	*device = new_device();
	return *device != NULL ? 0 : -ENOMEM;
}

void
drmFreeDevice(drmDevicePtr *device)
{
	if (device == NULL || *device == NULL) {
		return;
	}
	drmDevicePtr d = *device;
	for (int i = 0; i < DRM_NODE_MAX; i++) {
		free(d->nodes[i]);
	}
	free(d->nodes);
	if (d->deviceinfo.platform != NULL) {
		for (char **c = d->deviceinfo.platform->compatible; c != NULL && *c != NULL; c++) {
			free(*c);
		}
		free(d->deviceinfo.platform->compatible);
	}
	free(d->deviceinfo.platform);
	free(d->businfo.platform);
	free(d);
	*device = NULL;
}

int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void)flags;
	if (devices == NULL || max_devices < 1) {
		return 1;
	}
	devices[0] = new_device();
	return devices[0] != NULL ? 1 : -ENOMEM;
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	for (int i = 0; devices != NULL && i < count; i++) {
		drmFreeDevice(&devices[i]);
	}
}

int
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	return a != NULL && b != NULL && a->bustype == b->bustype;
}

/* Buffer objects. */

static struct bo *
bo_get(uint32_t handle)
{
	return handle != 0 && handle <= B2L_MALI_MAX_BOS && drm.bos[handle].live ? &drm.bos[handle] : NULL;
}

static int
bo_create(struct drm_panfrost_create_bo *args)
{
	uint64_t in[2] = { args->size, args->flags }, out[2] = { 0, 0 };
	uint32_t count = 2;
	if (IOConnectCallMethod(drm.connection, kB2LMaliBoCreate, in, 2, NULL, 0, out, &count, NULL, NULL) !=
	    KERN_SUCCESS || out[0] == 0 || out[0] > B2L_MALI_MAX_BOS) {
		return fail(ENOMEM);
	}
	struct bo *bo = &drm.bos[out[0]];
	memset(bo, 0, sizeof(*bo));
	bo->va = out[1];
	bo->size = args->size;
	bo->flags = args->flags;
	bo->live = 1;
	args->handle = (uint32_t)out[0];
	args->offset = out[1];
	return 0;
}

static int
bo_close(uint32_t handle)
{
	struct bo *bo = bo_get(handle);
	if (bo == NULL) {
		return fail(EINVAL);
	}
	if (bo->cpu != NULL) {
		IOConnectUnmapMemory64(drm.connection, B2L_MALI_BO_MEMORY(handle), mach_task_self(),
		    (mach_vm_address_t)bo->cpu);
	}
	uint64_t in[1] = { handle };
	uint32_t none = 0;
	kern_return_t kr = IOConnectCallMethod(drm.connection, kB2LMaliBoFree, in, 1, NULL, 0, NULL, &none, NULL,
	    NULL);
	memset(bo, 0, sizeof(*bo));
	return kr == KERN_SUCCESS ? 0 : fail(EINVAL);
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	if (!ours(fd)) {
		return fail(EBADF);
	}
	pthread_mutex_lock(&drm.lock);
	int ret = bo_close(handle);
	pthread_mutex_unlock(&drm.lock);
	return ret;
}

void *
b2l_drm_mmap(void *addr, size_t length, int prot, int flags, int fd, int64_t offset)
{
	if (!ours(fd)) {
		return mmap(addr, length, prot, flags, fd, (off_t)offset);
	}
	uint32_t handle = (uint32_t)((uint64_t)offset >> 32);
	uint64_t within = (uint64_t)offset & 0xffffffffull;
	void *result = MAP_FAILED;

	pthread_mutex_lock(&drm.lock);
	struct bo *bo = bo_get(handle);
	if (bo != NULL && within + length <= bo->size && !(bo->flags & PANFROST_BO_HEAP)) {
		if (bo->cpu == NULL) {
			mach_vm_address_t address = 0;
			mach_vm_size_t size = 0;
			if (IOConnectMapMemory64(drm.connection, B2L_MALI_BO_MEMORY(handle), mach_task_self(),
			    &address, &size, MAP_OPTIONS) == KERN_SUCCESS) {
				bo->cpu = (uint8_t *)address;
			}
		}
		if (bo->cpu != NULL) {
			bo->maps++;
			result = bo->cpu + within;
		}
	}
	pthread_mutex_unlock(&drm.lock);
	if (result == MAP_FAILED) {
		errno = EINVAL;
	}
	return result;
}

int
b2l_drm_munmap(void *addr, size_t length)
{
	uint8_t *p = addr;

	pthread_mutex_lock(&drm.lock);
	for (uint32_t h = 1; drm.connection != MACH_PORT_NULL && h <= B2L_MALI_MAX_BOS; h++) {
		struct bo *bo = &drm.bos[h];
		if (bo->live && bo->cpu != NULL && p >= bo->cpu && p < bo->cpu + bo->size) {
			if (bo->maps > 0 && --bo->maps == 0) {
				IOConnectUnmapMemory64(drm.connection, B2L_MALI_BO_MEMORY(h), mach_task_self(),
				    (mach_vm_address_t)bo->cpu);
				bo->cpu = NULL;
			}
			pthread_mutex_unlock(&drm.lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&drm.lock);
	return munmap(addr, length);
}

/* Sync objects: binary, signaled by every (synchronous) submit. */

static uint8_t *
sync_get(uint32_t handle)
{
	return handle != 0 && handle < drm.sync_capacity && drm.syncs[handle] != SYNC_FREE ? &drm.syncs[handle] : NULL;
}

static int
sync_new(uint8_t state, uint32_t *handle)
{
	for (uint32_t h = 1; h < drm.sync_capacity; h++) {
		if (drm.syncs[h] == SYNC_FREE) {
			drm.syncs[h] = state;
			*handle = h;
			return 0;
		}
	}
	uint32_t capacity = drm.sync_capacity ? drm.sync_capacity * 2 : 256;
	uint8_t *grown = realloc(drm.syncs, capacity);
	if (grown == NULL) {
		return fail(ENOMEM);
	}
	memset(grown + drm.sync_capacity, SYNC_FREE, capacity - drm.sync_capacity);
	uint32_t h = drm.sync_capacity ? drm.sync_capacity : 1;
	drm.syncs = grown;
	drm.sync_capacity = capacity;
	drm.syncs[h] = state;
	*handle = h;
	return 0;
}

static int
sync_set(const uint32_t *handles, uint32_t count, uint8_t state)
{
	for (uint32_t i = 0; i < count; i++) {
		if (sync_get(handles[i]) == NULL) {
			return fail(ENOENT);
		}
	}
	for (uint32_t i = 0; i < count; i++) {
		drm.syncs[handles[i]] = state;
	}
	return 0;
}

static int64_t
monotonic_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

/* Called with the lock held; drops it while waiting for another thread's submit. */
static int
sync_wait(const uint32_t *handles, unsigned count, int64_t deadline, unsigned flags, uint32_t *first)
{
	for (;;) {
		unsigned done = 0, first_done = count;
		for (unsigned i = 0; i < count; i++) {
			uint8_t *s = sync_get(handles[i]);
			if (s == NULL) {
				return fail(ENOENT);
			}
			if (*s == SYNC_SIGNALED) {
				done++;
				first_done = first_done < count ? first_done : i;
			}
		}
		bool all = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
		if (all ? done == count : done > 0) {
			if (first != NULL) {
				*first = first_done < count ? first_done : 0;
			}
			return 0;
		}
		/* Nothing is ever in flight: only another thread's submit can signal. */
		if (!(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)) {
			return fail(EINVAL);
		}
		if (monotonic_ns() >= deadline) {
			return fail(ETIME);
		}
		pthread_mutex_unlock(&drm.lock);
		usleep(200);
		pthread_mutex_lock(&drm.lock);
	}
}

#define SYNC_ENTRY(body) do { \
	if (!ours(fd)) { \
		return fail(EBADF); \
	} \
	pthread_mutex_lock(&drm.lock); \
	int ret_ = (body); \
	pthread_mutex_unlock(&drm.lock); \
	return ret_; \
} while (0)

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	SYNC_ENTRY(sync_new(flags & DRM_SYNCOBJ_CREATE_SIGNALED ? SYNC_SIGNALED : SYNC_UNSIGNALED, handle));
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	SYNC_ENTRY(sync_set(&handle, 1, SYNC_FREE));
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
	SYNC_ENTRY(sync_set(handles, handle_count, SYNC_UNSIGNALED));
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
	SYNC_ENTRY(sync_set(handles, handle_count, SYNC_SIGNALED));
}

int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
	(void)points;
	SYNC_ENTRY(sync_set(handles, handle_count, SYNC_SIGNALED));
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled)
{
	SYNC_ENTRY(sync_wait(handles, num_handles, timeout_nsec, flags, first_signaled));
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled)
{
	(void)points;
	SYNC_ENTRY(sync_wait(handles, num_handles, timeout_nsec, flags, first_signaled));
}

static int
sync_query(uint32_t *handles, uint64_t *points, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		uint8_t *s = sync_get(handles[i]);
		if (s == NULL) {
			return fail(ENOENT);
		}
		points[i] = *s == SYNC_SIGNALED;
	}
	return 0;
}

int
drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
	SYNC_ENTRY(sync_query(handles, points, handle_count));
}

int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count, uint32_t flags)
{
	(void)flags;
	SYNC_ENTRY(sync_query(handles, points, handle_count));
}

static int
sync_transfer(uint32_t dst, uint32_t src)
{
	uint8_t *d = sync_get(dst), *s = sync_get(src);
	if (d == NULL || s == NULL) {
		return fail(ENOENT);
	}
	*d = *s;
	return 0;
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
    uint32_t flags)
{
	(void)dst_point;
	(void)src_point;
	(void)flags;
	SYNC_ENTRY(sync_transfer(dst_handle, src_handle));
}

/* Sync files and shared sync objects stand for "already signaled": a /dev/null descriptor. */
static int
signaled_fd(void)
{
	return open("/dev/null", O_RDONLY | O_CLOEXEC);
}

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	if (!ours(fd)) {
		return fail(EBADF);
	}
	pthread_mutex_lock(&drm.lock);
	int ret = sync_get(handle) != NULL ? 0 : fail(ENOENT);
	pthread_mutex_unlock(&drm.lock);
	if (ret == 0) {
		*sync_file_fd = signaled_fd();
		ret = *sync_file_fd >= 0 ? 0 : -1;
	}
	return ret;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)sync_file_fd;
	SYNC_ENTRY(sync_set(&handle, 1, SYNC_SIGNALED));
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	return drmSyncobjExportSyncFile(fd, handle, obj_fd);
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)obj_fd;
	SYNC_ENTRY(sync_new(SYNC_SIGNALED, handle));
}

/* Buffer sharing goes through IOSurfaces in the EAGL layer, not prime fds. */

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd;
	(void)handle;
	(void)flags;
	(void)prime_fd;
	return fail(ENOSYS);
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	(void)fd;
	(void)prime_fd;
	(void)handle;
	return fail(ENOSYS);
}

/* The ioctls. */

static uint64_t
param(uint32_t which, int *known)
{
	const struct b2l_mali_props *p = &drm.props;
	*known = 1;
	if (which >= DRM_PANFROST_PARAM_TEXTURE_FEATURES0 && which <= DRM_PANFROST_PARAM_TEXTURE_FEATURES3) {
		return p->texture_features[which - DRM_PANFROST_PARAM_TEXTURE_FEATURES0];
	}
	if (which >= DRM_PANFROST_PARAM_JS_FEATURES0 && which <= DRM_PANFROST_PARAM_JS_FEATURES15) {
		return p->js_features[which - DRM_PANFROST_PARAM_JS_FEATURES0];
	}
	switch (which) {
	case DRM_PANFROST_PARAM_GPU_PROD_ID: return p->gpu_id >> 16;
	case DRM_PANFROST_PARAM_GPU_REVISION: return p->gpu_id & 0xffff;
	case DRM_PANFROST_PARAM_SHADER_PRESENT: return p->shader_present;
	case DRM_PANFROST_PARAM_TILER_PRESENT: return p->tiler_present;
	case DRM_PANFROST_PARAM_L2_PRESENT: return p->l2_present;
	case DRM_PANFROST_PARAM_STACK_PRESENT: return p->stack_present;
	case DRM_PANFROST_PARAM_AS_PRESENT: return p->as_present;
	case DRM_PANFROST_PARAM_JS_PRESENT: return p->js_present;
	case DRM_PANFROST_PARAM_L2_FEATURES: return p->l2_features;
	case DRM_PANFROST_PARAM_CORE_FEATURES: return p->core_features;
	case DRM_PANFROST_PARAM_TILER_FEATURES: return p->tiler_features;
	case DRM_PANFROST_PARAM_MEM_FEATURES: return p->mem_features;
	case DRM_PANFROST_PARAM_MMU_FEATURES: return p->mmu_features;
	case DRM_PANFROST_PARAM_THREAD_FEATURES: return p->thread_features;
	case DRM_PANFROST_PARAM_MAX_THREADS: return p->thread_max_threads;
	case DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ: return p->thread_max_workgroup;
	case DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ: return p->thread_max_barrier;
	case DRM_PANFROST_PARAM_COHERENCY_FEATURES: return p->coherency_features;
	case DRM_PANFROST_PARAM_NR_CORE_GROUPS: return (uint64_t)__builtin_popcountll(p->l2_present);
	case DRM_PANFROST_PARAM_THREAD_TLS_ALLOC: return p->thread_tls_alloc;
	case DRM_PANFROST_PARAM_AFBC_FEATURES: return p->afbc_features;
	default:
		*known = 0;
		return 0;
	}
}

/* Copy GPU memory at va through the process's mapping of its buffer object. */
static int
bo_read(uint64_t va, void *out, size_t bytes)
{
	for (uint32_t h = 1; h <= B2L_MALI_MAX_BOS; h++) {
		struct bo *bo = &drm.bos[h];
		if (bo->live && bo->cpu != NULL && va >= bo->va && va + bytes <= bo->va + bo->size) {
			memcpy(out, bo->cpu + (va - bo->va), bytes);
			return 1;
		}
	}
	return 0;
}

/*
 * Boot 39 diagnostic: the jobs of the first submissions as the GPU left them.
 * Each job header starts with the exception status the GPU writes back (1
 * done, 0 never reached) and holds the type, the index and the next job.
 */
#define CHAIN_LOGGED 300
static void
chain_note(const struct drm_panfrost_submit *args, uint64_t status)
{
	char line[200];
	int n = snprintf(line, sizeof(line), "libdrm: sub %llu req=%u st=%llx jc=%llx:", (unsigned long long)drm.jobs,
	    args->requirements, (unsigned long long)status, (unsigned long long)args->jc);
	uint64_t va = args->jc;
	for (unsigned i = 0; i < 12 && va != 0 && n > 0 && n < (int)sizeof(line) - 24; i++) {
		uint32_t h[8];
		if (!bo_read(va, h, sizeof(h))) {
			n += snprintf(line + n, sizeof(line) - n, " ?%llx", (unsigned long long)va);
			break;
		}
		n += snprintf(line + n, sizeof(line) - n, " t%u/%u:%x", (h[4] >> 1) & 0x7f, h[4] >> 16, h[0]);
		va = (uint64_t)h[6] | (uint64_t)h[7] << 32;
	}
	b2l_drm_note("%s", line);
}

static int
submit(struct drm_panfrost_submit *args)
{
	const uint32_t *in_syncs = (const uint32_t *)(uintptr_t)args->in_syncs;
	for (uint32_t i = 0; i < args->in_sync_count; i++) {
		if (sync_get(in_syncs[i]) == NULL) {
			return fail(ENOENT);
		}
	}
	if (args->out_sync != 0 && sync_get(args->out_sync) == NULL) {
		return fail(ENOENT);
	}
	uint64_t in[2] = { args->jc, args->requirements & PANFROST_JD_REQ_FS ? B2L_MALI_REQ_FS : 0 };
	uint64_t out[2] = { 0, 0 };
	uint32_t count = 2;
	kern_return_t kr = IOConnectCallMethod(drm.connection, kB2LMaliJobSubmit, in, 2, NULL, 0, out, &count, NULL,
	    NULL);
	if (kr != KERN_SUCCESS) {
		if (drm.faults++ < MAX_LOGGED) {
			b2l_drm_note("libdrm: submit jc=0x%llx refused kr=0x%x", (unsigned long long)args->jc, kr);
		}
		return fail(EIO);
	}
	drm.jobs++;
	/* A faulted job is reported like Linux does: through the log, not the ioctl. */
	if (out[0] != B2L_MALI_STATUS_DONE && drm.faults++ < MAX_LOGGED) {
		b2l_drm_note("libdrm: job %llu jc=0x%llx req=%u status=0x%llx", (unsigned long long)drm.jobs,
		    (unsigned long long)args->jc, args->requirements, (unsigned long long)out[0]);
	}
	if (drm.jobs <= CHAIN_LOGGED) {
		chain_note(args, out[0]);
	}
	if (args->out_sync != 0) {
		drm.syncs[args->out_sync] = SYNC_SIGNALED;
	}
	return 0;
}

static int
ioctl_locked(unsigned long request, void *arg)
{
	switch (request) {
	case DRM_IOCTL_PANFROST_GET_PARAM: {
		struct drm_panfrost_get_param *a = arg;
		int known;
		a->value = param(a->param, &known);
		return known ? 0 : fail(EINVAL);
	}
	case DRM_IOCTL_PANFROST_CREATE_BO:
		return bo_create(arg);
	case DRM_IOCTL_PANFROST_MMAP_BO: {
		struct drm_panfrost_mmap_bo *a = arg;
		if (bo_get(a->handle) == NULL) {
			return fail(ENOENT);
		}
		a->offset = (uint64_t)a->handle << 32;
		return 0;
	}
	case DRM_IOCTL_PANFROST_GET_BO_OFFSET: {
		struct drm_panfrost_get_bo_offset *a = arg;
		struct bo *bo = bo_get(a->handle);
		if (bo == NULL) {
			return fail(ENOENT);
		}
		a->offset = bo->va;
		return 0;
	}
	case DRM_IOCTL_PANFROST_SUBMIT:
		return submit(arg);
	case DRM_IOCTL_PANFROST_WAIT_BO:
		return bo_get(((struct drm_panfrost_wait_bo *)arg)->handle) != NULL ? 0 : fail(ENOENT);
	case DRM_IOCTL_PANFROST_MADVISE: {
		struct drm_panfrost_madvise *a = arg;
		if (bo_get(a->handle) == NULL) {
			return fail(ENOENT);
		}
		a->retained = 1;        /* nothing is ever purged */
		return 0;
	}
	case DRM_IOCTL_GEM_CLOSE:
		return bo_close(((struct drm_gem_close *)arg)->handle);
	case DRM_IOCTL_SYNCOBJ_CREATE: {
		struct drm_syncobj_create *a = arg;
		return sync_new(a->flags & DRM_SYNCOBJ_CREATE_SIGNALED ? SYNC_SIGNALED : SYNC_UNSIGNALED, &a->handle);
	}
	case DRM_IOCTL_SYNCOBJ_DESTROY:
		return sync_set(&((struct drm_syncobj_destroy *)arg)->handle, 1, SYNC_FREE);
	case DRM_IOCTL_SYNCOBJ_RESET: {
		struct drm_syncobj_array *a = arg;
		return sync_set((const uint32_t *)(uintptr_t)a->handles, a->count_handles, SYNC_UNSIGNALED);
	}
	case DRM_IOCTL_SYNCOBJ_SIGNAL: {
		struct drm_syncobj_array *a = arg;
		return sync_set((const uint32_t *)(uintptr_t)a->handles, a->count_handles, SYNC_SIGNALED);
	}
	case DRM_IOCTL_SYNCOBJ_WAIT: {
		struct drm_syncobj_wait *a = arg;
		return sync_wait((const uint32_t *)(uintptr_t)a->handles, a->count_handles, a->timeout_nsec, a->flags,
		    &a->first_signaled);
	}
	default:
		return fail(ENOTTY);
	}
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	if (!ours(fd)) {
		int ret;
		do {
			ret = ioctl(fd, request, arg);
		} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
		return ret;
	}
	pthread_mutex_lock(&drm.lock);
	int ret = ioctl_locked(request, arg);
	pthread_mutex_unlock(&drm.lock);
	return ret;
}
