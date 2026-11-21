// SPDX-License-Identifier: MIT
/*
 * S10 port: B2LMetal.bundle, Metal milestone M1 (docs/metal-on-panfrost.md).
 *
 * What Metal does on iOS 26 (read from the 23G90 cache): it matches the
 * IOAcceleratorES services, reads MetalPluginName ("B2LMetal-<hash>", this
 * bundle under /System/Library/Extensions) and MetalPluginClassName
 * ("B2LMetalDevice"), takes that class if it is a subclass of its private
 * _MTLDevice, and sends it -initWithAcceleratorPort:. B2LMali makes that
 * IOAcceleratorES with boot argument b2l_metal=N; the kernel publishes this
 * bundle (b2l_userspace_seed.h).
 *
 * The approach -- the device class made at run time on the system's own
 * _MTLDevice, MTLDevice and the private MTLDeviceSPI declared (MTLAddDevice
 * asserts both), every message the class has no method for answered with a
 * zero of the protocol's own return type and logged once -- is HakiOS's
 * (github.com/oosexgang/hakios-metal, plugin/HakiosMetal.m, MIT). This file is
 * written for the S10 port in C on the Objective-C runtime, logs to the kernel
 * log through B2LMali ("B2L GPU UI: mtl M1: ..."), and draws nothing: it is
 * the list of what Metal and Core Animation ask first.
 *
 * Who is shown the device: boot argument b2l_metal=1 shows it only to a
 * process started with B2L_MTL_TEST in its environment (userspace/b2l-gpu.c
 * starts one); b2l_metal=2 (M3) shows it to everyone, backboardd included. A
 * process not shown it gets no class registered, so Metal finds no device and
 * Core Animation stays on its OpenGL ES path.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
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

typedef mach_port_t io_object_t, io_service_t, io_connect_t;
extern const mach_port_t kIOMainPortDefault;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t port, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallMethod(io_connect_t c, uint32_t selector, const uint64_t *in, uint32_t in_count,
    const void *struct_in, size_t struct_in_size, uint64_t *out, uint32_t *out_count, void *struct_out,
    size_t *struct_out_size);

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
	int head = snprintf(text, sizeof(text), "mtl M1: [%d %s] ", getpid(), getprogname());
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

/* ---- objects ---- */

static Class gDeviceBase, gDeviceClass, gQueueClass;
static id gDevice;
static unsigned gPort;
static Protocol *gProtocols[64];
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
				    sel_registerName("signatureWithObjCTypes:"), d.types);
			}
		}
	}
	struct objc_super sup = { self, class_getSuperclass(object_getClass(self)) };
	return ((id (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper)(&sup,
	    sel_registerName("methodSignatureForSelector:"), selector);
}

#define SEEN_MAX 512
static SEL seen[SEEN_MAX];
static unsigned seen_count;
static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;

static void
object_forward(id self, SEL _cmd, id invocation)
{
	(void)_cmd;
	SEL selector = ((SEL (*)(id, SEL))objc_msgSend)(invocation, sel_registerName("selector"));
	id signature = ((id (*)(id, SEL))objc_msgSend)(invocation, sel_registerName("methodSignature"));
	unsigned long length = ((unsigned long (*)(id, SEL))objc_msgSend)(signature, sel_registerName("methodReturnLength"));
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
	if (first) {
		const char *type = ((const char *(*)(id, SEL))objc_msgSend)(signature, sel_registerName("methodReturnType"));
		say("%s asked -%s, not implemented: zero (%lu bytes, %s)", class_getName(object_getClass(self)),
		    sel_getName(selector), length, type != NULL ? type : "?");
	}
	if (length != 0) {
		void *zero = calloc(1, length);
		if (zero != NULL) {
			((void (*)(id, SEL, void *))objc_msgSend)(invocation, sel_registerName("setReturnValue:"), zero);
			free(zero);
		}
	}
}

/* ---- the device ---- */

static id
device_init(id self, SEL _cmd, unsigned port)
{
	(void)_cmd;
	struct objc_super sup = { self, gDeviceBase };
	self = ((super0_t)objc_msgSendSuper)(&sup, sel_registerName("init"));
	gPort = port;
	gDevice = self;
	say("device for accelerator port %u: %s", port, self != nil ? "made" : "_MTLDevice's init gave nil");
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

/* What the GPU's memory could hold, not a promise: nothing allocates yet. */
static uint64_t
device_max_buffer_length(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return 256ull << 20;
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
	id queue = ((send0_t)objc_msgSend)(((send0_t)objc_msgSend)((id)gQueueClass, sel_registerName("alloc")),
	    sel_registerName("init"));
	say("newCommandQueue: %s", queue != nil ? "a queue that queues nothing (M2)" : "_MTLCommandQueue's init gave nil");
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
queue_device(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return gDevice;
}

static id
queue_label(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return (id)CFSTR("B2LMetalQueue");
}

static void
queue_ignore(id self, SEL _cmd, id value)
{
	(void)self;
	(void)_cmd;
	(void)value;
}

static void
queue_ignore_number(id self, SEL _cmd, unsigned long value)
{
	(void)self;
	(void)_cmd;
	(void)value;
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
	class_addMethod(cls, sel_registerName("device"), (IMP)queue_device, "@@:");
	class_addMethod(cls, sel_registerName("label"), (IMP)queue_label, "@@:");
	class_addMethod(cls, sel_registerName("setLabel:"), (IMP)queue_ignore, "v@:@");
	class_addMethod(cls, sel_registerName("setCompletionQueue:"), (IMP)queue_ignore, "v@:@");
	class_addMethod(cls, sel_registerName("setGPUPriority:"), (IMP)queue_ignore_number, "v@:Q");
	class_addMethod(cls, sel_registerName("setBackgroundGPUPriority:"), (IMP)queue_ignore_number, "v@:Q");
	class_addMethod(cls, sel_registerName("methodSignatureForSelector:"), (IMP)object_signature, "@@::");
	class_addMethod(cls, sel_registerName("forwardInvocation:"), (IMP)object_forward, "v@:@");
	objc_registerClassPair(cls);
	gQueueClass = cls;
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
	 * private MTLDeviceSPI (HakiOS's runs on iOS 16; to be confirmed here). */
	remember_protocol(cls, objc_getProtocol("MTLDevice"));
	Protocol *spi = objc_getProtocol("MTLDeviceSPI");
	if (spi == NULL) {
		say("this Metal has no protocol MTLDeviceSPI");
	}
	remember_protocol(cls, spi);
	class_addMethod(cls, sel_registerName("initWithAcceleratorPort:"), (IMP)device_init, "@@:I");
	class_addMethod(cls, sel_registerName("name"), (IMP)device_name, "@@:");
	class_addMethod(cls, sel_registerName("registryID"), (IMP)device_registry_id, "Q@:");
	class_addMethod(cls, sel_registerName("maxBufferLength"), (IMP)device_max_buffer_length, "Q@:");
	class_addMethod(cls, sel_registerName("newCommandQueue"), (IMP)device_new_queue, "@@:");
	class_addMethod(cls, sel_registerName("newCommandQueueWithMaxCommandBufferCount:"), (IMP)device_new_queue_max,
	    "@@:Q");
	class_addMethod(cls, sel_registerName("methodSignatureForSelector:"), (IMP)object_signature, "@@::");
	class_addMethod(cls, sel_registerName("forwardInvocation:"), (IMP)object_forward, "v@:@");
	objc_registerClassPair(cls);
	gDeviceClass = cls;
	register_queue_class();
	say("B2LMetalDevice registered on _MTLDevice (b2l_metal=%d), %u protocols", level, gProtocolCount);
}
