// SPDX-License-Identifier: MIT
/*
 * S10 port: B2LMetal.bundle, the Objective-C half of the Metal driver
 * (docs/metal-on-panfrost.md). M1 made the device; M2a gives it objects that
 * hold something, through libb2l-mesa's b2l_mtl_* table (scripts/mesa/b2l-
 * target/b2l_mtl.h, Gallium on Panfrost): buffers, textures (IOSurfaces
 * among them), samplers, depth-stencil states, libraries and functions (with
 * their constants, by index or by name), render pipelines (the shaders
 * compiled by Panfrost when the pipeline is made), command buffers with
 * render passes (load actions, clears) and blits. Draws are M2b: a draw is
 * recorded as not implemented and the command buffer ends in error.
 *
 * What Metal does on iOS 26 (read from the 23G90 cache): it matches the
 * IOAcceleratorES services, reads MetalPluginName ("B2LMetal-<hash>", this
 * bundle under /System/Library/Extensions) and MetalPluginClassName
 * ("B2LMetalDevice"), takes that class if it is a subclass of its private
 * _MTLDevice, and sends it -initWithAcceleratorPort:. B2LMali makes that
 * IOAcceleratorES with boot argument b2l_metal=N; the kernel publishes this
 * bundle (b2l_userspace_seed.h).
 *
 * The approach -- classes made at run time, the device on the system's own
 * _MTLDevice with MTLDevice and the private MTLDeviceSPI declared (MTLAddDevice
 * asserts both), every message a class has no method for answered with a zero
 * of the protocol's own return type and logged once -- is HakiOS's
 * (github.com/oosexgang/hakios-metal, plugin/HakiosMetal.m, MIT). This file is
 * written for the S10 port in C on the Objective-C runtime and logs to the
 * kernel log through B2LMali ("B2L GPU UI: mtl M2: ..."). Every object class
 * other than the device and the queue is made on NSObject, not on Metal's
 * private bases, so that nothing of Metal's own implementation runs on state
 * it never initialised; what such a class is not asked yet is logged.
 *
 * Who is shown the device: boot argument b2l_metal=1 shows it only to a
 * process started with B2L_MTL_TEST in its environment (userspace/b2l-gpu.c
 * starts one); b2l_metal=2 (M3) shows it to everyone, backboardd included. A
 * process not shown it gets no class registered, so Metal finds no device and
 * Core Animation stays on its OpenGL ES path.
 */
#include <Block.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "b2l_mali_uapi.h"
#include "b2l_mtl.h"

typedef mach_port_t io_object_t, io_service_t, io_connect_t;
extern const mach_port_t kIOMainPortDefault;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t port, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallMethod(io_connect_t c, uint32_t selector, const uint64_t *in, uint32_t in_count,
    const void *struct_in, size_t struct_in_size, uint64_t *out, uint32_t *out_count, void *struct_out,
    size_t *struct_out_size);

/* libobjc's own entry points (not in the SDK's public headers) */
extern id objc_retain(id object);
extern void objc_release(id object);
extern id objc_autorelease(id object);
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *pool);

#define ONE(x) ((x) != 0 ? (x) : 1)

/* ---- the kernel log: a B2LMali client that only ever sends kB2LMaliLog ---- */

static io_connect_t log_connection;
static pthread_once_t log_once = PTHREAD_ONCE_INIT;

static void
log_open(void)
{
	io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("B2LMali"));
	if (service == MACH_PORT_NULL) {
		return;
	}
	/* The runtime behind the client (clientBegin) is reached by no log call. */
	if (IOServiceOpen(service, mach_task_self(), B2L_MALI_CLIENT_BO, &log_connection) != KERN_SUCCESS) {
		log_connection = MACH_PORT_NULL;
	}
	IOObjectRelease(service);
}

static void
say(const char *format, ...)
{
	char text[B2L_MALI_MAX_LOG - 1];
	int head = snprintf(text, sizeof(text), "mtl M2: [%d %s] ", getpid(), getprogname());
	if (head < 0 || (size_t)head >= sizeof(text)) {
		return;
	}
	va_list ap;
	va_start(ap, format);
	int n = vsnprintf(text + head, sizeof(text) - (size_t)head, format, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	size_t len = strnlen(text, sizeof(text));
	pthread_once(&log_once, log_open);
	if (log_connection != MACH_PORT_NULL) {
		IOConnectCallMethod(log_connection, kB2LMaliLog, NULL, 0, text, len, NULL, NULL, NULL, NULL);
	}
}

static uint64_t
now_us(void)
{
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom / 1000;
}

/* ---- who is shown the device ---- */

static int
metal_level(void)
{
	char args[2048];
	size_t size = sizeof(args) - 1;
	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) {
		return 0;
	}
	args[size < sizeof(args) ? size : sizeof(args) - 1] = 0;
	for (const char *p = args; *p != 0;) {
		while (*p == ' ') {
			p++;
		}
		if (strncmp(p, "b2l_metal=", 10) == 0) {
			return atoi(p + 10);
		}
		const char *end = strchr(p, ' ');
		if (end == NULL) {
			break;
		}
		p = end;
	}
	return 0;
}

static bool
shown_to_this_process(int level)
{
	if (level >= 2) {
		return true;
	}
	return level >= 1 && getenv("B2L_MTL_TEST") != NULL;
}

/* ---- Objective-C from C ---- */

static SEL
S(const char *name)
{
	return sel_registerName(name);
}

static id
msg(id o, const char *sel)
{
	return o != nil ? ((id (*)(id, SEL))objc_msgSend)(o, S(sel)) : nil;
}

static unsigned long
msg_u(id o, const char *sel)
{
	return o != nil ? ((unsigned long (*)(id, SEL))objc_msgSend)(o, S(sel)) : 0;
}

static bool
msg_b(id o, const char *sel)
{
	return o != nil && ((bool (*)(id, SEL))objc_msgSend)(o, S(sel));
}

static float
msg_f(id o, const char *sel)
{
	return o != nil ? ((float (*)(id, SEL))objc_msgSend)(o, S(sel)) : 0.0f;
}

static double
msg_d(id o, const char *sel)
{
	return o != nil ? ((double (*)(id, SEL))objc_msgSend)(o, S(sel)) : 0.0;
}

static id
msg_at(id array, unsigned long index)
{
	return array != nil ? ((id (*)(id, SEL, unsigned long))objc_msgSend)(array, S("objectAtIndexedSubscript:"),
	    index) : nil;
}

static bool
responds(id o, const char *sel)
{
	return o != nil && ((bool (*)(id, SEL, SEL))objc_msgSend)(o, S("respondsToSelector:"), S(sel));
}

/* An NSString's UTF-8, into out. */
static const char *
cstring(id string, char *out, size_t size)
{
	out[0] = 0;
	if (string != nil) {
		CFStringGetCString((CFStringRef)string, out, (CFIndex)size, kCFStringEncodingUTF8);
	}
	return out;
}

static id
nsstring(const char *text)
{
	return (id)CFStringCreateWithCString(NULL, text != NULL ? text : "", kCFStringEncodingUTF8);
}

