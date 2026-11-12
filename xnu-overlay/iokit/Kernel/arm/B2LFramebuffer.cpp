/*
 * CPU scanout bridge for the S10+ firmware-provided framebuffer. Physical
 * range, width, height and stride come from the loader/PE console, not A5.
 * AppleCLCD matches the samsung,beyond2lte-display arm-io nub.
 *
 * iOS 26.6.2 IOMobileFramebuffer client contract inspected locally:
 *   3 default IOSurface port; 4 swap id; 5 swap record (0x588 bytes)
 *   record +0x98 swap id, +0x9c surface IDs, +0xac source rects,
 *   +0x10c destination rects, +0x14c updated-layer mask, +0x150 disabled-layer mask (four layers).
 *   6 wait; 7 display id; 8 dimensions; 18 main; 28 dot pitch;
 *   72 notification control. Unsupported selectors return Unsupported.
 *
 * BGRA layers are copied without resizing, inside SwapEnd: each layer is
 * compared with a cached shadow of the panel and only the changed span of
 * each row, widened to 64-byte boundaries, is written to the uncached panel
 * memory. A 60 Hz software timer supplies
 * notification type 5, not hardware scanout synchronization. After the first
 * accepted copy only console drawing is suppressed; persistent logging and
 * panic drawing remain active. GPU acceleration is not provided.
 */
#include "B2LSurface.h"
#include "B2LBlit.h"
#include "b2l_decon_dump.h"
extern "C" void b2l_hid_set_display_inset(uint32_t inset_rows, uint32_t panel_rows);
extern "C" void flush_dcache64(addr64_t addr, unsigned count, int phys);

#if defined(B2L)
#include <IOKit/IOService.h>
#include <arm64/b2l_diag.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/system.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <sys/proc.h>
#include <arm64/b2l_early.h>

enum {
	kB2LFBGetLayerDefaultSurface  = 3,
	kB2LFBSwapBegin               = 4,
	kB2LFBSwapEnd                 = 5,
	kB2LFBSwapWait                = 6,
	kB2LFBGetID                   = 7,
	kB2LFBGetDisplaySize          = 8,
	kB2LFBIsMainDisplay           = 18,
	kB2LFBGetDotPitch             = 28,
	kB2LFBNotifications           = 72,
	kB2LFBFrameInfoDescriptor     = 74,
	kB2LFBSetContrast             = 84,
	kB2LFBSelectDisplay           = 88,
};

#define B2L_FB_SWAP_REC_SIZE          0x588
#define B2L_FB_LAYERS                 4
#define B2L_FB_NOTIFY_TYPES           8
#define B2L_FB_NOTIFY_VSYNC           5
#define B2L_FB_VSYNC_US               16667
#define B2L_FB_DOT_PITCH              522     /* S10+ panel; geometry still comes from GOP */
#define B2L_FB_FORMAT_BGRA            0x42475241
/* Rows kept black above iOS for the S10+ front-camera hole (top right). 136
 * leaves 1440x2904, exactly 480x968 points at 3x, and keeps every row start
 * 64-byte aligned. Boot argument b2l_fb_top_inset overrides it. */
#define B2L_FB_TOP_INSET              136

class IOMobileFramebuffer : public IOService {
	OSDeclareDefaultStructors(IOMobileFramebuffer);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

class IOMobileFramebufferUserClient;

class AppleCLCD : public IOMobileFramebuffer {
	OSDeclareDefaultStructors(AppleCLCD);

	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fVsync;
	uint64_t            fVsyncCount;

	static void vsyncFired(OSObject *owner, IOTimerEventSource *sender);
	static void deconDumpMain(void *owner, wait_result_t result);
	void deconDump(void);
	/* b2l_fb_probe=1: a panel-sized buffer that is never presented. Its
	 * physical runs are logged, per-page hashes are checked every 5 s to
	 * catch writes from outside the kernel, and at 60 s uptime it is filled
	 * with a pattern, as the dirty copy's first fill would. */
	uint8_t      *fProbe;
	uint64_t     *fProbeHash;
	uint32_t      fProbePages, fProbeReports;
	bool          fProbeFilled;
	void probeStart(void);
	void probeTick(void);

public:
	uint64_t      fPhys;
	uint32_t      fWidth, fHeight, fRowBytes;
	uintptr_t     fPixels;          /* kernel mapping of the presented area */
	uintptr_t     fPanelPixels;     /* kernel mapping of the whole panel */
	uint32_t      fTopInset;        /* black rows above the presented area */
	IOMemoryMap  *fMap;
	B2LSurface *fSurface;         /* the panel memory as an IOSurface */
	IOLock       *fLock;            /* swaps and client list */
	OSArray      *fClients;
	uint32_t      fSwapID;
	bool          fConsolePaused;
	/* Present-path counters, reported every 10 s for the first 60 reports;
	 * guarded by fLock. */
	uint32_t      fPerfSwaps, fPerfCopies, fPerfFull, fPerfReports, fPerfWaits, fCopyLogs;
	uint64_t      fPerfPixels, fPerfTicks, fPerfMaxTicks, fPerfWritten, fPerfWaitTicks;
	/* One queued presentation (guarded by fLock) and the copy worker. */
	struct Layer { B2LSurface *s; int32_t sx, sy, dx, dy, w, h; };
	struct Job { uint32_t count; Layer layer[B2L_FB_LAYERS]; };
	Job           fJob;
	bool          fBusy;            /* a job is queued or being copied */
	thread_call_t fCopyCall;
	uint8_t      *fShadow;          /* cached copy of what the panel shows */
	bool          fShadowValid;     /* set once a copy covered the whole panel */

