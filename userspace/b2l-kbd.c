// SPDX-License-Identifier: BSD-2-Clause
/*
 * S10: keyboard relief, loaded into MobileNotes only (the kernel adds it with
 * DYLD_INSERT_LIBRARIES, bsd/kern/b2l_exec_gpu_ui.h).
 *
 * Boots 35-37: Notes' main thread spent its time in one chain.
 * AFNetworkAvailability reports every network-state change to the dictation
 * connection, which forwards each one unfiltered
 * (-[AFDictationConnection networkAvailability:isAvailable:] ->
 * _availabilityChanged -> _tellSpeechDelegateAvailabilityChanged). From there
 * -[UIDictationController dictationConnnectionDidChangeAvailability:] makes
 * the keyboard dock rebuild its glyphs, and UIKit looks those glyphs up by
 * scanning the bundle on disk. Typing lagged by seconds. Neither the language
 * list nor turning dictation off stopped it.
 *
 * This lets one such notice through at most every 2 s; the last one of an
 * interval is delivered at its end, so the dock still follows a real change.
 * How many arrive is logged ("dev.b2l.kbd") every 30 s.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <objc/runtime.h>
#include <os/log.h>
#include <stdint.h>

#define B2L_KBD_INTERVAL_NS 2000000000ull

typedef void (*notice_fn)(id self, SEL cmd, id connection);
static notice_fn original;
static uint64_t last_ns, window_ns;
static unsigned received, forwarded;
static int pending;
static os_log_t log_handle;

static uint64_t
now_ns(void)
{
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom;
}

static void
deliver(id self, SEL cmd, id connection)
{
	last_ns = now_ns();
	forwarded++;
	original(self, cmd, connection);
}

/* Runs on the main queue, where UIKit delivers the notice. */
static void
throttled(id self, SEL cmd, id connection)
{
	uint64_t now = now_ns();
	received++;
	if (now - window_ns >= 30000000000ull) {
		os_log(log_handle, "dictation availability notices: %u received, %u forwarded in 30 s", received, forwarded);
		received = forwarded = 0;
		window_ns = now;
	}
	if (now - last_ns >= B2L_KBD_INTERVAL_NS) {
		deliver(self, cmd, connection);
		return;
	}
	if (pending) {
		return;
	}
	pending = 1;
	CFRetain((CFTypeRef)self);
	if (connection != NULL) {
		CFRetain((CFTypeRef)connection);
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(B2L_KBD_INTERVAL_NS - (now - last_ns))),
	    dispatch_get_main_queue(), ^{
		pending = 0;
		deliver(self, cmd, connection);
		if (connection != NULL) {
			CFRelease((CFTypeRef)connection);
		}
		CFRelease((CFTypeRef)self);
	});
}

static void
install(void)
{
	Class controller = objc_getClass("UIDictationController");
	SEL name = sel_registerName("dictationConnnectionDidChangeAvailability:");
	Method method = controller != NULL ? class_getInstanceMethod(controller, name) : NULL;
	if (method == NULL) {
		os_log(log_handle, "UIDictationController notice method not found; nothing changed");
		return;
	}
	original = (notice_fn)method_getImplementation(method);
	method_setImplementation(method, (IMP)throttled);
	window_ns = now_ns();
	os_log(log_handle, "dictation availability notices limited to one per 2 s");
}

__attribute__((constructor)) static void
b2l_kbd_start(void)
{
	log_handle = os_log_create("dev.b2l.kbd", "keyboard");
	/* UIKit is up once the main queue runs (UIApplicationMain). */
	dispatch_async(dispatch_get_main_queue(), ^{
		install();
	});
}