/* An autoreleased NSError with this description. */
static id
make_error(const char *domain, long code, const char *text)
{
	CFStringRef key = CFSTR("NSLocalizedDescription");
	CFStringRef value = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
	CFStringRef name = CFStringCreateWithCString(NULL, domain, kCFStringEncodingUTF8);
	CFDictionaryRef info = CFDictionaryCreate(NULL, (const void **)&key, (const void **)&value, 1,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	id error = ((id (*)(id, SEL, id, long, id))objc_msgSend)((id)objc_getClass("NSError"),
	    S("errorWithDomain:code:userInfo:"), (id)name, code, (id)info);
	CFRelease(info);
	CFRelease(name);
	CFRelease(value);
	return error;
}

/* *error, when the caller asked for one. */
static void
set_error(id *error, long code, const char *text)
{
	if (error != NULL) {
		*error = make_error("MTLLibraryErrorDomain", code, text);
	}
}

/* ---- classes ---- */

static Class gDeviceBase, gDeviceClass, gQueueClass;
static Class gBufferClass, gTextureClass, gSamplerClass, gDepthStencilClass, gLibraryClass, gFunctionClass;
static Class gRenderPipelineClass, gCommandBufferClass, gRenderEncoderClass, gBlitEncoderClass, gComputeEncoderClass;
static id gDevice;
static unsigned gPort;
static Protocol *gProtocols[96];
static unsigned gProtocolCount;

typedef id (*send0_t)(id, SEL);
typedef id (*super0_t)(struct objc_super *, SEL);

static void
remember_protocol(Class cls, Protocol *protocol)
{
	if (protocol == NULL) {
		return;
	}
	class_addProtocol(cls, protocol);
	for (unsigned i = 0; i < gProtocolCount; i++) {
		if (gProtocols[i] == protocol) {
			return;
		}
	}
	if (gProtocolCount < sizeof(gProtocols) / sizeof(gProtocols[0])) {
		gProtocols[gProtocolCount++] = protocol;
	}
}

/* Every protocol of this process whose name begins with `prefix`: the public
 * one and Metal's private ones of the same family (MTLCommandQueueSPI ...). */
static void
declare_family(Class cls, const char *prefix)
{
	unsigned count = 0;
	Protocol **all = objc_copyProtocolList(&count);
	size_t n = strlen(prefix);
	for (unsigned i = 0; i < count; i++) {
		const char *name = protocol_getName(all[i]);
		if (name != NULL && strncmp(name, prefix, n) == 0) {
			remember_protocol(cls, all[i]);
		}
	}
	free(all);
}

/* Exactly these protocols and their "SPI" twins, where this Metal has them
 * (a family by prefix would take MTLBufferBinding for a buffer). */
static void
declare_exact(Class cls, const char *const *names)
{
	for (unsigned i = 0; names[i] != NULL; i++) {
		char spi[96];
		remember_protocol(cls, objc_getProtocol(names[i]));
		snprintf(spi, sizeof(spi), "%sSPI", names[i]);
		remember_protocol(cls, objc_getProtocol(spi));
	}
}

/* A message the class has no method for, but one of its protocols declares:
 * the signature from the protocol, so the zero below has the right size. */
static id
object_signature(id self, SEL _cmd, SEL selector)
{
	(void)_cmd;
	for (unsigned i = 0; i < gProtocolCount; i++) {
		for (int required = 1; required >= 0; required--) {
			struct objc_method_description d = protocol_getMethodDescription(gProtocols[i], selector, required, true);
			if (d.name != NULL && d.types != NULL) {
				return ((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSMethodSignature"),
				    S("signatureWithObjCTypes:"), d.types);
			}
		}
	}
	struct objc_super sup = { self, class_getSuperclass(object_getClass(self)) };
	return ((id (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper)(&sup, S("methodSignatureForSelector:"),
	    selector);
}

#define SEEN_MAX 512
static SEL seen[SEEN_MAX];
static unsigned seen_count;
static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;

static bool
first_time(SEL selector)
{
	bool first = true;
	pthread_mutex_lock(&seen_lock);
	for (unsigned i = 0; i < seen_count; i++) {
		if (seen[i] == selector) {
			first = false;
			break;
		}
	}
	if (first && seen_count < SEEN_MAX) {
		seen[seen_count++] = selector;
	}
	pthread_mutex_unlock(&seen_lock);
	return first;
}

static void
object_forward(id self, SEL _cmd, id invocation)
{
	(void)_cmd;
	SEL selector = ((SEL (*)(id, SEL))objc_msgSend)(invocation, S("selector"));
	id signature = msg(invocation, "methodSignature");
	unsigned long length = msg_u(signature, "methodReturnLength");
	if (first_time(selector)) {
		const char *type = ((const char *(*)(id, SEL))objc_msgSend)(signature, S("methodReturnType"));
		say("%s asked -%s, not implemented: zero (%lu bytes, %s)", class_getName(object_getClass(self)),
		    sel_getName(selector), length, type != NULL ? type : "?");
	}
	if (length != 0) {
		void *zero = calloc(1, length);
		if (zero != NULL) {
			((void (*)(id, SEL, void *))objc_msgSend)(invocation, S("setReturnValue:"), zero);
			free(zero);
		}
	}
}

/* ---- per-object state: one ivar, _b2l, pointing at a C structure ---- */

struct base {
	id label;
};

static void **
slot_of(id self)
{
	Ivar ivar = self != nil ? class_getInstanceVariable(object_getClass(self), "_b2l") : NULL;
	return ivar != NULL ? (void **)((char *)self + ivar_getOffset(ivar)) : NULL;
}

static void *
state_of(id self)
{
	void **slot = slot_of(self);
	return slot != NULL ? *slot : NULL;
}

/* An object of ours (class_createInstance: +1, no -init) holding `state`. */
static id
make_object(Class cls, void *state)
{
	if (cls == Nil || state == NULL) {
		return nil;
	}
	id o = class_createInstance(cls, 0);
	void **slot = slot_of(o);
	if (o == nil || slot == NULL) {
		return nil;
	}
	*slot = state;
	return o;
}

static id
object_device(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return gDevice;
}

static id
object_label(id self, SEL _cmd)
{
	(void)_cmd;
	struct base *b = state_of(self);
	return b != NULL ? b->label : nil;
}

static void
object_set_label(id self, SEL _cmd, id label)
{
	(void)_cmd;
	struct base *b = state_of(self);
	if (b == NULL) {
		return;
	}
	id old = b->label;
	b->label = label != nil ? msg(label, "copy") : nil;
	if (old != nil) {
		objc_release(old);
	}
}

static void
object_nothing(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
}

static void
object_nothing_id(id self, SEL _cmd, id value)
{
	(void)self;
	(void)_cmd;
	(void)value;
}

static bool
object_no(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return false;
}

static bool
object_yes(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return true;
}

static id
object_nil(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return nil;
}

static unsigned long
object_zero(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return 0;
}

static unsigned long
object_tracked(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return 2;                       /* MTLHazardTrackingModeTracked */
}

static unsigned long
object_non_volatile(id self, SEL _cmd, unsigned long state)
{
	(void)self;
	(void)_cmd;
	(void)state;
	return 2;                       /* MTLPurgeableStateNonVolatile */
}

static uint64_t
object_resource_id(id self, SEL _cmd)
{
	(void)_cmd;
	return (uint64_t)(uintptr_t)state_of(self);
}

/* A class of ours on NSObject with the _b2l slot, the given protocols, the
 * label, device and forwarding methods; the caller adds the rest and
 * registers it. */
static Class
object_class(const char *name, const char *const *protocols)
{
	if (objc_getClass(name) != Nil) {
		return Nil;
	}
	Class cls = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);
	if (cls == Nil) {
		say("could not make the class %s", name);
		return Nil;
	}
	class_addIvar(cls, "_b2l", sizeof(void *), 3, "^v");
	declare_exact(cls, protocols);
	class_addMethod(cls, S("device"), (IMP)object_device, "@@:");
	class_addMethod(cls, S("label"), (IMP)object_label, "@@:");
	class_addMethod(cls, S("setLabel:"), (IMP)object_set_label, "v@:@");
	class_addMethod(cls, S("methodSignatureForSelector:"), (IMP)object_signature, "@@::");
	class_addMethod(cls, S("forwardInvocation:"), (IMP)object_forward, "v@:@");
	return cls;
}

/* -dealloc: the state's own parts by `release`, the label, the state, then
 * NSObject's -dealloc. */
static void
dealloc_with(id self, void (*release)(void *))
{
	void **slot = slot_of(self);
	if (slot != NULL && *slot != NULL) {
		struct base *b = *slot;
		if (release != NULL) {
			release(b);
		}
		if (b->label != nil) {
			objc_release(b->label);
		}
		free(b);
		*slot = NULL;
	}
	struct objc_super sup = { self, objc_getClass("NSObject") };
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, S("dealloc"));
}

/* ---- libb2l-mesa ---- */

static const struct b2l_mtl_api *mtl;
static struct b2l_mtl_device *mdev;

static void
mesa_open(void)
{
#ifdef B2L_MESA_LIBRARY
	uint64_t t0 = now_us();
	void *mesa = dlopen(B2L_MESA_LIBRARY, RTLD_NOW | RTLD_LOCAL);
	const struct b2l_mtl_api *(*table)(uint32_t) =
	    mesa != NULL ? (const struct b2l_mtl_api *(*)(uint32_t))dlsym(mesa, "b2l_mtl_api") : NULL;
	mtl = table != NULL ? table(B2L_MTL_API_VERSION) : NULL;
	if (mtl == NULL) {
		const char *error = dlerror();
		say("%s: %s; the device has no GPU objects", B2L_MESA_LIBRARY,
		    mesa == NULL ? (error != NULL ? error : "not loaded") :
		    table == NULL ? "no b2l_mtl_api" : "not API version 1");
		return;
	}
	char why[160] = "";
	mdev = mtl->device_open(why, sizeof(why));
	say("libb2l-mesa %s in %llu ms", mdev != NULL ? "opened, Panfrost device ready" : why,
	    (unsigned long long)((now_us() - t0) / 1000));
#else
	say("built without the Mesa library: the device has no GPU objects");
#endif
}

static bool
gpu_ready(const char *what)
{
	if (mtl != NULL && mdev != NULL) {
		return true;
	}
	if (first_time(S(what))) {
		say("%s: no Panfrost device, nil", what);
	}
	return false;
}

/* ---- buffers ---- */

struct buffer_state {
	struct base b;
	struct b2l_mtl_buffer *mb;
	uint64_t length;
	unsigned long options;
	void (^deallocator)(void *, unsigned long);
	void *nocopy;
};

static void
buffer_free(void *p)
{
	struct buffer_state *s = p;
	if (s->mb != NULL) {
		mtl->buffer_release(s->mb);
	}
	if (s->deallocator != NULL) {
		s->deallocator(s->nocopy, (unsigned long)s->length);
		Block_release(s->deallocator);
	}
}

static void
buffer_dealloc(id self, SEL _cmd)
{
	(void)_cmd;
	dealloc_with(self, buffer_free);
}

static id
buffer_make(unsigned long length, const void *bytes, unsigned long options, const char *what)
{
	if (!gpu_ready(what)) {
		return nil;
	}
	char why[160] = "";
	struct b2l_mtl_buffer *mb = mtl->buffer_new(mdev, length, bytes, why, sizeof(why));
	if (mb == NULL) {
		say("%s %lu bytes: %s", what, length, why);
		return nil;
	}
	struct buffer_state *s = calloc(1, sizeof(*s));
	if (s == NULL) {
		mtl->buffer_release(mb);
		return nil;
	}
	s->mb = mb;
	s->length = length;
	s->options = options;
	id o = make_object(gBufferClass, s);
	if (o == nil) {
		mtl->buffer_release(mb);
		free(s);
	}
	return o;
}

static id
device_new_buffer(id self, SEL _cmd, unsigned long length, unsigned long options)
{
	(void)self;
	(void)_cmd;
	return buffer_make(length, NULL, options, "newBufferWithLength:options:");
}

static id
device_new_buffer_bytes(id self, SEL _cmd, const void *bytes, unsigned long length, unsigned long options)
{
	(void)self;
	(void)_cmd;
	return buffer_make(length, bytes, options, "newBufferWithBytes:length:options:");
}

/* The GPU cannot map the caller's memory here: the bytes are copied, and the
 * deallocator is called when the buffer goes. Later CPU writes to the
 * caller's memory do not reach the buffer (logged once). */
static id
device_new_buffer_nocopy(id self, SEL _cmd, void *bytes, unsigned long length, unsigned long options,
    void (^deallocator)(void *, unsigned long))
{
	(void)self;
	if (first_time(_cmd)) {
		say("newBufferWithBytesNoCopy: copied, not shared");
	}
	id o = buffer_make(length, bytes, options, "newBufferWithBytesNoCopy:length:options:deallocator:");
	struct buffer_state *s = state_of(o);
	if (s != NULL && deallocator != NULL) {
		s->deallocator = Block_copy(deallocator);
		s->nocopy = bytes;
	}
	return o;
}

static void *
buffer_contents(id self, SEL _cmd)
{
	(void)_cmd;
	struct buffer_state *s = state_of(self);
	return s != NULL ? mtl->buffer_contents(s->mb) : NULL;
}

static unsigned long
buffer_length(id self, SEL _cmd)
{
	(void)_cmd;
	struct buffer_state *s = state_of(self);
	return s != NULL ? (unsigned long)s->length : 0;
}

static unsigned long
buffer_options(id self, SEL _cmd)
{
	(void)_cmd;
	struct buffer_state *s = state_of(self);
	return s != NULL ? s->options : 0;
}

static unsigned long
buffer_storage_mode(id self, SEL _cmd)
{
	return (buffer_options(self, _cmd) >> 4) & 0xf;
}

static unsigned long
buffer_cache_mode(id self, SEL _cmd)
{
	return buffer_options(self, _cmd) & 0xf;
}

static unsigned long
buffer_hazard_mode(id self, SEL _cmd)
{
	unsigned long mode = (buffer_options(self, _cmd) >> 8) & 0x3;
	return mode != 0 ? mode : 2;    /* MTLHazardTrackingModeTracked */
}

static unsigned long
buffer_allocated_size(id self, SEL _cmd)
{
	return (buffer_length(self, _cmd) + 4095) & ~4095ul;
}

static void
buffer_did_modify(id self, SEL _cmd, CFRange range)
{
	(void)self;
	(void)_cmd;
	(void)range;                    /* the mapping is coherent */
}

static uint64_t
buffer_gpu_address(id self, SEL _cmd)
{
	(void)self;
	if (first_time(_cmd)) {
		say("gpuAddress: 0 (argument buffers are not M2a's)");
	}
	return 0;
}

static struct b2l_mtl_buffer *
mesa_buffer(id buffer)
{
	return buffer != nil && object_getClass(buffer) == gBufferClass ?
	    ((struct buffer_state *)state_of(buffer))->mb : NULL;
}

/* ---- textures ---- */

struct texture_state {
	struct base b;
	struct b2l_mtl_texture *mt;
	struct b2l_mtl_texture_desc desc;
	IOSurfaceRef surface;
	uint32_t plane;
	unsigned long options;
};

static void
texture_free(void *p)
{
	struct texture_state *s = p;
	if (s->mt != NULL) {
		mtl->texture_release(s->mt);
	}
	if (s->surface != NULL) {
		CFRelease(s->surface);
	}
}

static void
texture_dealloc(id self, SEL _cmd)
{
	(void)_cmd;
	dealloc_with(self, texture_free);
}

static void
texture_desc_of(id d, struct b2l_mtl_texture_desc *out)
{
	memset(out, 0, sizeof(*out));
	out->type = (uint32_t)msg_u(d, "textureType");
	out->pixel_format = (uint32_t)msg_u(d, "pixelFormat");
	out->width = (uint32_t)msg_u(d, "width");
	out->height = (uint32_t)msg_u(d, "height");
	out->depth = (uint32_t)msg_u(d, "depth");
	out->mip_levels = (uint32_t)msg_u(d, "mipmapLevelCount");
	out->array_length = (uint32_t)msg_u(d, "arrayLength");
	out->sample_count = (uint32_t)msg_u(d, "sampleCount");
	out->usage = (uint32_t)msg_u(d, "usage");
	out->storage_mode = (uint32_t)msg_u(d, "storageMode");
}

static id
texture_wrap(struct b2l_mtl_texture *mt, const struct b2l_mtl_texture_desc *desc, IOSurfaceRef surface,
    uint32_t plane, unsigned long options)
{
	struct texture_state *s = calloc(1, sizeof(*s));
	if (s == NULL) {
		mtl->texture_release(mt);
		return nil;
	}
	s->mt = mt;
	s->desc = *desc;
	s->surface = surface != NULL ? (IOSurfaceRef)CFRetain(surface) : NULL;
	s->plane = plane;
	s->options = options;
	id o = make_object(gTextureClass, s);
	if (o == nil) {
		texture_free(s);
		free(s);
	}
	return o;
}

static id
device_new_texture(id self, SEL _cmd, id descriptor)
{
	(void)self;
	(void)_cmd;
	if (!gpu_ready("newTextureWithDescriptor:")) {
		return nil;
	}
	struct b2l_mtl_texture_desc desc;
	texture_desc_of(descriptor, &desc);
	char why[160] = "";
	struct b2l_mtl_texture *mt = mtl->texture_new(mdev, &desc, why, sizeof(why));
	if (mt == NULL) {
		say("newTextureWithDescriptor: %ux%u format %u type %u: %s", desc.width, desc.height, desc.pixel_format,
		    desc.type, why);
		return nil;
	}
	return texture_wrap(mt, &desc, NULL, 0, msg_u(descriptor, "resourceOptions"));
}

static id
device_new_texture_iosurface(id self, SEL _cmd, id descriptor, IOSurfaceRef surface, unsigned long plane)
{
	(void)self;
	(void)_cmd;
	if (!gpu_ready("newTextureWithDescriptor:iosurface:plane:")) {
		return nil;
	}
	struct b2l_mtl_texture_desc desc;
	texture_desc_of(descriptor, &desc);
	char why[160] = "";
	struct b2l_mtl_texture *mt = mtl->texture_from_iosurface(mdev, &desc, surface, (uint32_t)plane, why,
	    sizeof(why));
	if (mt == NULL) {
		say("newTextureWithDescriptor:iosurface:%u plane %lu: %s", surface != NULL ? IOSurfaceGetID(surface) : 0,
		    plane, why);
		return nil;
	}
	return texture_wrap(mt, &desc, surface, (uint32_t)plane, msg_u(descriptor, "resourceOptions"));
}

/* MTLRegion: an MTLOrigin and an MTLSize, NSUIntegers. */
typedef struct {
	unsigned long x, y, z;
} mtl_origin;
typedef struct {
	unsigned long width, height, depth;
} mtl_size;
typedef struct {
	mtl_origin origin;
	mtl_size size;
} mtl_region;

static struct b2l_mtl_region
region_of(mtl_origin o, mtl_size s)
{
	struct b2l_mtl_region r = {
		(uint32_t)o.x, (uint32_t)o.y, (uint32_t)o.z, (uint32_t)s.width, (uint32_t)s.height, (uint32_t)s.depth,
	};
	return r;
}

static void
texture_replace_slice(id self, SEL _cmd, mtl_region region, unsigned long level, unsigned long slice,
    const void *bytes, unsigned long bytes_per_row, unsigned long bytes_per_image)
{
	(void)_cmd;
	struct texture_state *s = state_of(self);
	if (s != NULL) {
		struct b2l_mtl_region r = region_of(region.origin, region.size);
		mtl->texture_replace(s->mt, &r, (uint32_t)level, (uint32_t)slice, bytes, bytes_per_row, bytes_per_image);
	}
}

static void
texture_replace(id self, SEL _cmd, mtl_region region, unsigned long level, const void *bytes,
    unsigned long bytes_per_row)
{
	texture_replace_slice(self, _cmd, region, level, 0, bytes, bytes_per_row, 0);
}

static void
texture_get_slice(id self, SEL _cmd, void *bytes, unsigned long bytes_per_row, unsigned long bytes_per_image,
    mtl_region region, unsigned long level, unsigned long slice)
{
	(void)_cmd;
	struct texture_state *s = state_of(self);
	if (s != NULL) {
		struct b2l_mtl_region r = region_of(region.origin, region.size);
		mtl->texture_get(s->mt, &r, (uint32_t)level, (uint32_t)slice, bytes, bytes_per_row, bytes_per_image);
	}
}

static void
texture_get(id self, SEL _cmd, void *bytes, unsigned long bytes_per_row, mtl_region region, unsigned long level)
{
	texture_get_slice(self, _cmd, bytes, bytes_per_row, 0, region, level, 0);
}

#define TEXTURE_FIELD(fn, field)                                  \
	static unsigned long fn(id self, SEL _cmd)                \
	{                                                         \
		(void)_cmd;                                       \
		struct texture_state *s = state_of(self);         \
		return s != NULL ? (unsigned long)(field) : 0;    \
	}
TEXTURE_FIELD(texture_type, s->desc.type)
TEXTURE_FIELD(texture_pixel_format, s->desc.pixel_format)
TEXTURE_FIELD(texture_width, s->desc.width)
TEXTURE_FIELD(texture_height, ONE(s->desc.height))
TEXTURE_FIELD(texture_depth, ONE(s->desc.depth))
TEXTURE_FIELD(texture_levels, ONE(s->desc.mip_levels))
TEXTURE_FIELD(texture_array_length, ONE(s->desc.array_length))
TEXTURE_FIELD(texture_sample_count, ONE(s->desc.sample_count))
TEXTURE_FIELD(texture_usage, s->desc.usage)
TEXTURE_FIELD(texture_storage_mode, s->desc.storage_mode)
TEXTURE_FIELD(texture_plane, s->plane)
TEXTURE_FIELD(texture_options, s->options)
TEXTURE_FIELD(texture_allocated_size, mtl->texture_allocated_size(s->mt))

static IOSurfaceRef
texture_iosurface(id self, SEL _cmd)
{
	(void)_cmd;
	struct texture_state *s = state_of(self);
	return s != NULL ? s->surface : NULL;
}

/* MTLTextureSwizzleChannels: red, green, blue, alpha (2, 3, 4, 5). */
static uint32_t
texture_swizzle(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return 0x05040302u;
}

static struct b2l_mtl_texture *
mesa_texture(id texture)
{
	return texture != nil && object_getClass(texture) == gTextureClass ?
	    ((struct texture_state *)state_of(texture))->mt : NULL;
}

/* ---- samplers and depth-stencil states ---- */

struct sampler_state {
	struct base b;
	struct b2l_mtl_sampler *ms;
};

static void
sampler_free(void *p)
{
	struct sampler_state *s = p;
	if (s->ms != NULL) {
		mtl->sampler_release(s->ms);
	}
}

static void
sampler_dealloc(id self, SEL _cmd)
{
	(void)_cmd;
	dealloc_with(self, sampler_free);
}

static id
device_new_sampler(id self, SEL _cmd, id d)
{
	(void)self;
	(void)_cmd;
	if (!gpu_ready("newSamplerStateWithDescriptor:")) {
		return nil;
	}
	struct b2l_mtl_sampler_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.min_filter = (uint32_t)msg_u(d, "minFilter");
	desc.mag_filter = (uint32_t)msg_u(d, "magFilter");
	desc.mip_filter = (uint32_t)msg_u(d, "mipFilter");
	desc.address[0] = (uint32_t)msg_u(d, "sAddressMode");
	desc.address[1] = (uint32_t)msg_u(d, "tAddressMode");
	desc.address[2] = (uint32_t)msg_u(d, "rAddressMode");
	desc.compare = (uint32_t)msg_u(d, "compareFunction");
	desc.border = (uint32_t)msg_u(d, "borderColor");
	desc.max_anisotropy = (uint32_t)msg_u(d, "maxAnisotropy");
	desc.normalized = msg_b(d, "normalizedCoordinates");
	desc.lod_min = msg_f(d, "lodMinClamp");
	desc.lod_max = msg_f(d, "lodMaxClamp");
	struct b2l_mtl_sampler *ms = mtl->sampler_new(mdev, &desc);
	struct sampler_state *s = ms != NULL ? calloc(1, sizeof(*s)) : NULL;
	if (s == NULL) {
		say("newSamplerStateWithDescriptor: refused");
		if (ms != NULL) {
			mtl->sampler_release(ms);
		}
		return nil;
	}
	s->ms = ms;
	return make_object(gSamplerClass, s);
}

struct depth_stencil_state {
	struct base b;
	struct b2l_mtl_depth_stencil *md;
};

static void
depth_stencil_free(void *p)
{
	struct depth_stencil_state *s = p;
	if (s->md != NULL) {
		mtl->depth_stencil_release(s->md);
	}
}

static void
depth_stencil_dealloc(id self, SEL _cmd)
{
	(void)_cmd;
	dealloc_with(self, depth_stencil_free);
}

static void
stencil_of(id d, struct b2l_mtl_stencil_desc *out)
{
	out->compare = (uint32_t)msg_u(d, "stencilCompareFunction");
	out->fail = (uint32_t)msg_u(d, "stencilFailureOperation");
	out->depth_fail = (uint32_t)msg_u(d, "depthFailureOperation");
	out->pass = (uint32_t)msg_u(d, "depthStencilPassOperation");
	out->read_mask = (uint32_t)msg_u(d, "readMask");
	out->write_mask = (uint32_t)msg_u(d, "writeMask");
}

static id
device_new_depth_stencil(id self, SEL _cmd, id d)
{
	(void)self;
	(void)_cmd;
	if (!gpu_ready("newDepthStencilStateWithDescriptor:")) {
		return nil;
	}
	struct b2l_mtl_depth_stencil_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.depth_compare = (uint32_t)msg_u(d, "depthCompareFunction");
	desc.depth_write = msg_b(d, "isDepthWriteEnabled");
	id front = msg(d, "frontFaceStencil"), back = msg(d, "backFaceStencil");
	desc.stencil = front != nil || back != nil;
	stencil_of(front != nil ? front : back, &desc.front);
	stencil_of(back != nil ? back : front, &desc.back);
	struct b2l_mtl_depth_stencil *md = mtl->depth_stencil_new(mdev, &desc);
	struct depth_stencil_state *s = md != NULL ? calloc(1, sizeof(*s)) : NULL;
	if (s == NULL) {
		say("newDepthStencilStateWithDescriptor: refused");
		if (md != NULL) {
			mtl->depth_stencil_release(md);
		}
		return nil;
	}
	s->md = md;
	return make_object(gDepthStencilClass, s);
}

/* ---- libraries and functions ---- */

struct library_state {
	struct base b;
	struct b2l_mtl_library *ml;
	id names;                       /* NSArray of NSString, made once */
};

struct function_state {
	struct base b;
	struct b2l_mtl_function *mf;
	id name;
	id library;                     /* retained */
};

static void
library_free(void *p)
{
	struct library_state *s = p;
	if (s->ml != NULL) {
		mtl->library_release(s->ml);
	}
	if (s->names != nil) {
		objc_release(s->names);
	}
}

static void
library_dealloc(id self, SEL _cmd)
{
	(void)_cmd;
	dealloc_with(self, library_free);
}

static id
library_wrap(struct b2l_mtl_library *ml, const char *where, uint64_t t0)
{
	struct library_state *s = calloc(1, sizeof(*s));
	if (s == NULL) {
		mtl->library_release(ml);
		return nil;
	}
	s->ml = ml;
	say("library %s: %u functions in %llu ms", where, mtl->library_count(ml),
	    (unsigned long long)((now_us() - t0) / 1000));
	return make_object(gLibraryClass, s);
}

static id
library_from_path(const char *path, id *error)
{
	if (!gpu_ready("newLibraryWithURL:error:")) {
		set_error(error, 1, "no Panfrost device");
		return nil;
	}
	uint64_t t0 = now_us();
	char why[192] = "";
	struct b2l_mtl_library *ml = mtl->library_open(mdev, path, why, sizeof(why));
	if (ml == NULL) {
		say("library %s: %s", path, why);
		set_error(error, 1, why);
		return nil;
	}
	return library_wrap(ml, path, t0);
}

static id
device_new_library_url(id self, SEL _cmd, id url, id *error)
{
	(void)self;
	(void)_cmd;
	char path[1024];
	cstring(msg(url, "path"), path, sizeof(path));
	return library_from_path(path, error);
}

static id
device_new_library_file(id self, SEL _cmd, id file, id *error)
{
	(void)self;
	(void)_cmd;
	char path[1024];
	cstring(file, path, sizeof(path));
	return library_from_path(path, error);
}

static id
device_new_library_data(id self, SEL _cmd, dispatch_data_t data, id *error)
{
	(void)self;
	(void)_cmd;
	if (!gpu_ready("newLibraryWithData:error:")) {
		set_error(error, 1, "no Panfrost device");
		return nil;
	}
	const void *bytes = NULL;
	size_t size = 0;
	dispatch_data_t map = data != NULL ? dispatch_data_create_map(data, &bytes, &size) : NULL;
	uint64_t t0 = now_us();
	char why[192] = "";
	struct b2l_mtl_library *ml = mtl->library_new_data(mdev, bytes, size, why, sizeof(why));
	if (map != NULL) {
		dispatch_release(map);
	}
	if (ml == NULL) {
		say("newLibraryWithData: %zu bytes: %s", size, why);
		set_error(error, 1, why);
		return nil;
	}
	return library_wrap(ml, "from data", t0);
}

static id
device_new_default_library_bundle(id self, SEL _cmd, id bundle, id *error)
{
	(void)self;
	(void)_cmd;
	id url = ((id (*)(id, SEL, id, id))objc_msgSend)(bundle, S("URLForResource:withExtension:"),
	    (id)CFSTR("default"), (id)CFSTR("metallib"));
	if (url == nil) {
		set_error(error, 1, "the bundle has no default.metallib");
		return nil;
	}
	char path[1024];
	cstring(msg(url, "path"), path, sizeof(path));
	return library_from_path(path, error);
}

static id
device_new_default_library(id self, SEL _cmd)
{
	return device_new_default_library_bundle(self, _cmd, msg((id)objc_getClass("NSBundle"), "mainBundle"), NULL);
}

static id
library_function_names(id self, SEL _cmd)
{
	(void)_cmd;
	struct library_state *s = state_of(self);
	if (s == NULL) {
		return nil;
	}
	if (s->names == nil) {
		uint32_t n = mtl->library_count(s->ml);
		CFMutableArrayRef names = CFArrayCreateMutable(NULL, n, &kCFTypeArrayCallBacks);
		for (uint32_t i = 0; i < n; i++) {
			CFStringRef name = CFStringCreateWithCString(NULL, mtl->library_name(s->ml, i), kCFStringEncodingUTF8);
			if (name != NULL) {
				CFArrayAppendValue(names, name);
				CFRelease(name);
			}
		}
		s->names = (id)names;
	}
	return s->names;
}

/* Bytes of an MTLDataType's value (0: not a constant this driver reads). */
static unsigned long
data_type_size(unsigned long type)
{
	switch (type) {
	case 45: case 49: case 53: return 1;                    /* char, uchar, bool */
	case 46: case 50: case 54: case 16: case 37: case 41: case 121: return 2;
	case 47: case 51: case 55: return 3;
	case 48: case 52: case 56: case 3: case 29: case 33: case 17: case 38: case 42: return 4;
	case 18: case 39: case 43: return 6;
	case 4: case 30: case 34: case 19: case 40: case 44: case 81: case 85: return 8;
	case 5: case 31: case 35: return 12;
	case 6: case 32: case 36: return 16;
	default: return 0;
	}
}

/* An MTLIndexedConstantValue / MTLNamedConstantValue's -data: a pointer, or
 * an object with -bytes, as its method's type says. */
static const void *
constant_bytes(id value)
{
	Method m = class_getInstanceMethod(object_getClass(value), S("data"));
	const char *types = m != NULL ? method_getTypeEncoding(m) : NULL;
	if (types == NULL) {
		return NULL;
	}
	while (*types == 'r' || *types == 'n' || *types == 'N' || *types == 'o' || *types == 'O' || *types == 'V') {
		types++;
	}
	if (*types == '@') {
		id data = msg(value, "data");
		return data != nil ? ((const void *(*)(id, SEL))objc_msgSend)(data, S("bytes")) : NULL;
	}
	if (*types == '^') {
		return ((const void *(*)(id, SEL))objc_msgSend)(value, S("data"));
	}
	return NULL;
}

#define MAX_CONSTANTS 96

/* The values of an MTLFunctionConstantValues, through Metal's own
 * newIndexedConstantArray / newNamedConstantArray (the class has no public
 * getter; both are in the 23G90 cache). Names are copied into `names`. */
static unsigned
constants_of(id values, struct b2l_mtl_constant *out, char (*names)[64])
{
	if (values == nil) {
		return 0;
	}
	unsigned n = 0;
	static const char *const arrays[] = { "newIndexedConstantArray", "newNamedConstantArray" };
	for (unsigned a = 0; a < 2; a++) {
		if (!responds(values, arrays[a])) {
			if (first_time(S(arrays[a]))) {
				say("MTLFunctionConstantValues (%s) has no -%s: its constants are not read",
				    class_getName(object_getClass(values)), arrays[a]);
			}
			continue;
		}
		id array = msg(values, arrays[a]);
		unsigned long count = msg_u(array, "count");
		for (unsigned long i = 0; i < count && n < MAX_CONSTANTS; i++) {
			id v = msg_at(array, i);
			unsigned long type = msg_u(v, "dataType");
			unsigned long size = data_type_size(type);
			const void *bytes = constant_bytes(v);
			if (size == 0 || bytes == NULL) {
				if (first_time(S("constant type"))) {
					say("a function constant of type %lu (%s) not read", type, class_getName(object_getClass(v)));
				}
				continue;
			}
			struct b2l_mtl_constant *c = &out[n];
			memset(c, 0, sizeof(*c));
			c->data_type = (uint32_t)type;
			c->size = (uint32_t)(size < sizeof(c->bytes) ? size : sizeof(c->bytes));
			memcpy(c->bytes, bytes, c->size);
			if (a == 0) {
				c->index = (int32_t)msg_u(v, "index");
			} else {
				c->index = -1;
				cstring(msg(v, "name"), names[n], sizeof(names[n]));
				c->name = names[n];
			}
			n++;
		}
		if (array != nil) {
			objc_release(array);
		}
	}
	return n;
}

static void
function_free(void *p)
{
	struct function_state *s = p;
	if (s->mf != NULL) {
		mtl->function_release(s->mf);
	}
	if (s->name != nil) {
		objc_release(s->name);
	}
	if (s->library != nil) {
		objc_release(s->library);
	}
}

static void
function_dealloc(id self, SEL _cmd)
{
	(void)_cmd;
	dealloc_with(self, function_free);
}

static id
library_function(id self, id name, id values, id *error)
{
	struct library_state *s = state_of(self);
	char text[256];
	cstring(name, text, sizeof(text));
	int32_t index = s != NULL ? mtl->library_find(s->ml, text) : -1;
	if (index < 0) {
		set_error(error, 5, "no such function");
		return nil;
	}
	struct b2l_mtl_constant constants[MAX_CONSTANTS];
	char (*names)[64] = calloc(MAX_CONSTANTS, 64);
	unsigned count = names != NULL ? constants_of(values, constants, names) : 0;
	char why[160] = "";
	struct b2l_mtl_function *mf = mtl->function_new(s->ml, (uint32_t)index, constants, count, why, sizeof(why));
	free(names);
	if (why[0] != 0) {
		say("function %s (%u constants given): %s", text, count, why);
	}
	struct function_state *f = mf != NULL ? calloc(1, sizeof(*f)) : NULL;
	if (f == NULL) {
		if (mf != NULL) {
			mtl->function_release(mf);
		}
		set_error(error, 5, why[0] != 0 ? why : "refused");
		return nil;
	}
	f->mf = mf;
	f->name = nsstring(text);
	f->library = objc_retain(self);
	return make_object(gFunctionClass, f);
}

static id
library_new_function(id self, SEL _cmd, id name)
{
	(void)_cmd;
	return library_function(self, name, nil, NULL);
}

static id
library_new_function_constants(id self, SEL _cmd, id name, id values, id *error)
{
	(void)_cmd;
	return library_function(self, name, values, error);
}

/* MTLFunctionDescriptor: Core Animation's way (name and constantValues). */
static id
library_new_function_descriptor(id self, SEL _cmd, id descriptor, id *error)
{
	(void)_cmd;
	return library_function(self, msg(descriptor, "name"), msg(descriptor, "constantValues"), error);
}

static id
function_name_of(id self, SEL _cmd)
{
	(void)_cmd;
	struct function_state *s = state_of(self);
	return s != NULL ? s->name : nil;
}

static unsigned long
function_type(id self, SEL _cmd)
{
	(void)_cmd;
	struct function_state *s = state_of(self);
	switch (s != NULL ? mtl->function_stage(s->mf) : -1) {
	case 0: return 1;               /* MTLFunctionTypeVertex */
	case 4: return 2;               /* Fragment */
	case 5: return 3;               /* Kernel */
	default: return 5;              /* Visible */
	}
}

static id
function_empty_dictionary(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return objc_autorelease((id)CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks));
}

static id
function_empty_array(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return objc_autorelease((id)CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks));
}