	/* Scanout straight from a surface: DECON_F window 5, DMA channel 0, as the
	 * boot chain set them up for the panel memory (0147 dump). */
	IOMemoryMap  *fDeconMap, *fDmaMap;
	volatile uint32_t *fDecon, *fDma;
	bool          fScanout;         /* b2l_scanout, default on */
	bool          fFlipped;         /* the channel points at a surface, not the panel */
	B2LSurface   *fShown[2];        /* the surface on screen and the one before it */
	uint32_t      fPerfFlips, fPerfFallbacks;
	uint64_t      fPerfFlipTicks, fPerfCleanTicks;
	bool flipTo(B2LSurface *s, const Layer *l);
	void pointAtPanel(void);
	void programChannel(uint32_t base, uint32_t stride_px, uint32_t w, uint32_t h, uint32_t top);

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	void swap(const uint8_t *rec);
	void waitIdle(void);
	void runCopy(void);
	static void copyWorker(thread_call_param_t owner, thread_call_param_t unused);
};

OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

class IOMobileFramebufferUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	AppleCLCD          *fFB;
	task_t              fTask;
	mach_port_t         fPort;
	io_user_reference_t fFunc[B2L_FB_NOTIFY_TYPES];
	io_user_reference_t fRefcon[B2L_FB_NOTIFY_TYPES];

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
	    io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	void notify(uint32_t type, const uint64_t *values, uint32_t count);
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
AppleCLCD::start(IOService *provider)
{
	PE_Video video;

	if (!IOService::start(provider)) {
		return false;
	}
	PE_current_console(&video);
	uint64_t mapped_bytes=0;
    if(!b2l_early_range(&fPhys,&mapped_bytes))return false;
	fWidth = (uint32_t)video.v_width;
	fHeight = (uint32_t)video.v_height;
	fRowBytes = (uint32_t)video.v_rowBytes;
	if (fPhys == 0 || fWidth == 0 || fHeight == 0 || fWidth>8192 || fHeight>8192 || fRowBytes < fWidth * 4 || (uint64_t)fRowBytes*fHeight>mapped_bytes) {
		IOLog("B2L FB: no boot framebuffer\n");
		return false;
	}
	fLock = IOLockAlloc();
	fClients = OSArray::withCapacity(4);
	fSurface = b2l_surface_create_physical(fPhys, fWidth, fHeight, fRowBytes, B2L_FB_FORMAT_BGRA);
	fPanelPixels = fSurface != NULL ? fSurface->kernelAddress() : 0;
	fWorkLoop = IOWorkLoop::workLoop();
	if (fLock == NULL || fClients == NULL || fPanelPixels == 0 || fWorkLoop == NULL) {
		IOLog("B2L FB: setup failed\n");
		OSSafeReleaseNULL(fSurface);    /* stays published: also held by the table */
		OSSafeReleaseNULL(fWorkLoop);
		return false;
	}
	/* From here on fPixels/fHeight describe only the area below the camera
	 * band: GetDisplaySize, copies, clipping and the shadow all use it. The
	 * default surface stays the whole physical panel and is never copied. */
	uint32_t inset = B2L_FB_TOP_INSET;
	PE_parse_boot_argn("b2l_fb_top_inset", &inset, sizeof(inset));
	if (inset > fHeight / 4) inset = 0;
	fTopInset = inset;
	fPixels = fPanelPixels + (uintptr_t)fRowBytes * inset;
	fHeight -= inset;
	b2l_hid_set_display_inset(inset, fHeight + inset);
	IOLog("B2L FB: top inset %u rows, presenting %ux%u\n", inset, fWidth, fHeight);
	/* Without a shadow every copy is a full write; without the thread call it
	 * runs in SwapEnd as before. Both are allowed to fail. */
	/* Dirty-span copy (64-byte aligned spans, see B2LBlit.h); boot argument
	 * b2l_fb_dirty=0 restores full-frame copies. */
	uint32_t dirty = 1;
	PE_parse_boot_argn("b2l_fb_dirty", &dirty, sizeof(dirty));
	fShadow = dirty ? (uint8_t *)IOMallocData((vm_size_t)fRowBytes * fHeight) : NULL;
	uint32_t probe = 0;
	PE_parse_boot_argn("b2l_fb_probe", &probe, sizeof(probe));
	if (probe) probeStart();
	/* 0109 copied on a worker thread and SwapEnd returned before the copy;
	 * backboardd then presented almost nothing and the screen stayed dark.
	 * Until that contract is understood, copy inside SwapEnd (the worker
	 * path stays, unused, behind fCopyCall). */
	fCopyCall = NULL;
	IOLog("B2L FB: present shadow %s, copy worker %s\n", fShadow ? "on" : "off", fCopyCall ? "on" : "off");
	IOMemoryDescriptor *regs = IOMemoryDescriptor::withPhysicalAddress(B2L_DECON_BASE, B2L_DECON_BYTES, kIODirectionInOut);
	fDeconMap = regs ? regs->map(kIOMapInhibitCache) : NULL;
	OSSafeReleaseNULL(regs);
	regs = IOMemoryDescriptor::withPhysicalAddress(B2L_DPU_DMA_BASE, B2L_DPU_DMA_BYTES, kIODirectionInOut);
	fDmaMap = regs ? regs->map(kIOMapInhibitCache) : NULL;
	OSSafeReleaseNULL(regs);
	fDecon = fDeconMap ? (volatile uint32_t *)fDeconMap->getVirtualAddress() : NULL;
	fDma = fDmaMap ? (volatile uint32_t *)fDmaMap->getVirtualAddress() : NULL;
	int scanout = 1;
	PE_parse_boot_argn("b2l_scanout", &scanout, sizeof(scanout));
	fScanout = scanout && fDecon != NULL && fDma != NULL;
	IOLog("B2L FB: scanout from surfaces %s\n", fScanout ? "on" : "off");
	int dump = 1;
	PE_parse_boot_argn("b2l_decon_dump", &dump, sizeof(dump));
	if (dump) {
		thread_t thread;
		retain();
		if (kernel_thread_start(&AppleCLCD::deconDumpMain, this, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			release();
		}
	}
	fVsync = IOTimerEventSource::timerEventSource(this, &AppleCLCD::vsyncFired);
	if (fVsync == NULL || fWorkLoop->addEventSource(fVsync) != kIOReturnSuccess) {
		IOLog("B2L FB: no vsync timer\n");
		return false;
	}
	fVsync->setTimeoutUS(B2L_FB_VSYNC_US);

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	setProperty("external", false);
	setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32);
	registerService();
	IOLog("B2L FB: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u\n",
	    fWidth, fHeight, fRowBytes, fPhys, fSurface->id);
	return true;
}

void
AppleCLCD::vsyncFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	uint64_t now = mach_absolute_time();
	uint64_t count = ++fb->fVsyncCount;
	uint64_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32),
		(uint32_t)now, (uint32_t)(now >> 32),
		0, 0,
	};

	if (fb->fProbe != NULL && count % 300 == 0) fb->probeTick();
	IOLockLock(fb->fLock);
	if (count % 600 == 0 && fb->fPerfReports < 60) {
		uint64_t total_ns, max_ns;
		absolutetime_to_nanoseconds(fb->fPerfTicks, &total_ns);
		absolutetime_to_nanoseconds(fb->fPerfMaxTicks, &max_ns);
		fb->fPerfReports++;
		uint64_t wait_ns;
		absolutetime_to_nanoseconds(fb->fPerfWaitTicks, &wait_ns);
		uint64_t flip_ns = 0, clean_ns = 0;
		absolutetime_to_nanoseconds(fb->fPerfFlipTicks, &flip_ns);
		absolutetime_to_nanoseconds(fb->fPerfCleanTicks, &clean_ns);
		IOLog("B2L FB PERF: vsync=%llu swaps=%u copies=%u full=%u kpix=%llu written_kb=%llu copy_ms=%llu max_copy_us=%llu waits=%u wait_ms=%llu flips=%u flip_ms=%llu clean_ms=%llu fallbacks=%u\n",
		    count, fb->fPerfSwaps, fb->fPerfCopies, fb->fPerfFull, fb->fPerfPixels / 1000,
		    fb->fPerfWritten / 1024, total_ns / 1000000, max_ns / 1000, fb->fPerfWaits, wait_ns / 1000000,
		    fb->fPerfFlips, flip_ns / 1000000, clean_ns / 1000000, fb->fPerfFallbacks);
		fb->fPerfFlips = fb->fPerfFallbacks = 0;
		fb->fPerfFlipTicks = fb->fPerfCleanTicks = 0;
		fb->fPerfSwaps = fb->fPerfCopies = fb->fPerfFull = fb->fPerfWaits = 0;
		fb->fPerfPixels = fb->fPerfTicks = fb->fPerfMaxTicks = fb->fPerfWritten = fb->fPerfWaitTicks = 0;
	}
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fb->fClients->getObject(i);
		client->notify(B2L_FB_NOTIFY_VSYNC, values, 6);
	}
	IOLockUnlock(fb->fLock);
	sender->setTimeoutUS(B2L_FB_VSYNC_US);
}