static struct b2l_mtl_function *
mesa_function(id function)
{
	return function != nil && object_getClass(function) == gFunctionClass ?
	    ((struct function_state *)state_of(function))->mf : NULL;
}

/* ---- render pipelines ---- */

struct pipeline_state {
	struct base b;
	struct b2l_mtl_render_pipeline *mp;
};

static void
pipeline_free(void *p)
{
	struct pipeline_state *s = p;
	if (s->mp != NULL) {
		mtl->render_pipeline_release(s->mp);
	}
}

static void
pipeline_dealloc(id self, SEL _cmd)
{
	(void)_cmd;
	dealloc_with(self, pipeline_free);
}

static id
device_new_render_pipeline_options(id self, SEL _cmd, id d, unsigned long options, id *reflection, id *error)
{
	(void)self;
	(void)_cmd;
	(void)options;
	if (reflection != NULL) {
		*reflection = nil;
	}
	if (!gpu_ready("newRenderPipelineStateWithDescriptor:error:")) {
		set_error(error, 1, "no Panfrost device");
		return nil;
	}
	struct b2l_mtl_render_pipeline_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.vertex = mesa_function(msg(d, "vertexFunction"));
	desc.fragment = mesa_function(msg(d, "fragmentFunction"));
	id colors = msg(d, "colorAttachments");
	for (unsigned i = 0; i < B2L_MTL_MAX_COLORS; i++) {
		id c = msg_at(colors, i);
		struct b2l_mtl_color_desc *o = &desc.color[i];
		o->pixel_format = (uint32_t)msg_u(c, "pixelFormat");
		o->blend = msg_b(c, "isBlendingEnabled");
		o->rgb_op = (uint32_t)msg_u(c, "rgbBlendOperation");
		o->alpha_op = (uint32_t)msg_u(c, "alphaBlendOperation");
		o->src_rgb = (uint32_t)msg_u(c, "sourceRGBBlendFactor");
		o->dst_rgb = (uint32_t)msg_u(c, "destinationRGBBlendFactor");
		o->src_alpha = (uint32_t)msg_u(c, "sourceAlphaBlendFactor");
		o->dst_alpha = (uint32_t)msg_u(c, "destinationAlphaBlendFactor");
		o->write_mask = (uint32_t)msg_u(c, "writeMask");
	}
	desc.depth_format = (uint32_t)msg_u(d, "depthAttachmentPixelFormat");
	desc.stencil_format = (uint32_t)msg_u(d, "stencilAttachmentPixelFormat");
	desc.sample_count = (uint32_t)msg_u(d, responds(d, "rasterSampleCount") ? "rasterSampleCount" : "sampleCount");
	desc.topology_class = (uint32_t)msg_u(d, "inputPrimitiveTopology");
	desc.alpha_to_coverage = msg_b(d, "isAlphaToCoverageEnabled");
	desc.rasterization = !responds(d, "isRasterizationEnabled") || msg_b(d, "isRasterizationEnabled");
	id vd = msg(d, "vertexDescriptor");
	if (vd != nil) {
		desc.vertex_descriptor = true;
		id attributes = msg(vd, "attributes"), layouts = msg(vd, "layouts");
		for (unsigned i = 0; i < B2L_MTL_MAX_ATTRIBUTES; i++) {
			id a = msg_at(attributes, i);
			desc.attribute[i].format = (uint32_t)msg_u(a, "format");
			desc.attribute[i].offset = (uint32_t)msg_u(a, "offset");
			desc.attribute[i].buffer = (uint32_t)msg_u(a, "bufferIndex");
		}
		for (unsigned i = 0; i < B2L_MTL_MAX_BUFFERS; i++) {
			id l = msg_at(layouts, i);
			desc.layout[i].stride = (uint32_t)msg_u(l, "stride");
			desc.layout[i].step_function = (uint32_t)msg_u(l, "stepFunction");
			desc.layout[i].step_rate = (uint32_t)msg_u(l, "stepRate");
		}
	}
	char vname[128], fname[128];
	cstring(msg(msg(d, "vertexFunction"), "name"), vname, sizeof(vname));
	cstring(msg(msg(d, "fragmentFunction"), "name"), fname, sizeof(fname));
	if (desc.vertex == NULL) {
		say("render pipeline %s + %s: the vertex function is not one of ours", vname, fname);
		set_error(error, 1, "no vertex function of this device");
		return nil;
	}
	struct b2l_mtl_pipeline_report report;
	char why[192] = "";
	struct b2l_mtl_render_pipeline *mp = mtl->render_pipeline_new(mdev, &desc, &report, why, sizeof(why));
	if (mp == NULL) {
		say("render pipeline %s + %s refused: %s", vname, fname, why);
		set_error(error, 1, why);
		return nil;
	}
	struct pipeline_state *s = calloc(1, sizeof(*s));
	if (s == NULL) {
		mtl->render_pipeline_release(mp);
		return nil;
	}
	s->mp = mp;
	return make_object(gRenderPipelineClass, s);
}

static id
device_new_render_pipeline(id self, SEL _cmd, id d, id *error)
{
	return device_new_render_pipeline_options(self, _cmd, d, 0, NULL, error);
}

/* ---- command buffers ---- */

enum {
	CB_NOT_ENQUEUED = 0, CB_ENQUEUED = 1, CB_COMMITTED = 2, CB_SCHEDULED = 3, CB_COMPLETED = 4, CB_ERROR = 5,
};

#define MAX_HANDLERS 32

typedef void (^handler_t)(id);

struct cmdbuf_state {
	struct base b;
	struct b2l_mtl_cmdbuf *mc;
	id queue;                       /* retained */
	handler_t completed[MAX_HANDLERS], scheduled[MAX_HANDLERS];
	unsigned num_completed, num_scheduled;
	unsigned long status;
	id error;                       /* retained NSError */
	bool committed;
	double gpu_start, gpu_end;
};

static dispatch_queue_t handler_queue;
static pthread_once_t handler_once = PTHREAD_ONCE_INIT;

static void
handler_queue_make(void)
{
	handler_queue = dispatch_queue_create("dev.b2l.metal.completion", NULL);
}

static void
cmdbuf_free(void *p)
{
	struct cmdbuf_state *s = p;
	if (s->mc != NULL) {
		mtl->cmdbuf_release(s->mc);
	}
	for (unsigned i = 0; i < s->num_completed; i++) {
		Block_release(s->completed[i]);
	}
	for (unsigned i = 0; i < s->num_scheduled; i++) {
		Block_release(s->scheduled[i]);
	}
	if (s->queue != nil) {
		objc_release(s->queue);
	}
	if (s->error != nil) {
		objc_release(s->error);
	}
}