void
AppleCLCD::addClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	fClients->setObject(client);
	IOLockUnlock(fLock);
}

void
AppleCLCD::removeClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	unsigned int i = fClients->getNextIndexOfObject(client, 0);
	if (i != (unsigned int)-1) {
		fClients->removeObject(i);
	}
	IOLockUnlock(fLock);
}

static uint32_t
get32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

static uint32_t b2l_fb_swaps_logged;
/* Presentation telemetry for the LIVE reporter: swaps, the longest gap between
 * two swaps since the last snapshot and how many gaps exceeded two seconds. */
static uint64_t b2l_fb_swap_count, b2l_fb_swap_last_abs, b2l_fb_swap_max_gap_abs, b2l_fb_swap_long_gaps;
static uint64_t b2l_fb_copy_count, b2l_fb_copy_total_abs, b2l_fb_copy_max_abs;
extern "C" void
b2l_fb_swap_snapshot(uint64_t out[7])
{
	out[0] = __atomic_load_n(&b2l_fb_swap_count, __ATOMIC_RELAXED);
	out[1] = __atomic_exchange_n(&b2l_fb_swap_max_gap_abs, 0, __ATOMIC_RELAXED);
	out[2] = __atomic_load_n(&b2l_fb_swap_long_gaps, __ATOMIC_RELAXED);
	out[3] = __atomic_load_n(&b2l_fb_swap_last_abs, __ATOMIC_RELAXED);
	out[4] = __atomic_load_n(&b2l_fb_copy_count, __ATOMIC_RELAXED);
	out[5] = __atomic_load_n(&b2l_fb_copy_total_abs, __ATOMIC_RELAXED);
	out[6] = __atomic_exchange_n(&b2l_fb_copy_max_abs, 0, __ATOMIC_RELAXED);
}
static void
b2l_fb_copy_note(uint64_t elapsed)
{
    __atomic_fetch_add(&b2l_fb_copy_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&b2l_fb_copy_total_abs, elapsed, __ATOMIC_RELAXED);
    uint64_t maximum = __atomic_load_n(&b2l_fb_copy_max_abs, __ATOMIC_RELAXED);
    while (elapsed > maximum && !__atomic_compare_exchange_n(&b2l_fb_copy_max_abs,
        &maximum, elapsed, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
static void
b2l_fb_swap_note(void)
{
	uint64_t now = mach_absolute_time(), last = __atomic_exchange_n(&b2l_fb_swap_last_abs, now, __ATOMIC_RELAXED);
	__atomic_fetch_add(&b2l_fb_swap_count, 1, __ATOMIC_RELAXED);
	if (last && now > last) {
		uint64_t gap = now - last, two_seconds = 0, max = __atomic_load_n(&b2l_fb_swap_max_gap_abs, __ATOMIC_RELAXED);
		nanoseconds_to_absolutetime(2 * NSEC_PER_SEC, &two_seconds);
		while (gap > max && !__atomic_compare_exchange_n(&b2l_fb_swap_max_gap_abs, &max, gap, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
		if (gap > two_seconds) __atomic_fetch_add(&b2l_fb_swap_long_gaps, 1, __ATOMIC_RELAXED);
	}
}

static uint64_t
b2l_probe_hash(const uint8_t *p, uint32_t n)
{
	uint64_t h = 0xcbf29ce484222325ull, v;
	for (uint32_t i = 0; i + 8 <= n; i += 8) { memcpy(&v, p + i, 8); h = (h ^ v) * 0x100000001b3ull; }
	return h;
}

static uint64_t
b2l_probe_uptime(void)
{
	uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_SEC;
}

void
AppleCLCD::probeStart(void)
{
	uint32_t bytes = fRowBytes * fHeight;
	fProbePages = (uint32_t)((bytes + PAGE_SIZE - 1) / PAGE_SIZE);
	fProbe = (uint8_t *)IOMallocData(bytes);
	fProbeHash = (uint64_t *)IOMallocData(fProbePages * sizeof(uint64_t));
	if (fProbe == NULL || fProbeHash == NULL) {
		IOLog("B2L FB PROBE: allocation failed\n");
		fProbe = NULL;
		return;
	}
	IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)fProbe, bytes,
	    kIODirectionNone, kernel_task);
	if (md != NULL && md->prepare() == kIOReturnSuccess) {
		unsigned runs = 0;
		for (IOByteCount off = 0; off < bytes && runs < 40;) {
			IOByteCount len = 0;
			addr64_t pa = md->getPhysicalSegment(off, &len, kIOMemoryMapperNone);
			if (!pa || !len) break;
			IOLog("B2L FB PROBE: run %u va_off=0x%llx pa=0x%llx len=0x%llx\n", runs++,
			    (uint64_t)off, (uint64_t)pa, (uint64_t)len);
			off += len;
		}
		md->complete();
	}
	OSSafeReleaseNULL(md);
	for (uint32_t i = 0; i < fProbePages; i++) {
		uint32_t n = (i + 1) * PAGE_SIZE <= bytes ? PAGE_SIZE : bytes - i * PAGE_SIZE;
		fProbeHash[i] = b2l_probe_hash(fProbe + (size_t)i * PAGE_SIZE, n);
	}
	IOLog("B2L FB PROBE: %u pages watched from sec=%llu, fill at sec=60\n", fProbePages, b2l_probe_uptime());
}

/* Every 5 s on the vsync work loop: report pages changed by anyone else,
 * then (once, at 60 s) fill the buffer and keep checking the pattern. */
void
AppleCLCD::probeTick(void)
{
	uint32_t bytes = fRowBytes * fHeight, changed = 0, logged = 0;
	uint64_t sec = b2l_probe_uptime();
	for (uint32_t i = 0; i < fProbePages; i++) {
		uint32_t n = (i + 1) * PAGE_SIZE <= bytes ? PAGE_SIZE : bytes - i * PAGE_SIZE;
		uint64_t h = b2l_probe_hash(fProbe + (size_t)i * PAGE_SIZE, n);
		if (h != fProbeHash[i]) {
			changed++;
			if (logged++ < 8) {
				IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(
				    (mach_vm_address_t)(fProbe + (size_t)i * PAGE_SIZE), PAGE_SIZE, kIODirectionNone, kernel_task);
				addr64_t pa = 0;
				if (md != NULL && md->prepare() == kIOReturnSuccess) {
					IOByteCount len = 0;
					pa = md->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
					md->complete();
				}
				OSSafeReleaseNULL(md);
				IOLog("B2L FB PROBE: foreign write page=%u pa=0x%llx sec=%llu\n", i, (uint64_t)pa, sec);
			}
			fProbeHash[i] = h;
		}
	}
	if (fProbeReports++ < 120) {
		IOLog("B2L FB PROBE: sec=%llu filled=%d changed_pages=%u\n", sec, fProbeFilled, changed);
	}
	if (!fProbeFilled && sec >= 60) {
		/* The same kind of write the dirty copy's first fill makes. */
		for (uint32_t y = 0; y < fHeight; y++) {
			uint32_t *row = (uint32_t *)(fProbe + (size_t)y * fRowBytes);
			for (uint32_t x = 0; x < fRowBytes / 4; x++) row[x] = 0xff000000u | (y << 8) | (x & 0xff);
		}
		for (uint32_t i = 0; i < fProbePages; i++) {
			uint32_t n = (i + 1) * PAGE_SIZE <= bytes ? PAGE_SIZE : bytes - i * PAGE_SIZE;
			fProbeHash[i] = b2l_probe_hash(fProbe + (size_t)i * PAGE_SIZE, n);
		}
		fProbeFilled = true;
		IOLog("B2L FB PROBE: filled at sec=%llu\n", b2l_probe_uptime());
	}
}

/* Wait until no presentation is queued or being copied. */
void
AppleCLCD::waitIdle(void)
{
	IOLockLock(fLock);
	if (fBusy) {
		uint64_t start = mach_absolute_time();
		while (fBusy) IOLockSleep(fLock, &fBusy, THREAD_UNINT);
		fPerfWaits++;
		fPerfWaitTicks += mach_absolute_time() - start;
	}
	IOLockUnlock(fLock);
}

void
AppleCLCD::copyWorker(thread_call_param_t owner, thread_call_param_t unused __unused)
{
	((AppleCLCD *)owner)->runCopy();
}

void
AppleCLCD::deconDumpMain(void *owner, wait_result_t result __unused)
{
	AppleCLCD *self = (AppleCLCD *)owner;

	IOSleep(45000);         /* after the GPU stage, before the CPU boost */
	self->deconDump();
	self->release();
	thread_terminate(current_thread());
}

/* See b2l_decon_dump.h. Nothing is written. */
void
AppleCLCD::deconDump(void)
{
	volatile uint32_t *decon = fDecon, *dma = fDma;

	if (decon == NULL || dma == NULL) {
		b2l_diag_record("B2L DECON: not mapped\n");
		return;
	}
	b2l_diag_record("B2L DECON: reading global\n");
	IOSleep(1500);
	b2l_diag_record("B2L DECON: global=%08x shadow_req=%08x trig=%08x path0=%08x path2=%08x panel_pa=%llx\n",
	    decon[0x0 / 4], decon[0x60 / 4], decon[0x70 / 4], decon[0x214 / 4], decon[0x230 / 4], (uint64_t)fPhys);
	IOSleep(1500);
	for (unsigned w = 0; w < B2L_DECON_WINDOWS; w++) {
		unsigned at = (0x1000 + w * 0x30) / 4;
		b2l_diag_record("B2L DECON: win%u control=%08x %08x start=%08x end=%08x\n", w, decon[at], decon[at + 1],
		    decon[at + 2], decon[at + 3]);
	}
	IOSleep(1500);
	for (unsigned c = 0; c < B2L_DPU_DMA_CHANNELS; c++) {
		unsigned at = (0x1000 + c * 0x1000) / 4;
		b2l_diag_record("B2L DECON: dma%u enable=%08x in_con=%08x src=%08x offset=%08x img=%08x base=%08x\n", c,
		    dma[at], dma[at + 0x8 / 4], dma[at + 0x10 / 4], dma[at + 0x14 / 4], dma[at + 0x18 / 4], dma[at + 0x40 / 4]);
	}
	b2l_diag_record("B2L DECON: dump done\n");
}

/*
 * Point DMA channel 0 and window 5 at a frame. SRC_SIZE is the whole frame in
 * pixels, IMG_SIZE the part shown, both height in the high half; the window's
 * start and end are panel coordinates. SHADOW_REG_UPDATE_REQ for window 5 makes
 * the hardware take all of it at the next frame trigger, as Samsung's driver does.
 */
void
AppleCLCD::programChannel(uint32_t base, uint32_t stride_px, uint32_t w, uint32_t h, uint32_t top)
{
	volatile uint32_t *dma = fDma + 0x1000 / 4, *win = fDecon + 0x1000 / 4 + 5 * 0x30 / 4;

	dma[0x14 / 4] = 0;
	dma[0x10 / 4] = (h << 16) | stride_px;
	dma[0x18 / 4] = (h << 16) | w;
	dma[0x40 / 4] = base;
	win[0x8 / 4] = top << 16;
	win[0xc / 4] = ((top + h - 1) << 16) | (w - 1);
	__asm__ volatile("dsb sy":::"memory");
	fDecon[0x60 / 4] = 1u << 5;
	__asm__ volatile("dsb sy":::"memory");
}

void
AppleCLCD::pointAtPanel(void)
{
	if (!fFlipped) {
		return;
	}
	programChannel((uint32_t)fPhys, fRowBytes / 4, fWidth, fHeight, 0);
	fFlipped = false;
	fShadowValid = false;   /* the panel memory is stale: the next copy fills it */
	fPerfFallbacks++;
}

/*
 * Show a surface without copying it. It must be the whole panel area in BGRA
 * with 4-byte pixels, in contiguous memory under 4 GB (the base register is 32
 * bits). Its cache lines are cleaned first: the CPU drew into cached memory and
 * the controller reads DRAM. The surface stays referenced until two flips later.
 */
bool
AppleCLCD::flipTo(B2LSurface *s, const Layer *l)
{
	if (!fScanout || s->memory == NULL || s->pixelFormat != B2L_FB_FORMAT_BGRA || s->bytesPerElement != 4 ||
	    s->elementWidth != 1 || s->elementHeight != 1 || l->dx != 0 || l->dy != 0 || l->sx != 0 || l->sy != 0 ||
	    (l->w > 0 && l->w != (int32_t)fWidth) || (l->h > 0 && l->h != (int32_t)fHeight) ||
	    s->width != fWidth || s->height != fHeight || (s->bytesPerRow & 3) || s->bytesPerRow / 4 > 0xffff ||
	    s->kernelAddress() == 0) {
		return false;
	}
	IOByteCount length = 0;
	uint64_t bytes = (uint64_t)s->bytesPerRow * s->height;
	addr64_t pa = s->memory->getPhysicalSegment(s->offset, &length, kIOMemoryMapperNone);
	if (pa == 0 || length < bytes || pa + bytes > 0x100000000ull || (pa & 63)) {
		return false;
	}
	uint64_t start = mach_absolute_time();
	flush_dcache64((addr64_t)s->kernelAddress() + s->offset, (unsigned)bytes, FALSE);
	uint64_t cleaned = mach_absolute_time();
	programChannel((uint32_t)pa, s->bytesPerRow / 4, s->width, s->height, fTopInset);
	fFlipped = true;
	s->retain();
	if (fShown[1] != NULL) {
		fShown[1]->release();
	}
	fShown[1] = fShown[0];
	fShown[0] = s;
	fPerfFlips++;
	fPerfCleanTicks += cleaned - start;
	fPerfFlipTicks += mach_absolute_time() - start;
	return true;
}

/* Copy the queued layers into the panel, then release them and go idle. */
void
AppleCLCD::runCopy(void)
{
	Job job;
	IOLockLock(fLock);
	job = fJob;
	fJob.count = 0;
	bool shadowValid = fShadowValid;
	IOLockUnlock(fLock);

	/* Stop console drawing before the first copy, so nothing can draw on
	 * the panel between the copy and the shadow becoming valid. */
	if (job.count && !fConsolePaused) {
		b2l_early_gui_present();
		fConsolePaused = true;
		/* Clear boot-console text from the camera band once; whole rows,
		 * so every write is 64-byte aligned like all other panel writes. */
		for (uint32_t y = 0; y < fTopInset; y++) {
			volatile uint64_t *row = (volatile uint64_t *)(fPanelPixels + (uintptr_t)y * fRowBytes);
			for (uint32_t x = 0; x < fRowBytes / 8; x++) row[x] = 0xff000000ff000000ull;
		}
		IOLog("B2L FB: first IOSurface frame copied to physical scanout\n");
	}
	if (job.count == 1 && flipTo(job.layer[0].s, &job.layer[0])) {
		job.layer[0].s->release();
		IOLockLock(fLock);
		fBusy = false;
		IOLockWakeup(fLock, &fBusy, false);
		IOLockUnlock(fLock);
		return;
	}
	pointAtPanel();         /* a frame the controller cannot take directly: copy it as before */
	uint64_t start = mach_absolute_time(), written = 0, pixels = 0;
	uint32_t copies = 0, full = 0;
	bool covered = false;
	const uint64_t panelBytes = (uint64_t)fRowBytes * fHeight;
	for (uint32_t i = 0; i < job.count; i++) {
		Layer *l = &job.layer[i];
		B2LSurface *s = l->s;
		uintptr_t src = s->kernelAddress();
		if (src && s->memory && s->bytesPerElement == 4 && s->elementWidth == 1 && s->elementHeight == 1 &&
		    s->pixelFormat == B2L_FB_FORMAT_BGRA) {
			int32_t w = l->w, h = l->h;
			if (w <= 0 || h <= 0) { w = (int32_t)s->width; h = (int32_t)s->height; }
			struct b2l_blit_rect r;
			/* First 64 copies: begin/end lines bracket the copy, and a sampled
			 * 8x8 grid of source pixels shows whether the frame is black. */
			bool trace = fCopyLogs < 64;
			uint64_t traceStart = 0, traceWritten = written;
			const char *path = (fShadow != NULL && shadowValid) ? "dirty" : (fShadow != NULL ? "fill" : "full");
			if (trace) {
				fCopyLogs++;
				uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
				b2l_diag_record("B2L FB COPY n=%u begin path=%s surface=%ux%u rect=%d,%d %dx%d sec=%llu\n",
				    fCopyLogs, path, s->width, s->height, l->dx, l->dy, w, h, ns / NSEC_PER_SEC);
				traceStart = mach_absolute_time();
			}
			int shown = b2l_blit_clip(panelBytes, fWidth, fHeight, fRowBytes, s->memory->getLength(),
			    s->width, s->height, s->bytesPerRow, s->offset, l->sx, l->sy, l->dx, l->dy, w, h, &r);
			if (shown && fShadow != NULL && shadowValid) {
				uint64_t n = 0;
				b2l_blit_dirty((void *)fPixels, fShadow, panelBytes, fWidth, fHeight, fRowBytes,
				    (const void *)src, s->memory->getLength(), s->width, s->height, s->bytesPerRow, s->offset,
				    l->sx, l->sy, l->dx, l->dy, w, h, &n);
				written += n;
			} else if (shown) {
				b2l_blit((void *)fPixels, panelBytes, fWidth, fHeight, fRowBytes,
				    (const void *)src, s->memory->getLength(), s->width, s->height, s->bytesPerRow, s->offset,
				    l->sx, l->sy, l->dx, l->dy, w, h);
				if (fShadow != NULL) {
					b2l_blit(fShadow, panelBytes, fWidth, fHeight, fRowBytes,
					    (const void *)src, s->memory->getLength(), s->width, s->height, s->bytesPerRow, s->offset,
					    l->sx, l->sy, l->dx, l->dy, w, h);
				}
				written += (uint64_t)r.w * 4 * r.h;
			}
			if (trace) {
				unsigned nonblack = 0;
				for (unsigned gy = 0; gy < 8 && s->height && s->width; gy++) {
					for (unsigned gx = 0; gx < 8; gx++) {
						uint64_t at = (uint64_t)s->offset + (uint64_t)(gy * s->height / 8) * s->bytesPerRow +
						    (uint64_t)(gx * s->width / 8) * 4;
						if (at + 4 <= s->memory->getLength()) {
							uint32_t px; memcpy(&px, (const uint8_t *)src + at, 4);
							if (px & 0x00FFFFFFu) nonblack++;
						}
					}
				}
				uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time() - traceStart, &ns);
				b2l_diag_record("B2L FB COPY n=%u end shown=%d written=%llu us=%llu nonblack=%u/64\n",
				    fCopyLogs, shown, written - traceWritten, ns / 1000, nonblack);
			}
			if (shown) {
				copies++;
				pixels += (uint64_t)r.w * r.h;
				if (r.w == fWidth && r.h == fHeight) { full++; covered = true; }
			}
		}
		s->release();
	}
	__asm__ volatile("dsb sy":::"memory");
	uint64_t ticks = mach_absolute_time() - start;
	if (copies) b2l_fb_copy_note(ticks);   /* S10 port: keep B2L FRAMES copy statistics */

	IOLockLock(fLock);
	if (covered && fShadow != NULL) fShadowValid = true;
	fPerfCopies += copies;
	fPerfFull += full;
	fPerfPixels += pixels;
	fPerfWritten += written;
	if (copies) {
		fPerfTicks += ticks;
		if (ticks > fPerfMaxTicks) fPerfMaxTicks = ticks;
	}
	fBusy = false;
	IOLockWakeup(fLock, &fBusy, false);
	IOLockUnlock(fLock);
}

/* Queue each enabled layer's surface for copying into the panel memory. */
void
AppleCLCD::swap(const uint8_t *rec)
{
	/* kern_SwapSetLayer stores client+0x164/+0x168; kern_SwapEnd sends
	 * client+0x18. client+0x194 (wire+0x17c) is unrelated and cleared by
	 * SwapBegin. Reading it silently discarded every native CPU frame. */
	uint32_t mask = get32(rec, 0x14c);
	uint32_t disabled = get32(rec, 0x150);
	b2l_fb_swap_note();
	static unsigned records;
	if (__atomic_fetch_add(&records, 1, __ATOMIC_RELAXED) < 32) {
		b2l_diag_record("B2L FB RECORD: swap=%u updated=0x%x disabled=0x%x ids=%u,%u,%u,%u\n",
		    get32(rec, 0x98), mask, disabled, get32(rec, 0x9c),
		    get32(rec, 0xa0), get32(rec, 0xa4), get32(rec, 0xa8));
	}

	Job job;
	job.count = 0;
	for (uint32_t layer = 0; layer < B2L_FB_LAYERS; layer++) {
		uint32_t id = get32(rec, 0x9c + 4 * layer);
		const uint8_t *a = rec + 0xac + 16 * layer;
		const uint8_t *b = rec + 0x10c + 16 * layer;
		int32_t sx = (int32_t)get32(a, 0), sy = (int32_t)get32(a, 4);
		int32_t dx = (int32_t)get32(b, 0), dy = (int32_t)get32(b, 4);
		int32_t w = (int32_t)get32(b, 8), h = (int32_t)get32(b, 12);

		if (!(mask & (1u << layer)) || (disabled & (1u << layer)) || id == 0) {
			continue;
		}
		B2LSurface *s = b2l_surface_copy(id);
		if (b2l_fb_swaps_logged < 16) {
			b2l_fb_swaps_logged++;
			IOLog("B2L FB: swap %u layer %u surface %u %ux%u fmt 0x%x bpr %u a %d,%d %ux%u b %d,%d %dx%d\n",
			    get32(rec, 0x98), layer, id, s ? s->width : 0, s ? s->height : 0,
			    s ? s->pixelFormat : 0, s ? s->bytesPerRow : 0, sx, sy, get32(a, 8),
			    get32(a, 12), dx, dy, w, h);
		}
		if (s == NULL || s == fSurface) {
			OSSafeReleaseNULL(s);
			continue;
		}
		Layer *l = &job.layer[job.count++];
		l->s = s;     /* reference released by runCopy */
		l->sx = sx; l->sy = sy; l->dx = dx; l->dy = dy; l->w = w; l->h = h;
	}

	/* The previous presentation's buffers stay readable until it is copied;
	 * waiting and queueing under one lock hold keeps clients from interleaving. */
	IOLockLock(fLock);
	fPerfSwaps++;
	if (fBusy) {
		uint64_t start = mach_absolute_time();
		while (fBusy) IOLockSleep(fLock, &fBusy, THREAD_UNINT);
		fPerfWaits++;
		fPerfWaitTicks += mach_absolute_time() - start;
	}
	if (job.count == 0) {
		IOLockUnlock(fLock);
		return;
	}
	fJob = job;
	fBusy = true;
	IOLockUnlock(fLock);
	if (fCopyCall != NULL) {
		thread_call_enter(fCopyCall);
	} else {
		runCopy();      /* no worker: copy in SwapEnd, as before */
	}
}

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
    b2l_diag_record("B2L FB OPEN: pid=%d type=%u\n",proc_selfpid(),type);
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	setProperty(kIOUserClientDefaultLockingKey,kOSBooleanTrue);
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService *provider)
{
	fFB = OSDynamicCast(AppleCLCD, provider);
	if (fFB == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fFB->addClient(this);
	return true;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fFB != NULL) {
		fFB->removeClient(this);
		IOLockLock(fFB->fLock);
		mach_port_t old = fPort;
		fPort = MACH_PORT_NULL;
		IOLockUnlock(fFB->fLock);
		if (old != MACH_PORT_NULL) {
			releaseNotificationPort(old);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, __unused UInt32 type,
    __unused io_user_reference_t refCon)
{
	mach_port_t old;
	IOLockLock(fFB->fLock);
	old = fPort;
	fPort = port;
	IOLockUnlock(fFB->fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

/* Called with the framebuffer lock held. */
void
IOMobileFramebufferUserClient::notify(uint32_t type, const uint64_t *values, uint32_t count)
{
	OSAsyncReference64 ref;

	if (type >= B2L_FB_NOTIFY_TYPES || fFunc[type] == 0 || fPort == MACH_PORT_NULL) {
		return;
	}
	bzero(ref, sizeof(ref));
	setAsyncReference64(ref, fPort, (mach_vm_address_t)fFunc[type], fRefcon[type], fTask);
	sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,
	    kIOUserNotifyOptionCanDrop);
}

static uint64_t b2l_fb_logged;        /* selectors already logged (< 64) */

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
    if (args == NULL) return kIOReturnBadArgument;
    static unsigned samples;
    if(__atomic_fetch_add(&samples,1,__ATOMIC_RELAXED)<64)
        b2l_diag_record("B2L FB CALL: pid=%d selector=%u scalar_in=%u struct_in=%llu struct_out=%llu\n",
            proc_selfpid(),selector,args?args->scalarInputCount:0,
            args?(uint64_t)args->structureInputSize:0,args?(uint64_t)args->structureOutputSize:0);

	IOReturn ret = kIOReturnSuccess;
	uint64_t out[2] = { 0, 0 };
	uint32_t outCount = 0;

	/* 0058: preserve unknown block/parameter failures while capturing their ABI.
	 * No profile data is fabricated from the requested output length. */
	if (selector == 68 || selector == 79 || selector == kB2LFBSetContrast) {
		static unsigned detailSamples;
		if (__atomic_fetch_add(&detailSamples, 1, __ATOMIC_RELAXED) < 24) {
			uint64_t values[3] = { 0, 0, 0 }, input = 0;
			if (args->scalarInput != NULL) {
				for (unsigned i = 0; i < args->scalarInputCount && i < 3; ++i)
					values[i] = args->scalarInput[i];
			}
			if (args->structureInputDescriptor == NULL && args->structureInput != NULL)
				memcpy(&input, args->structureInput,
				    args->structureInputSize < sizeof(input) ? args->structureInputSize : sizeof(input));
			b2l_diag_record("B2L FB DETAIL: pid=%d selector=%u values=0x%llx,0x%llx,0x%llx input=0x%llx\n",
			    proc_selfpid(), selector, values[0], values[1], values[2], input);
		}
	}

	switch (selector) {
	/* Native wrappers: 0x2294bf384 (74), 0x2294c00d4 (84),
	 * 0x2294c1adc (88), pristine iOS 26.6.2 cache, unslid addresses. */
	case kB2LFBFrameInfoDescriptor: {
		if (args->scalarInputCount != 1 || args->scalarInput == NULL ||
		    args->structureInputSize != 0 || args->structureInputDescriptor != NULL) {
			return kIOReturnBadArgument;
		}
		if (args->scalarInput[0] != 0) return kIOReturnBadArgument;
		if (args->structureOutputDescriptor != NULL) return kIOReturnUnsupported;
		if (args->scalarOutputCount != 2 || args->scalarOutput == NULL ||
		    args->structureOutputSize != 64 || args->structureOutput == NULL) {
			return kIOReturnNoSpace;
		}
		/* Fixed CPU scanout has no frame-info profiles. Native enumeration
		 * at 0x2294bde68 accepts descriptor count zero without reading a name. */
		bzero(args->structureOutput, 64);
		args->structureOutputSize = 64;
		outCount = 2; /* count = 0, selected index = 0 */
		break;
	}
	case kB2LFBSetContrast: {
		uint32_t contrastBits;
		if (args->scalarInputCount != 0 || args->structureInputDescriptor != NULL ||
		    args->structureInput == NULL || args->structureInputSize != sizeof(contrastBits) ||
		    args->scalarOutputCount != 0 || args->structureOutputSize != 0 ||
		    args->structureOutputDescriptor != NULL) {
			return kIOReturnBadArgument;
		}
		memcpy(&contrastBits, args->structureInput, sizeof(contrastBits));
		/* Identity contrast needs no pixel transform; other values are unsupported. */
		if (contrastBits != 0x3f800000U) ret = kIOReturnUnsupported;
		break;
	}
	case kB2LFBSelectDisplay:
		if (args->scalarInputCount != 1 || args->scalarInput == NULL ||
		    args->structureInputSize != 0 || args->structureInputDescriptor != NULL ||
		    args->scalarOutputCount != 0 || args->structureOutputSize != 0 ||
		    args->structureOutputDescriptor != NULL) {
			return kIOReturnBadArgument;
		}
		/* This service exposes only logical display type 0 (primary). */
		if (args->scalarInput[0] != 0) ret = kIOReturnUnsupported;
		break;
	case kB2LFBGetLayerDefaultSurface:
		out[0] = b2l_surface_port_in_task(fFB->fSurface, fTask);
		outCount = 1;
		break;
	case kB2LFBSwapBegin:
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		outCount = 1;
		break;
	case kB2LFBSwapEnd: {
		uint8_t rec[B2L_FB_SWAP_REC_SIZE];
		if (args->structureInputDescriptor != NULL ||
		    args->structureInput == NULL || args->structureInputSize != B2L_FB_SWAP_REC_SIZE) {
			ret = kIOReturnBadArgument;
			break;
		}
		memcpy(rec, args->structureInput, sizeof(rec));
		fFB->swap(rec);
		break;
	}
	case kB2LFBGetID:
		out[0] = 0;
		outCount = 1;
		break;
	case kB2LFBGetDisplaySize:
		out[0] = fFB->fWidth;
		out[1] = fFB->fHeight;
		outCount = 2;
		break;
	case kB2LFBIsMainDisplay:
		out[0] = 1;
		outCount = 1;
		break;
	case kB2LFBGetDotPitch:
		out[0] = B2L_FB_DOT_PITCH;
		outCount = 1;
		break;
	case kB2LFBNotifications:
		if (args->scalarInputCount >= 3 && args->scalarInput && args->scalarInput[2] < B2L_FB_NOTIFY_TYPES) {
			uint32_t type = (uint32_t)args->scalarInput[2];
			IOLockLock(fFB->fLock);
			fFunc[type] = args->scalarInput[0];
			fRefcon[type] = args->scalarInput[1];
			IOLockUnlock(fFB->fLock);
			char name[32];
			proc_selfname(name, sizeof(name));
			IOLog("B2L FB: %s notifications type %u %s\n", name, type,
			    args->scalarInput[0] ? "on" : "off");
		}
		break;
	case kB2LFBSwapWait:
		fFB->waitIdle();
		break;
    default: ret=kIOReturnUnsupported;break;
	}

    if(outCount && (args->scalarOutputCount<outCount || !args->scalarOutput))return kIOReturnNoSpace;
    for(uint32_t i=0;i<outCount;++i)args->scalarOutput[i]=out[i];
    args->scalarOutputCount=outCount;

	if (args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	if (selector >= 64 || !(b2l_fb_logged & (1ULL << selector))) {
		if (selector < 64) {
			b2l_fb_logged |= 1ULL << selector;
		}
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("B2L FB: %s sel %u in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    args->scalarInputCount, args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputSize, ret);
	}
	return ret;
}
#endif /* B2L */