static void
cmdbuf_dealloc(id self, SEL _cmd)
{
	(void)_cmd;
	dealloc_with(self, cmdbuf_free);
}

static id
queue_command_buffer(id self, SEL _cmd)
{
	(void)_cmd;
	if (!gpu_ready("commandBuffer")) {
		return nil;
	}
	struct cmdbuf_state *s = calloc(1, sizeof(*s));
	struct b2l_mtl_cmdbuf *mc = s != NULL ? mtl->cmdbuf_new(mdev) : NULL;
	if (mc == NULL) {
		free(s);
		return nil;
	}
	s->mc = mc;
	s->queue = objc_retain(self);
	id o = make_object(gCommandBufferClass, s);
	if (o == nil) {
		cmdbuf_free(s);
		free(s);
		return nil;
	}
	return objc_autorelease(o);
}

static id
queue_command_buffer_descriptor(id self, SEL _cmd, id descriptor)
{
	(void)descriptor;
	return queue_command_buffer(self, _cmd);
}

/* On the completion queue: the handlers, then the reference commit took. */
static void
cmdbuf_handlers(void *context)
{
	id self = context;
	struct cmdbuf_state *s = state_of(self);
	void *pool = objc_autoreleasePoolPush();
	for (unsigned i = 0; s != NULL && i < s->num_scheduled; i++) {
		s->scheduled[i](self);
	}
	for (unsigned i = 0; s != NULL && i < s->num_completed; i++) {
		s->completed[i](self);
	}
	objc_autoreleasePoolPop(pool);
	objc_release(self);
}

static unsigned long committed_count;

static void
cmdbuf_commit(id self, SEL _cmd)
{
	(void)_cmd;
	struct cmdbuf_state *s = state_of(self);
	if (s == NULL || s->committed) {
		return;
	}
	s->committed = true;
	s->status = CB_SCHEDULED;
	uint64_t gpu_us = 0;
	char why[192] = "";
	double start = CFAbsoluteTimeGetCurrent();
	int result = mtl->cmdbuf_execute(s->mc, &gpu_us, why, sizeof(why));
	s->gpu_start = start;
	s->gpu_end = start + (double)gpu_us / 1e6;
	unsigned long n = __atomic_add_fetch(&committed_count, 1, __ATOMIC_RELAXED);
	if (result != 0) {
		s->status = CB_ERROR;
		/* MTLCommandBufferErrorInternal (1) */
		s->error = objc_retain(make_error("MTLCommandBufferErrorDomain", 1, why));
		say("command buffer %lu: error: %s", n, why);
	} else {
		s->status = CB_COMPLETED;
		if (n <= 8) {
			say("command buffer %lu: done in %llu us", n, (unsigned long long)gpu_us);
		}
	}
	pthread_once(&handler_once, handler_queue_make);
	dispatch_async_f(handler_queue, objc_retain(self), cmdbuf_handlers);
}

static void
cmdbuf_add_completed(id self, SEL _cmd, handler_t handler)
{
	(void)_cmd;
	struct cmdbuf_state *s = state_of(self);
	if (s != NULL && !s->committed && handler != NULL && s->num_completed < MAX_HANDLERS) {
		s->completed[s->num_completed++] = Block_copy(handler);
	}
}

static void
cmdbuf_add_scheduled(id self, SEL _cmd, handler_t handler)
{
	(void)_cmd;
	struct cmdbuf_state *s = state_of(self);
	if (s != NULL && !s->committed && handler != NULL && s->num_scheduled < MAX_HANDLERS) {
		s->scheduled[s->num_scheduled++] = Block_copy(handler);
	}
}

static unsigned long
cmdbuf_status(id self, SEL _cmd)
{
	(void)_cmd;
	struct cmdbuf_state *s = state_of(self);
	return s != NULL ? s->status : CB_NOT_ENQUEUED;
}

static id
cmdbuf_error(id self, SEL _cmd)
{
	(void)_cmd;
	struct cmdbuf_state *s = state_of(self);
	return s != NULL ? s->error : nil;
}

static id
cmdbuf_queue(id self, SEL _cmd)
{
	(void)_cmd;
	struct cmdbuf_state *s = state_of(self);
	return s != NULL ? s->queue : nil;
}

static void
cmdbuf_enqueue(id self, SEL _cmd)
{
	(void)_cmd;
	struct cmdbuf_state *s = state_of(self);
	if (s != NULL && s->status == CB_NOT_ENQUEUED) {
		s->status = CB_ENQUEUED;
	}
}

/* The GPU's work is done when commit returns (B2LMali runs each job to its
 * end), so there is nothing to wait for. */
static void
cmdbuf_wait(id self, SEL _cmd)
{
	(void)_cmd;
	struct cmdbuf_state *s = state_of(self);
	if (s != NULL && !s->committed && first_time(S("waitUntilCompleted before commit"))) {
		say("waitUntilCompleted on a command buffer not committed");
	}
}

static double
cmdbuf_gpu_start(id self, SEL _cmd)
{
	(void)_cmd;
	struct cmdbuf_state *s = state_of(self);
	return s != NULL ? s->gpu_start : 0;
}

static double
cmdbuf_gpu_end(id self, SEL _cmd)
{
	(void)_cmd;
	struct cmdbuf_state *s = state_of(self);
	return s != NULL ? s->gpu_end : 0;
}

/* ---- encoders ---- */

struct encoder_state {
	struct base b;
	id cmdbuf;                      /* retained */
	bool ended;
};

static void
encoder_free(void *p)
{
	struct encoder_state *s = p;
	if (s->cmdbuf != nil) {
		objc_release(s->cmdbuf);
	}
}

static void
encoder_dealloc(id self, SEL _cmd)
{
	(void)_cmd;
	dealloc_with(self, encoder_free);
}

/* The command buffer an encoder records into, while it may. */
static struct b2l_mtl_cmdbuf *
recording(id encoder)
{
	struct encoder_state *e = state_of(encoder);
	struct cmdbuf_state *c = e != NULL && !e->ended ? state_of(e->cmdbuf) : NULL;
	return c != NULL && !c->committed ? c->mc : NULL;
}

static id
encoder_make(Class cls, id cmdbuf)
{
	struct encoder_state *e = calloc(1, sizeof(*e));
	if (e == NULL) {
		return nil;
	}
	e->cmdbuf = objc_retain(cmdbuf);
	id o = make_object(cls, e);
	if (o == nil) {
		encoder_free(e);
		free(e);
		return nil;
	}
	return objc_autorelease(o);
}

static void
attachment_of(id a, struct b2l_mtl_attachment *out)
{
	memset(out, 0, sizeof(*out));
	id texture = msg(a, "texture");
	if (texture == nil) {
		return;
	}
	out->texture = mesa_texture(texture);
	if (out->texture == NULL && first_time(S("foreign attachment"))) {
		say("render pass: an attachment that is not this device's texture (%s)",
		    class_getName(object_getClass(texture)));
	}
	out->level = (uint32_t)msg_u(a, "level");
	out->slice = (uint32_t)msg_u(a, "slice");
	out->depth_plane = (uint32_t)msg_u(a, "depthPlane");
	out->load = (uint32_t)msg_u(a, "loadAction");
	out->store = (uint32_t)msg_u(a, "storeAction");
}

typedef struct {
	double red, green, blue, alpha;
} mtl_clear_color;

static id
cmdbuf_render_encoder(id self, SEL _cmd, id pass)
{
	(void)_cmd;
	struct cmdbuf_state *c = state_of(self);
	if (c == NULL || c->committed || gRenderEncoderClass == Nil) {
		return nil;
	}
	struct b2l_mtl_pass_desc desc;
	memset(&desc, 0, sizeof(desc));
	id colors = msg(pass, "colorAttachments");
	for (unsigned i = 0; i < B2L_MTL_MAX_COLORS; i++) {
		id a = msg_at(colors, i);
		attachment_of(a, &desc.color[i]);
		if (desc.color[i].texture != NULL) {
			mtl_clear_color cc = ((mtl_clear_color (*)(id, SEL))objc_msgSend)(a, S("clearColor"));
			desc.color[i].clear[0] = cc.red;
			desc.color[i].clear[1] = cc.green;
			desc.color[i].clear[2] = cc.blue;
			desc.color[i].clear[3] = cc.alpha;
		}
	}
	id depth = msg(pass, "depthAttachment"), stencil = msg(pass, "stencilAttachment");
	attachment_of(depth, &desc.depth);
	desc.depth.clear[0] = msg_d(depth, "clearDepth");
	attachment_of(stencil, &desc.stencil);
	desc.stencil.clear_stencil = (uint32_t)msg_u(stencil, "clearStencil");
	desc.width = (uint32_t)msg_u(pass, "renderTargetWidth");
	desc.height = (uint32_t)msg_u(pass, "renderTargetHeight");
	mtl->cmdbuf_pass_begin(c->mc, &desc);
	return encoder_make(gRenderEncoderClass, self);
}

static id
cmdbuf_blit_encoder(id self, SEL _cmd)
{
	(void)_cmd;
	struct cmdbuf_state *c = state_of(self);
	return c != NULL && !c->committed ? encoder_make(gBlitEncoderClass, self) : nil;
}

static id
cmdbuf_blit_encoder_descriptor(id self, SEL _cmd, id descriptor)
{
	(void)descriptor;
	return cmdbuf_blit_encoder(self, _cmd);
}

static id
cmdbuf_compute_encoder(id self, SEL _cmd)
{
	(void)_cmd;
	struct cmdbuf_state *c = state_of(self);
	if (c == NULL || c->committed) {
		return nil;
	}
	mtl->cmdbuf_unsupported(c->mc, "compute encoders (M4)");
	return encoder_make(gComputeEncoderClass, self);
}

static id
cmdbuf_compute_encoder_type(id self, SEL _cmd, unsigned long type)
{
	(void)type;
	return cmdbuf_compute_encoder(self, _cmd);
}

static void
render_end(id self, SEL _cmd)
{
	(void)_cmd;
	struct b2l_mtl_cmdbuf *mc = recording(self);
	if (mc != NULL) {
		mtl->cmdbuf_pass_end(mc);
	}
	struct encoder_state *e = state_of(self);
	if (e != NULL) {
		e->ended = true;
	}
}

static void
encoder_end(id self, SEL _cmd)
{
	(void)_cmd;
	struct encoder_state *e = state_of(self);
	if (e != NULL) {
		e->ended = true;
	}
}

static void
encoder_debug_group(id self, SEL _cmd, id name)
{
	(void)self;
	(void)_cmd;
	(void)name;
}

/* M2a has no draws: recorded as not implemented, the command buffer's error. */
static void
render_draw(id self, SEL _cmd)
{
	struct b2l_mtl_cmdbuf *mc = recording(self);
	if (mc != NULL) {
		mtl->cmdbuf_unsupported(mc, sel_getName(_cmd));
	}
	if (first_time(_cmd)) {
		say("-%s: draws are M2b, recorded as not implemented", sel_getName(_cmd));
	}
}

static void
render_draw3(id self, SEL _cmd, unsigned long a, unsigned long b, unsigned long c)
{
	(void)a;
	(void)b;
	(void)c;
	render_draw(self, _cmd);
}

static void
render_draw4(id self, SEL _cmd, unsigned long a, unsigned long b, unsigned long c, unsigned long d)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	render_draw(self, _cmd);
}

static void
render_draw_indexed(id self, SEL _cmd, unsigned long type, unsigned long count, unsigned long index_type,
    id buffer, unsigned long offset)
{
	(void)type;
	(void)count;
	(void)index_type;
	(void)buffer;
	(void)offset;
	render_draw(self, _cmd);
}

static void
render_draw_indexed_instanced(id self, SEL _cmd, unsigned long type, unsigned long count,
    unsigned long index_type, id buffer, unsigned long offset, unsigned long instances)
{
	(void)instances;
	render_draw_indexed(self, _cmd, type, count, index_type, buffer, offset);
}

static void
blit_copy_buffer(id self, SEL _cmd, id from, unsigned long from_offset, id to, unsigned long to_offset,
    unsigned long size)
{
	(void)_cmd;
	struct b2l_mtl_cmdbuf *mc = recording(self);
	struct b2l_mtl_buffer *a = mesa_buffer(from), *b = mesa_buffer(to);
	if (mc == NULL) {
		return;
	}
	if (a == NULL || b == NULL) {
		mtl->cmdbuf_unsupported(mc, "copyFromBuffer: a buffer that is not this device's");
		return;
	}
	mtl->cmdbuf_copy_buffer(mc, a, from_offset, b, to_offset, size);
}

static void
blit_fill(id self, SEL _cmd, id buffer, CFRange range, uint8_t value)
{
	(void)_cmd;
	struct b2l_mtl_cmdbuf *mc = recording(self);
	struct b2l_mtl_buffer *b = mesa_buffer(buffer);
	if (mc == NULL) {
		return;
	}
	if (b == NULL) {
		mtl->cmdbuf_unsupported(mc, "fillBuffer: a buffer that is not this device's");
		return;
	}
	mtl->cmdbuf_fill_buffer(mc, b, (uint64_t)range.location, (uint64_t)range.length, value);
}

static void
blit_copy_texture(id self, SEL _cmd, id from, unsigned long from_slice, unsigned long from_level,
    mtl_origin from_origin, mtl_size size, id to, unsigned long to_slice, unsigned long to_level, mtl_origin to_origin)
{
	(void)_cmd;
	struct b2l_mtl_cmdbuf *mc = recording(self);
	struct b2l_mtl_texture *a = mesa_texture(from), *b = mesa_texture(to);
	if (mc == NULL) {
		return;
	}
	if (a == NULL || b == NULL) {
		mtl->cmdbuf_unsupported(mc, "copyFromTexture: a texture that is not this device's");
		return;
	}
	struct b2l_mtl_region r = region_of(from_origin, size);
	mtl->cmdbuf_copy_texture(mc, a, (uint32_t)from_slice, (uint32_t)from_level, &r, b, (uint32_t)to_slice,
	    (uint32_t)to_level, (uint32_t)to_origin.x, (uint32_t)to_origin.y, (uint32_t)to_origin.z);
}

/* copyFromTexture:toTexture: the whole of level 0 of slice 0 (what M2a needs). */
static void
blit_copy_texture_whole(id self, SEL _cmd, id from, id to)
{
	mtl_origin zero = { 0, 0, 0 };
	mtl_size size = { msg_u(from, "width"), msg_u(from, "height"), msg_u(from, "depth") };
	if (msg_u(from, "mipmapLevelCount") > 1 || msg_u(from, "arrayLength") > 1) {
		struct b2l_mtl_cmdbuf *mc = recording(self);
		if (mc != NULL) {
			mtl->cmdbuf_unsupported(mc, "copyFromTexture:toTexture: of more than one level or slice");
		}
		return;
	}
	blit_copy_texture(self, _cmd, from, 0, 0, zero, size, to, 0, 0, zero);
}

static void
blit_buffer_to_texture(id self, SEL _cmd, id from, unsigned long offset, unsigned long bytes_per_row,
    unsigned long bytes_per_image, mtl_size size, id to, unsigned long slice, unsigned long level, mtl_origin origin)
{
	(void)_cmd;
	struct b2l_mtl_cmdbuf *mc = recording(self);
	struct b2l_mtl_buffer *b = mesa_buffer(from);
	struct b2l_mtl_texture *t = mesa_texture(to);
	if (mc == NULL) {
		return;
	}
	if (b == NULL || t == NULL) {
		mtl->cmdbuf_unsupported(mc, "copyFromBuffer:toTexture: an object that is not this device's");
		return;
	}
	struct b2l_mtl_region r = region_of(origin, size);
	mtl->cmdbuf_copy_buffer_to_texture(mc, b, offset, bytes_per_row, bytes_per_image, &r, t, (uint32_t)slice,
	    (uint32_t)level);
}

static void
blit_texture_to_buffer(id self, SEL _cmd, id from, unsigned long slice, unsigned long level, mtl_origin origin,
    mtl_size size, id to, unsigned long offset, unsigned long bytes_per_row, unsigned long bytes_per_image)
{
	(void)_cmd;
	struct b2l_mtl_cmdbuf *mc = recording(self);
	struct b2l_mtl_texture *t = mesa_texture(from);
	struct b2l_mtl_buffer *b = mesa_buffer(to);
	if (mc == NULL) {
		return;
	}
	if (b == NULL || t == NULL) {
		mtl->cmdbuf_unsupported(mc, "copyFromTexture:toBuffer: an object that is not this device's");
		return;
	}
	struct b2l_mtl_region r = region_of(origin, size);
	mtl->cmdbuf_copy_texture_to_buffer(mc, t, (uint32_t)slice, (uint32_t)level, &r, b, offset, bytes_per_row,
	    bytes_per_image);
}

static void
blit_mipmaps(id self, SEL _cmd, id texture)
{
	(void)texture;
	struct b2l_mtl_cmdbuf *mc = recording(self);
	if (mc != NULL) {
		mtl->cmdbuf_unsupported(mc, sel_getName(_cmd));
	}
}

static void
blit_synchronize_texture(id self, SEL _cmd, id texture, unsigned long slice, unsigned long level)
{
	(void)self;
	(void)_cmd;
	(void)texture;
	(void)slice;
	(void)level;
}

/* ---- the device ---- */

static id
device_init(id self, SEL _cmd, unsigned port)
{
	(void)_cmd;
	struct objc_super sup = { self, gDeviceBase };
	self = ((super0_t)objc_msgSendSuper)(&sup, S("init"));
	gPort = port;
	gDevice = self;
	say("device for accelerator port %u: %s", port, self != nil ? "made" : "_MTLDevice's init gave nil");
	if (self != nil) {
		mesa_open();
	}
	return self;
}

static id
device_name(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return (id)CFSTR("Mali-G76 MP12 (Panfrost, S10)");
}

static uint64_t
device_registry_id(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return gPort != 0 ? gPort : 1;
}

static uint64_t
device_max_buffer_length(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return 256ull << 20;
}

static unsigned long
device_max_texture_2d(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return mtl != NULL && mdev != NULL ? mtl->device_max_texture_2d(mdev) : 8192;
}

static unsigned long
device_alignment_for_format(id self, SEL _cmd, unsigned long format)
{
	(void)self;
	(void)_cmd;
	(void)format;
	return 64;                      /* the Mali's row alignment */
}

static unsigned long
device_constant_alignment(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return 256;
}

static unsigned long
device_max_fragment_samplers(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return 16;
}

static uint64_t
device_working_set(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return 1ull << 30;
}

static id
device_new_queue(id self, SEL _cmd)
{
	(void)_cmd;
	gDevice = self;
	if (gQueueClass == Nil) {
		say("newCommandQueue: no queue class, nil");
		return nil;
	}
	id queue = ((send0_t)objc_msgSend)(((send0_t)objc_msgSend)((id)gQueueClass, S("alloc")), S("init"));
	say("newCommandQueue: %s", queue != nil ? "made" : "_MTLCommandQueue's init gave nil");
	return queue;
}

static id
device_new_queue_max(id self, SEL _cmd, unsigned long count)
{
	(void)count;
	return device_new_queue(self, _cmd);
}

/* ---- the queue ---- */

static id
queue_label(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return (id)CFSTR("B2LMetalQueue");
}

static void
queue_ignore_number(id self, SEL _cmd, unsigned long value)
{
	(void)self;
	(void)_cmd;
	(void)value;
}

/*
 * -dealloc of the queue: NSObject's, not Metal's. The queue is made with
 * -init, not with Metal's initializer (-initWithDevice:), and
 * _MTLCommandQueue's -dealloc lets go of what that makes (HakiOS:
 * dispatch_release(NULL) faulted on its phone).
 */
static void
queue_dealloc(id self, SEL _cmd)
{
	struct objc_super sup = { self, objc_getClass("NSObject") };
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}

static void
register_queue_class(void)
{
	Class base = objc_getClass("_MTLCommandQueue");
	if (base == Nil) {
		say("this system has no _MTLCommandQueue: no queue class");
		return;
	}
	Class cls = objc_allocateClassPair(base, "B2LMetalQueue", 0);
	if (cls == Nil) {
		return;
	}
	remember_protocol(cls, objc_getProtocol("MTLCommandQueue"));
	declare_family(cls, "MTLCommandQueue");
	class_addMethod(cls, S("device"), (IMP)object_device, "@@:");
	class_addMethod(cls, S("label"), (IMP)queue_label, "@@:");
	class_addMethod(cls, S("setLabel:"), (IMP)object_nothing_id, "v@:@");
	class_addMethod(cls, S("setCompletionQueue:"), (IMP)object_nothing_id, "v@:@");
	class_addMethod(cls, S("setGPUPriority:"), (IMP)queue_ignore_number, "v@:Q");
	class_addMethod(cls, S("setBackgroundGPUPriority:"), (IMP)queue_ignore_number, "v@:Q");
	class_addMethod(cls, S("commandBuffer"), (IMP)queue_command_buffer, "@@:");
	class_addMethod(cls, S("commandBufferWithUnretainedReferences"), (IMP)queue_command_buffer, "@@:");
	class_addMethod(cls, S("commandBufferWithDescriptor:"), (IMP)queue_command_buffer_descriptor, "@@:@");
	class_addMethod(cls, S("insertDebugCaptureBoundary"), (IMP)object_nothing, "v@:");
	class_addMethod(cls, S("dealloc"), (IMP)queue_dealloc, "v@:");
	class_addMethod(cls, S("methodSignatureForSelector:"), (IMP)object_signature, "@@::");
	class_addMethod(cls, S("forwardInvocation:"), (IMP)object_forward, "v@:@");
	objc_registerClassPair(cls);
	gQueueClass = cls;
}

/* ---- the object classes ---- */

#define REGION "{?={?=QQQ}{?=QQQ}}"
#define ORIGIN "{?=QQQ}"
#define SIZE "{?=QQQ}"

static void
register_object_classes(Class device)
{
	static const char *const buffer[] = { "MTLBuffer", "MTLResource", "MTLAllocation", NULL };
	static const char *const texture[] = { "MTLTexture", "MTLResource", "MTLAllocation", NULL };
	static const char *const sampler[] = { "MTLSamplerState", NULL };
	static const char *const depth[] = { "MTLDepthStencilState", NULL };
	static const char *const library[] = { "MTLLibrary", NULL };
	static const char *const function[] = { "MTLFunction", NULL };
	static const char *const pipeline[] = { "MTLRenderPipelineState", NULL };
	static const char *const cmdbuf[] = { "MTLCommandBuffer", NULL };
	static const char *const render[] = { "MTLRenderCommandEncoder", "MTLCommandEncoder", NULL };
	static const char *const blit[] = { "MTLBlitCommandEncoder", "MTLCommandEncoder", NULL };
	static const char *const compute[] = { "MTLComputeCommandEncoder", "MTLCommandEncoder", NULL };
	Class c;

	if ((c = object_class("B2LMetalBuffer", buffer)) != Nil) {
		class_addMethod(c, S("dealloc"), (IMP)buffer_dealloc, "v@:");
		class_addMethod(c, S("contents"), (IMP)buffer_contents, "^v@:");
		class_addMethod(c, S("length"), (IMP)buffer_length, "Q@:");
		class_addMethod(c, S("resourceOptions"), (IMP)buffer_options, "Q@:");
		class_addMethod(c, S("storageMode"), (IMP)buffer_storage_mode, "Q@:");
		class_addMethod(c, S("cpuCacheMode"), (IMP)buffer_cache_mode, "Q@:");
		class_addMethod(c, S("hazardTrackingMode"), (IMP)buffer_hazard_mode, "Q@:");
		class_addMethod(c, S("allocatedSize"), (IMP)buffer_allocated_size, "Q@:");
		class_addMethod(c, S("didModifyRange:"), (IMP)buffer_did_modify, "v@:{_NSRange=QQ}");
		class_addMethod(c, S("gpuAddress"), (IMP)buffer_gpu_address, "Q@:");
		class_addMethod(c, S("setPurgeableState:"), (IMP)object_non_volatile, "Q@:Q");
		class_addMethod(c, S("isAliasable"), (IMP)object_no, "B@:");
		class_addMethod(c, S("makeAliasable"), (IMP)object_nothing, "v@:");
		class_addMethod(c, S("heap"), (IMP)object_nil, "@@:");
		class_addMethod(c, S("removeAllDebugMarkers"), (IMP)object_nothing, "v@:");
		objc_registerClassPair(c);
		gBufferClass = c;
		class_addMethod(device, S("newBufferWithLength:options:"), (IMP)device_new_buffer, "@@:QQ");
		class_addMethod(device, S("newBufferWithBytes:length:options:"), (IMP)device_new_buffer_bytes, "@@:r^vQQ");
		class_addMethod(device, S("newBufferWithBytesNoCopy:length:options:deallocator:"),
		    (IMP)device_new_buffer_nocopy, "@@:^vQQ@?");
	}
	if ((c = object_class("B2LMetalTexture", texture)) != Nil) {
		class_addMethod(c, S("dealloc"), (IMP)texture_dealloc, "v@:");
		class_addMethod(c, S("textureType"), (IMP)texture_type, "Q@:");
		class_addMethod(c, S("pixelFormat"), (IMP)texture_pixel_format, "Q@:");
		class_addMethod(c, S("width"), (IMP)texture_width, "Q@:");
		class_addMethod(c, S("height"), (IMP)texture_height, "Q@:");
		class_addMethod(c, S("depth"), (IMP)texture_depth, "Q@:");
		class_addMethod(c, S("mipmapLevelCount"), (IMP)texture_levels, "Q@:");
		class_addMethod(c, S("arrayLength"), (IMP)texture_array_length, "Q@:");
		class_addMethod(c, S("sampleCount"), (IMP)texture_sample_count, "Q@:");
		class_addMethod(c, S("usage"), (IMP)texture_usage, "Q@:");
		class_addMethod(c, S("storageMode"), (IMP)texture_storage_mode, "Q@:");
		class_addMethod(c, S("resourceOptions"), (IMP)texture_options, "Q@:");
		class_addMethod(c, S("cpuCacheMode"), (IMP)object_zero, "Q@:");
		class_addMethod(c, S("hazardTrackingMode"), (IMP)object_tracked, "Q@:");
		class_addMethod(c, S("allocatedSize"), (IMP)texture_allocated_size, "Q@:");
		class_addMethod(c, S("iosurface"), (IMP)texture_iosurface, "^{__IOSurface=}@:");
		class_addMethod(c, S("iosurfacePlane"), (IMP)texture_plane, "Q@:");
		class_addMethod(c, S("isFramebufferOnly"), (IMP)object_no, "B@:");
		class_addMethod(c, S("isShareable"), (IMP)object_no, "B@:");
		class_addMethod(c, S("isAliasable"), (IMP)object_no, "B@:");
		class_addMethod(c, S("makeAliasable"), (IMP)object_nothing, "v@:");
		class_addMethod(c, S("parentTexture"), (IMP)object_nil, "@@:");
		class_addMethod(c, S("buffer"), (IMP)object_nil, "@@:");
		class_addMethod(c, S("heap"), (IMP)object_nil, "@@:");
		class_addMethod(c, S("compressionType"), (IMP)object_zero, "q@:");
		class_addMethod(c, S("swizzle"), (IMP)texture_swizzle, "{?=CCCC}@:");
		class_addMethod(c, S("gpuResourceID"), (IMP)object_resource_id, "{?=Q}@:");
		class_addMethod(c, S("setPurgeableState:"), (IMP)object_non_volatile, "Q@:Q");
		class_addMethod(c, S("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"), (IMP)texture_replace,
		    "v@:" REGION "Qr^vQ");
		class_addMethod(c, S("replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:"),
		    (IMP)texture_replace_slice, "v@:" REGION "QQr^vQQ");
		class_addMethod(c, S("getBytes:bytesPerRow:fromRegion:mipmapLevel:"), (IMP)texture_get,
		    "v@:^vQ" REGION "Q");
		class_addMethod(c, S("getBytes:bytesPerRow:bytesPerImage:fromRegion:mipmapLevel:slice:"),
		    (IMP)texture_get_slice, "v@:^vQQ" REGION "QQ");
		objc_registerClassPair(c);
		gTextureClass = c;
		class_addMethod(device, S("newTextureWithDescriptor:"), (IMP)device_new_texture, "@@:@");
		class_addMethod(device, S("newTextureWithDescriptor:iosurface:plane:"), (IMP)device_new_texture_iosurface,
		    "@@:@^{__IOSurface=}Q");
	}
	if ((c = object_class("B2LMetalSamplerState", sampler)) != Nil) {
		class_addMethod(c, S("dealloc"), (IMP)sampler_dealloc, "v@:");
		class_addMethod(c, S("gpuResourceID"), (IMP)object_resource_id, "{?=Q}@:");
		objc_registerClassPair(c);
		gSamplerClass = c;
		class_addMethod(device, S("newSamplerStateWithDescriptor:"), (IMP)device_new_sampler, "@@:@");
	}
	if ((c = object_class("B2LMetalDepthStencilState", depth)) != Nil) {
		class_addMethod(c, S("dealloc"), (IMP)depth_stencil_dealloc, "v@:");
		objc_registerClassPair(c);
		gDepthStencilClass = c;
		class_addMethod(device, S("newDepthStencilStateWithDescriptor:"), (IMP)device_new_depth_stencil, "@@:@");
	}
	if ((c = object_class("B2LMetalFunction", function)) != Nil) {
		class_addMethod(c, S("dealloc"), (IMP)function_dealloc, "v@:");
		class_addMethod(c, S("name"), (IMP)function_name_of, "@@:");
		class_addMethod(c, S("functionType"), (IMP)function_type, "Q@:");
		class_addMethod(c, S("functionConstantsDictionary"), (IMP)function_empty_dictionary, "@@:");
		class_addMethod(c, S("vertexAttributes"), (IMP)function_empty_array, "@@:");
		class_addMethod(c, S("stageInputAttributes"), (IMP)function_empty_array, "@@:");
		class_addMethod(c, S("patchType"), (IMP)object_zero, "Q@:");
		class_addMethod(c, S("patchControlPointCount"), (IMP)object_zero, "q@:");
		class_addMethod(c, S("options"), (IMP)object_zero, "Q@:");
		objc_registerClassPair(c);
		gFunctionClass = c;
	}
	if ((c = object_class("B2LMetalLibrary", library)) != Nil) {
		class_addMethod(c, S("dealloc"), (IMP)library_dealloc, "v@:");
		class_addMethod(c, S("functionNames"), (IMP)library_function_names, "@@:");
		class_addMethod(c, S("newFunctionWithName:"), (IMP)library_new_function, "@@:@");
		class_addMethod(c, S("newFunctionWithName:constantValues:error:"), (IMP)library_new_function_constants,
		    "@@:@@^@");
		class_addMethod(c, S("newFunctionWithDescriptor:error:"), (IMP)library_new_function_descriptor, "@@:@^@");
		class_addMethod(c, S("type"), (IMP)object_zero, "q@:");        /* MTLLibraryTypeExecutable */
		class_addMethod(c, S("installName"), (IMP)object_nil, "@@:");
		objc_registerClassPair(c);
		gLibraryClass = c;
		class_addMethod(device, S("newLibraryWithURL:error:"), (IMP)device_new_library_url, "@@:@^@");
		class_addMethod(device, S("newLibraryWithFile:error:"), (IMP)device_new_library_file, "@@:@^@");
		class_addMethod(device, S("newLibraryWithData:error:"), (IMP)device_new_library_data, "@@:@^@");
		class_addMethod(device, S("newDefaultLibrary"), (IMP)device_new_default_library, "@@:");
		class_addMethod(device, S("newDefaultLibraryWithBundle:error:"), (IMP)device_new_default_library_bundle,
		    "@@:@^@");
	}
	if ((c = object_class("B2LMetalRenderPipelineState", pipeline)) != Nil) {
		class_addMethod(c, S("dealloc"), (IMP)pipeline_dealloc, "v@:");
		class_addMethod(c, S("supportIndirectCommandBuffers"), (IMP)object_no, "B@:");
		class_addMethod(c, S("threadgroupSizeMatchesTileSize"), (IMP)object_no, "B@:");
		class_addMethod(c, S("imageblockSampleLength"), (IMP)object_zero, "Q@:");
		class_addMethod(c, S("gpuResourceID"), (IMP)object_resource_id, "{?=Q}@:");
		objc_registerClassPair(c);
		gRenderPipelineClass = c;
		class_addMethod(device, S("newRenderPipelineStateWithDescriptor:error:"), (IMP)device_new_render_pipeline,
		    "@@:@^@");
		class_addMethod(device, S("newRenderPipelineStateWithDescriptor:options:reflection:error:"),
		    (IMP)device_new_render_pipeline_options, "@@:@Q^@^@");
	}
	if ((c = object_class("B2LMetalRenderEncoder", render)) != Nil) {
		class_addMethod(c, S("dealloc"), (IMP)encoder_dealloc, "v@:");
		class_addMethod(c, S("endEncoding"), (IMP)render_end, "v@:");
		class_addMethod(c, S("pushDebugGroup:"), (IMP)encoder_debug_group, "v@:@");
		class_addMethod(c, S("popDebugGroup"), (IMP)object_nothing, "v@:");
		class_addMethod(c, S("insertDebugSignpost:"), (IMP)encoder_debug_group, "v@:@");
		class_addMethod(c, S("drawPrimitives:vertexStart:vertexCount:"), (IMP)render_draw3, "v@:QQQ");
		class_addMethod(c, S("drawPrimitives:vertexStart:vertexCount:instanceCount:"), (IMP)render_draw4, "v@:QQQQ");
		class_addMethod(c, S("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:"),
		    (IMP)render_draw_indexed, "v@:QQQ@Q");
		class_addMethod(c,
		    S("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:"),
		    (IMP)render_draw_indexed_instanced, "v@:QQQ@QQ");
		objc_registerClassPair(c);
		gRenderEncoderClass = c;
	}
	if ((c = object_class("B2LMetalBlitEncoder", blit)) != Nil) {
		class_addMethod(c, S("dealloc"), (IMP)encoder_dealloc, "v@:");
		class_addMethod(c, S("endEncoding"), (IMP)encoder_end, "v@:");
		class_addMethod(c, S("pushDebugGroup:"), (IMP)encoder_debug_group, "v@:@");
		class_addMethod(c, S("popDebugGroup"), (IMP)object_nothing, "v@:");
		class_addMethod(c, S("insertDebugSignpost:"), (IMP)encoder_debug_group, "v@:@");
		class_addMethod(c, S("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:"),
		    (IMP)blit_copy_buffer, "v@:@Q@QQ");
		class_addMethod(c, S("fillBuffer:range:value:"), (IMP)blit_fill, "v@:@{_NSRange=QQ}C");
		class_addMethod(c,
		    S("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:"
		      "destinationLevel:destinationOrigin:"),
		    (IMP)blit_copy_texture, "v@:@QQ" ORIGIN SIZE "@QQ" ORIGIN);
		class_addMethod(c, S("copyFromTexture:toTexture:"), (IMP)blit_copy_texture_whole, "v@:@@");
		class_addMethod(c,
		    S("copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:"
		      "destinationSlice:destinationLevel:destinationOrigin:"),
		    (IMP)blit_buffer_to_texture, "v@:@QQQ" SIZE "@QQ" ORIGIN);
		class_addMethod(c,
		    S("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:"
		      "destinationBytesPerRow:destinationBytesPerImage:"),
		    (IMP)blit_texture_to_buffer, "v@:@QQ" ORIGIN SIZE "@QQQ");
		class_addMethod(c, S("generateMipmapsForTexture:"), (IMP)blit_mipmaps, "v@:@");
		class_addMethod(c, S("synchronizeResource:"), (IMP)object_nothing_id, "v@:@");
		class_addMethod(c, S("synchronizeTexture:slice:level:"), (IMP)blit_synchronize_texture, "v@:@QQ");
		class_addMethod(c, S("optimizeContentsForGPUAccess:"), (IMP)object_nothing_id, "v@:@");
		objc_registerClassPair(c);
		gBlitEncoderClass = c;
	}
	if ((c = object_class("B2LMetalComputeEncoder", compute)) != Nil) {
		class_addMethod(c, S("dealloc"), (IMP)encoder_dealloc, "v@:");
		class_addMethod(c, S("endEncoding"), (IMP)encoder_end, "v@:");
		class_addMethod(c, S("pushDebugGroup:"), (IMP)encoder_debug_group, "v@:@");
		class_addMethod(c, S("popDebugGroup"), (IMP)object_nothing, "v@:");
		objc_registerClassPair(c);
		gComputeEncoderClass = c;
	}
	if ((c = object_class("B2LMetalCommandBuffer", cmdbuf)) != Nil) {
		class_addMethod(c, S("dealloc"), (IMP)cmdbuf_dealloc, "v@:");
		class_addMethod(c, S("commit"), (IMP)cmdbuf_commit, "v@:");
		class_addMethod(c, S("commitAndWaitUntilSubmitted"), (IMP)cmdbuf_commit, "v@:");
		class_addMethod(c, S("enqueue"), (IMP)cmdbuf_enqueue, "v@:");
		class_addMethod(c, S("waitUntilCompleted"), (IMP)cmdbuf_wait, "v@:");
		class_addMethod(c, S("waitUntilScheduled"), (IMP)cmdbuf_wait, "v@:");
		class_addMethod(c, S("addCompletedHandler:"), (IMP)cmdbuf_add_completed, "v@:@?");
		class_addMethod(c, S("addScheduledHandler:"), (IMP)cmdbuf_add_scheduled, "v@:@?");
		class_addMethod(c, S("status"), (IMP)cmdbuf_status, "Q@:");
		class_addMethod(c, S("error"), (IMP)cmdbuf_error, "@@:");
		class_addMethod(c, S("commandQueue"), (IMP)cmdbuf_queue, "@@:");
		class_addMethod(c, S("retainedReferences"), (IMP)object_yes, "B@:");
		class_addMethod(c, S("GPUStartTime"), (IMP)cmdbuf_gpu_start, "d@:");
		class_addMethod(c, S("GPUEndTime"), (IMP)cmdbuf_gpu_end, "d@:");
		class_addMethod(c, S("kernelStartTime"), (IMP)cmdbuf_gpu_start, "d@:");
		class_addMethod(c, S("kernelEndTime"), (IMP)cmdbuf_gpu_end, "d@:");
		class_addMethod(c, S("pushDebugGroup:"), (IMP)encoder_debug_group, "v@:@");
		class_addMethod(c, S("popDebugGroup"), (IMP)object_nothing, "v@:");
		class_addMethod(c, S("renderCommandEncoderWithDescriptor:"), (IMP)cmdbuf_render_encoder, "@@:@");
		class_addMethod(c, S("blitCommandEncoder"), (IMP)cmdbuf_blit_encoder, "@@:");
		class_addMethod(c, S("blitCommandEncoderWithDescriptor:"), (IMP)cmdbuf_blit_encoder_descriptor, "@@:@");
		class_addMethod(c, S("computeCommandEncoder"), (IMP)cmdbuf_compute_encoder, "@@:");
		class_addMethod(c, S("computeCommandEncoderWithDispatchType:"), (IMP)cmdbuf_compute_encoder_type, "@@:Q");
		objc_registerClassPair(c);
		gCommandBufferClass = c;
	}
	class_addMethod(device, S("supportsMemorylessRenderTargets"), (IMP)object_no, "B@:");
	class_addMethod(device, S("hasUnifiedMemory"), (IMP)object_yes, "B@:");
	class_addMethod(device, S("isLowPower"), (IMP)object_no, "B@:");
	class_addMethod(device, S("isHeadless"), (IMP)object_no, "B@:");
	class_addMethod(device, S("isRemovable"), (IMP)object_no, "B@:");
	class_addMethod(device, S("maxTextureWidth2D"), (IMP)device_max_texture_2d, "Q@:");
	class_addMethod(device, S("maxTextureHeight2D"), (IMP)device_max_texture_2d, "Q@:");
	class_addMethod(device, S("minimumLinearTextureAlignmentForPixelFormat:"), (IMP)device_alignment_for_format,
	    "Q@:Q");
	class_addMethod(device, S("minimumTextureBufferAlignmentForPixelFormat:"), (IMP)device_alignment_for_format,
	    "Q@:Q");
	class_addMethod(device, S("minConstantBufferAlignmentBytes"), (IMP)device_constant_alignment, "Q@:");
	class_addMethod(device, S("maxFragmentSamplers"), (IMP)device_max_fragment_samplers, "Q@:");
	class_addMethod(device, S("recommendedMaxWorkingSetSize"), (IMP)device_working_set, "Q@:");
}

/* ---- loading ---- */

__attribute__((constructor)) static void
b2l_metal_load(void)
{
	int level = metal_level();
	if (!shown_to_this_process(level)) {
		say("not shown the device (b2l_metal=%d: 1 is the B2L_MTL_TEST process only, 2 everyone)", level);
		return;
	}
	gDeviceBase = objc_getClass("_MTLDevice");
	if (gDeviceBase == Nil) {
		say("this system has no _MTLDevice: no device class");
		return;
	}
	if (objc_getClass("B2LMetalDevice") != Nil) {
		return;
	}
	Class cls = objc_allocateClassPair(gDeviceBase, "B2LMetalDevice", 0);
	if (cls == Nil) {
		say("could not make the class B2LMetalDevice");
		return;
	}
	/* MTLAddDevice asserts that the device conforms to MTLDevice and to the
	 * private MTLDeviceSPI (boot 67: it does, and the device was added). */
	remember_protocol(cls, objc_getProtocol("MTLDevice"));
	Protocol *spi = objc_getProtocol("MTLDeviceSPI");
	if (spi == NULL) {
		say("this Metal has no protocol MTLDeviceSPI");
	}
	remember_protocol(cls, spi);
	class_addMethod(cls, S("initWithAcceleratorPort:"), (IMP)device_init, "@@:I");
	class_addMethod(cls, S("name"), (IMP)device_name, "@@:");
	class_addMethod(cls, S("registryID"), (IMP)device_registry_id, "Q@:");
	class_addMethod(cls, S("maxBufferLength"), (IMP)device_max_buffer_length, "Q@:");
	class_addMethod(cls, S("newCommandQueue"), (IMP)device_new_queue, "@@:");
	class_addMethod(cls, S("newCommandQueueWithMaxCommandBufferCount:"), (IMP)device_new_queue_max, "@@:Q");
	class_addMethod(cls, S("methodSignatureForSelector:"), (IMP)object_signature, "@@::");
	class_addMethod(cls, S("forwardInvocation:"), (IMP)object_forward, "v@:@");
	/* the object classes and the device's methods for them, before the
	 * device class is registered */
	register_object_classes(cls);
	objc_registerClassPair(cls);
	gDeviceClass = cls;
	register_queue_class();
	say("B2LMetalDevice registered on _MTLDevice (b2l_metal=%d), %u protocols", level, gProtocolCount);
}
