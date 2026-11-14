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
/* S10 port: B2LMali draws the scaled frame onto the panel (see runCopy). */
extern "C" void b2l_mif_for_display(int on);
extern "C" int b2l_mali_present_scaled(uint32_t source_id, uint32_t panel_id, uint32_t width, uint32_t height,
    uint64_t *gpu_ns);

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
/* S10: DSIM0 (live device tree dsim@0x19080000), exynos9820 regs-dsim.h. */
#define B2L_DSIM_BASE                 0x19080000ull
#define B2L_DSIM_BYTES                0x1000
#define B2L_DSIM_LINK_STATUS1         0x0c
#define B2L_DSIM_INTSRC               0x50
#define B2L_DSIM_INT_PH_FIFO_EMPTY    (1u << 28)
#define B2L_DSIM_PKTHDR               0x58
#define B2L_DSIM_FIFOCTRL             0x68
#define B2L_DSIM_FIFO_EMPTY_PH        (1u << 10)
#define B2L_FB_FORMAT_BGRA            0x42475241
/* Rows kept black above iOS for the S10+ front-camera hole (top right). 136
 * leaves 1440x2904, exactly 480x968 points at 3x, and keeps every row start
 * 64-byte aligned. Boot argument b2l_fb_top_inset overrides it. */
#define B2L_FB_TOP_INSET              136
#define B2L_FB_SEL_DISPLAY_POWER      12      /* S10 port: one scalar, 0 off / 1 on */

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
	/* S10 port: rendering below panel resolution (boot argument
	 * b2l_fb_render_width). iOS draws fWidth x fHeight (GetDisplaySize); the
	 * shadow holds that frame, and each changed span is scaled up (nearest
	 * texel) into fOutWidth x fOutHeight panel pixels. Unscaled, fOut* equal
	 * fWidth/fHeight and fShadowStride equals fRowBytes. */
	bool          fScaled;
	uint32_t      fOutWidth, fOutHeight, fShadowStride, fDotPitch;
	uint16_t     *fXMap;            /* source column of each panel column */
	uint16_t     *fYFirst;          /* first panel row of each source row */
	uint8_t      *fYCount;          /* panel rows of each source row */
	uint32_t     *fExpand;          /* one scaled panel row, cacheable; guarded by fBusy */
	/* GPU present (b2l_fb_gpu, default on): a full-frame single layer is scaled
	 * onto the panel by the Mali instead of the CPU. The shadow is not updated
	 * then, so it is marked invalid after each GPU frame. */
	bool          fGpuPresent;
	uint32_t      fGpuLogs, fGpuFallbacks;
	uint64_t      fGpuFrames;
	bool scaledSetup(uint32_t renderWidth);
	void presentSpan(uint32_t sy, uint32_t sx0, uint32_t sx1);
	void presentAll(void);
	int copyScaled(B2LSurface *s, const Layer *l, bool shadowValid, uint64_t *written,
	    uint64_t *pixels, bool *covered);
	/* S10 port: display off. This port has no panel power control, so iOS's
	 * "display off" left the last frame lit (Always-On could not be turned
	 * off). Off shows black, which an OLED pixel does not emit, and frames go
	 * to the shadow only. On, the panel stays black for iOS's next frame (the
	 * shadow then still holds the frame from before off, shown for about a
	 * second in boot 16); only if none comes within a second is the shadow
	 * written back. Guarded by fLock + fBusy. */
	bool          fBlanked;
	int           fBlankWanted;     /* atomic: 1 off, 0 on */
	/* S10: real panel sleep. DSIM0 (0x19080000) sends DCS short writes to the
	 * panel's driver IC: display off + sleep in after blanking, sleep out +
	 * display on before the next frame. Register offsets from Samsung's
	 * exynos9820 dpu20 regs-dsim.h. b2l_fb_panel_sleep=0 keeps blanking only. */
	IOMemoryMap  *fDsimMap;
	volatile uint32_t *fDsim;
	bool          fPanelSleep;      /* DSIM accepted commands at start */
	bool          fPanelAsleep;
	bool dsimShortWrite(uint8_t command);
	/* S10: stripes from the second wake on (boots 33-36). DSIM 0x000-0x0fc and
	 * DECON 0x000-0x0fc, 0x200-0x27c as the boot chain left them; at every
	 * display change the words that differ are logged. Reads only, and not
	 * DSIM 0x58-0x60 (packet FIFOs: a read could pop the RX FIFO). */
	uint32_t      fWatch[160];
	bool          fWatchReady;
	unsigned      fWatchEvents;
	uint32_t watchRead(unsigned i);
	void panelWatch(const char *when);
	thread_call_t fBlankCall, fRestoreCall;
	bool          fWakeRestore;     /* the shadow may be shown if no frame comes */
	uint64_t      fWakeCopies;      /* fPerfCopies when the display came on */
	void applyBlank(void);
	void restoreAfterWake(void);
	static void blankWorker(thread_call_param_t owner, thread_call_param_t unused);
	static void restoreWorker(thread_call_param_t owner, thread_call_param_t unused);

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
static AppleCLCD *gB2LCLCD;     /* S10 port: target of b2l_fb_display_power */

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
	fOutWidth = fWidth;
	fOutHeight = fHeight;
	fShadowStride = fRowBytes;
	fDotPitch = B2L_FB_DOT_PITCH;
	uint32_t renderWidth = 0;
	PE_parse_boot_argn("b2l_fb_render_width", &renderWidth, sizeof(renderWidth));
	if (dirty && renderWidth >= 320 && renderWidth < fWidth && !scaledSetup(renderWidth)) {
		IOLog("B2L FB: rendering at %u wide unavailable, full resolution\n", renderWidth);
	}
	fShadow = dirty ? (uint8_t *)IOMallocData((vm_size_t)fShadowStride * fHeight) : NULL;
	if (fScaled && fShadow == NULL) {
		/* The scaled path draws only from the shadow: without one, full size. */
		fScaled = false;
		fWidth = fOutWidth;
		fHeight = fOutHeight;
		fShadowStride = fRowBytes;
		fDotPitch = B2L_FB_DOT_PITCH;
	}
	IOLog("B2L FB: iOS renders %ux%u, panel %ux%u%s, dot pitch %u\n", fWidth, fHeight, fOutWidth, fOutHeight,
	    fScaled ? " (scaled up)" : "", fDotPitch);
	uint32_t gpuPresent = 1;
	PE_parse_boot_argn("b2l_fb_gpu", &gpuPresent, sizeof(gpuPresent));
	fGpuPresent = gpuPresent && fScaled && fTopInset == 0;
	IOLog("B2L FB: GPU present %s\n", fGpuPresent ? "on" : "off");
	uint32_t probe = 0;
	PE_parse_boot_argn("b2l_fb_probe", &probe, sizeof(probe));
	if (probe) probeStart();
	/* 0109 copied on a worker thread and SwapEnd returned before the copy;
	 * backboardd then presented almost nothing and the screen stayed dark.
	 * Until that contract is understood, copy inside SwapEnd (the worker
	 * path stays, unused, behind fCopyCall). */
	fCopyCall = NULL;
	IOLog("B2L FB: present shadow %s, copy worker %s\n", fShadow ? "on" : "off", fCopyCall ? "on" : "off");
	/* S10 port: display off from iOS (selector 12). b2l_fb_blank=0 disables it. */
	uint32_t blank = 1;
	PE_parse_boot_argn("b2l_fb_blank", &blank, sizeof(blank));
	fBlankCall = blank ? thread_call_allocate(&AppleCLCD::blankWorker, this) : NULL;
	fRestoreCall = fBlankCall ? thread_call_allocate(&AppleCLCD::restoreWorker, this) : NULL;
	if (fBlankCall != NULL) gB2LCLCD = this;
	IOLog("B2L FB: display off by blanking %s\n", fBlankCall ? "on" : "off");
	IOMemoryDescriptor *regs = IOMemoryDescriptor::withPhysicalAddress(B2L_DECON_BASE, B2L_DECON_BYTES, kIODirectionInOut);
	fDeconMap = regs ? regs->map(kIOMapInhibitCache) : NULL;
	OSSafeReleaseNULL(regs);
	regs = IOMemoryDescriptor::withPhysicalAddress(B2L_DPU_DMA_BASE, B2L_DPU_DMA_BYTES, kIODirectionInOut);
	fDmaMap = regs ? regs->map(kIOMapInhibitCache) : NULL;
	OSSafeReleaseNULL(regs);
	fDecon = fDeconMap ? (volatile uint32_t *)fDeconMap->getVirtualAddress() : NULL;
	fDma = fDmaMap ? (volatile uint32_t *)fDmaMap->getVirtualAddress() : NULL;
	/* S10: DSIM0 for panel sleep. Only used when it looks like the boot chain
	 * left it: command mode, packet header FIFO empty, no all-ones reads. */
	int panelSleep = 1;
	PE_parse_boot_argn("b2l_fb_panel_sleep", &panelSleep, sizeof(panelSleep));
	regs = panelSleep && fBlankCall != NULL ?
	    IOMemoryDescriptor::withPhysicalAddress(B2L_DSIM_BASE, B2L_DSIM_BYTES, kIODirectionInOut) : NULL;
	fDsimMap = regs ? regs->map(kIOMapInhibitCache) : NULL;
	OSSafeReleaseNULL(regs);
	fDsim = fDsimMap ? (volatile uint32_t *)fDsimMap->getVirtualAddress() : NULL;
	if (fDsim != NULL) {
		uint32_t status1 = fDsim[B2L_DSIM_LINK_STATUS1 / 4], fifo = fDsim[B2L_DSIM_FIFOCTRL / 4];
		fPanelSleep = status1 != UINT32_MAX && fifo != UINT32_MAX && (fifo & B2L_DSIM_FIFO_EMPTY_PH);
		IOLog("B2L FB: DSIM link_status1=0x%x (cmd mode %u) fifoctrl=0x%x: panel sleep %s\n", status1,
		    (status1 >> 26) & 1, fifo, fPanelSleep ? "on" : "off");
	}
	panelWatch("at start");
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

/* S10 port: display off/on, see fBlanked. Called from iOS's display power
 * request (selector 12): only records the wish; the panel is rewritten on a
 * thread call. */
/* S10 port: the last display power iOS asked for, for B2LMali's CPU clocks
 * (standby rates while the display is off). */
static int gB2LDisplayOff;

extern "C" int
b2l_fb_display_is_off(void)
{
	return __atomic_load_n(&gB2LDisplayOff, __ATOMIC_ACQUIRE);
}

extern "C" void
b2l_fb_display_power(int on)
{
	AppleCLCD *fb = gB2LCLCD;
	__atomic_store_n(&gB2LDisplayOff, on ? 0 : 1, __ATOMIC_RELEASE);
	if (fb == NULL || fb->fBlankCall == NULL) return;
	__atomic_store_n(&fb->fBlankWanted, on ? 0 : 1, __ATOMIC_RELEASE);
	thread_call_enter(fb->fBlankCall);
}

void
AppleCLCD::blankWorker(thread_call_param_t owner, thread_call_param_t unused __unused)
{
	((AppleCLCD *)owner)->applyBlank();
}

/*
 * One DCS short write without a parameter (data type 0x05): clear the header
 * FIFO empty flag, queue the header, wait up to 50 ms for the FIFO to drain.
 * As Samsung's dsim_write_data for MIPI_DSI_DCS_SHORT_WRITE.
 */
bool
AppleCLCD::dsimShortWrite(uint8_t command)
{
	if (fDsim == NULL || !fPanelSleep) return false;
	uint32_t fifo = fDsim[B2L_DSIM_FIFOCTRL / 4];
	if (fifo == UINT32_MAX || !(fifo & B2L_DSIM_FIFO_EMPTY_PH)) return false;
	fDsim[B2L_DSIM_INTSRC / 4] = B2L_DSIM_INT_PH_FIFO_EMPTY;
	fDsim[B2L_DSIM_PKTHDR / 4] = 0x05u | ((uint32_t)command << 8);
	__asm__ volatile("dsb sy":::"memory");
	for (unsigned i = 0; i < 500; i++) {
		if (fDsim[B2L_DSIM_FIFOCTRL / 4] & B2L_DSIM_FIFO_EMPTY_PH) return true;
		IODelay(100);
	}
	return false;
}

uint32_t
AppleCLCD::watchRead(unsigned i)
{
	if (i < 64) {
		unsigned off = i * 4;
		return fDsim == NULL || (off >= 0x58 && off <= 0x60) ? 0 : fDsim[i];
	}
	if (fDecon == NULL) {
		return 0;
	}
	return i < 128 ? fDecon[i - 64] : fDecon[0x200 / 4 + (i - 128)];
}

void
AppleCLCD::panelWatch(const char *when)
{
	static const unsigned count = sizeof(fWatch) / sizeof(fWatch[0]);
	if (!fWatchReady) {
		for (unsigned i = 0; i < count; i++) {
			fWatch[i] = watchRead(i);
		}
		fWatchReady = true;
		b2l_diag_record("B2L PANEL %s: dsim status1=%08x intsrc=%08x decon global=%08x pend=%08x %08x\n", when,
		    fWatch[0x0c / 4], fWatch[0x50 / 4], fWatch[64], fWatch[64 + 0x4c / 4], fWatch[64 + 0x50 / 4]);
		return;
	}
	if (fWatchEvents++ > 400) {
		return;
	}
	char line[200];
	int n = 0, shown = 0;
	for (unsigned i = 0; i < count && shown < 40; i++) {
		uint32_t now = watchRead(i);
		if (now == fWatch[i]) {
			continue;
		}
		unsigned off = i < 64 ? i * 4 : i < 128 ? (i - 64) * 4 : 0x200 + (i - 128) * 4;
		n += snprintf(line + n, sizeof(line) - n, " %s+%03x %x>%x", i < 64 ? "dsim" : "decon", off, fWatch[i], now);
		shown++;
		if (n > 150) {
			b2l_diag_record("B2L PANEL %s:%s\n", when, line);
			n = 0;
		}
	}
	b2l_diag_record("B2L PANEL %s:%s%s\n", when, n ? line : "", shown ? "" : " as at boot");
}

void
AppleCLCD::applyBlank(void)
{
	bool off = __atomic_load_n(&fBlankWanted, __ATOMIC_ACQUIRE) == 1;
	/*
	 * S10: display off = DCS display off, so the panel goes dark at once; the
	 * panel memory is then painted black while dark, three frames let the black
	 * reach the panel, and DCS sleep in puts it to sleep; frames wait in the
	 * shadow. Display on = sleep out, 120 ms, display on: the panel holds black
	 * until iOS's next frame, so the frame from before off never shows.
	 * Without DSIM this is plain blanking, as before.
	 *
	 * The memory clock changes only while the panel sleeps (no TE, so DECON
	 * reads nothing): raised here before anything reaches the panel again,
	 * lowered once it has gone to sleep. Changed under a running panel it
	 * left striped bars until reboot: boots 33, 34 and 35 each got them on the
	 * wake where the panel came back before the memory clock rose.
	 */
	panelWatch(off ? "before off" : "before on");
	if (!off) b2l_mif_for_display(1);
	if (off && fPanelSleep && !fPanelAsleep) {
		fPanelAsleep = dsimShortWrite(0x28);            /* display off */
		if (!fPanelAsleep) b2l_diag_record("B2L FB: DCS display off FAILED, blanking only\n");
	}
	auto light = [this]() {
		if (!fPanelAsleep) return;
		bool done = false;
		for (unsigned attempt = 0; attempt < 3 && !done; attempt++) {
			bool out = dsimShortWrite(0x11);        /* sleep out (harmless if awake) */
			IOSleep(120);
			done = dsimShortWrite(0x29) && out;     /* display on */
			if (!done) IOSleep(20);
		}
		fPanelAsleep = false;
		b2l_diag_record("B2L FB: DCS sleep out + display on %s\n", done ? "sent" : "FAILED");
		panelWatch("lit");
	};

	IOLockLock(fLock);
	while (fBusy) IOLockSleep(fLock, &fBusy, THREAD_UNINT);
	if (off == fBlanked) {
		IOLockUnlock(fLock);
		if (!off) light();
		return;
	}
	fBlanked = off;
	fBusy = true;           /* no copy runs while the panel is rewritten */
	IOLockUnlock(fLock);

	pointAtPanel();         /* a flipped channel would keep showing its surface */
	IOLockLock(fLock);
	bool shadowValid = fShadowValid;
	IOLockUnlock(fLock);
	uint64_t start = mach_absolute_time();
	if (off) {
		/* Whole rows, so every write is 64-byte aligned like all other panel writes. */
		for (uint32_t y = 0; y < fOutHeight; y++) {
			volatile uint64_t *row = (volatile uint64_t *)(fPixels + (uintptr_t)y * fRowBytes);
			for (uint32_t x = 0; x < fRowBytes / 8; x++) row[x] = 0xff000000ff000000ull;
		}
	}
	__asm__ volatile("dsb sy":::"memory");
	if (off && fPanelAsleep) {
		IOSleep(50);                                    /* the black frame reaches the panel */
		bool slept = dsimShortWrite(0x10);              /* sleep in */
		IOSleep(120);
		b2l_diag_record("B2L FB: DCS sleep in %s\n", slept ? "sent" : "FAILED");
		panelWatch("asleep");
		if (slept) b2l_mif_for_display(0);
	}
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);

	IOLockLock(fLock);
	if (!off) {
		/* Stay black for iOS's next frame, which then fills the whole panel. */
		fWakeRestore = fShadow != NULL && shadowValid;
		fWakeCopies = fPerfCopies;
		fShadowValid = false;
	}
	fBusy = false;
	IOLockWakeup(fLock, &fBusy, false);
	IOLockUnlock(fLock);
	if (!off) light();
	if (!off && fRestoreCall != NULL) {
		uint64_t deadline;
		clock_interval_to_deadline(1000, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(fRestoreCall, deadline);
	}
	b2l_diag_record("B2L FB: display %s, panel %s in %llu us\n", off ? "off" : "on",
	    off ? (fPanelAsleep ? "blanked and asleep" : "blanked") : "black until the next frame", ns / 1000);
}

void
AppleCLCD::restoreWorker(thread_call_param_t owner, thread_call_param_t unused __unused)
{
	((AppleCLCD *)owner)->restoreAfterWake();
}

/* A second after the display came on: if iOS sent no frame, show the shadow. */
void
AppleCLCD::restoreAfterWake(void)
{
	panelWatch("1 s after on");
	IOLockLock(fLock);
	while (fBusy) IOLockSleep(fLock, &fBusy, THREAD_UNINT);
	if (fBlanked || !fWakeRestore || fPerfCopies != fWakeCopies || fShadow == NULL) {
		bool copied = fPerfCopies != fWakeCopies;
		fWakeRestore = false;
		IOLockUnlock(fLock);
		b2l_diag_record("B2L FB: display on, %s\n", copied ? "new frame shown" : "nothing to restore");
		return;
	}
	fWakeRestore = false;
	fBusy = true;
	IOLockUnlock(fLock);
	const uint64_t panelBytes = (uint64_t)fRowBytes * fHeight;
	bool restored = true;
	if (fScaled) {
		presentAll();
	} else {
		restored = b2l_blit((void *)fPixels, panelBytes, fWidth, fHeight, fRowBytes,
		    fShadow, panelBytes, fWidth, fHeight, fRowBytes, 0, 0, 0, 0, 0,
		    (int32_t)fWidth, (int32_t)fHeight) != 0;
	}
	__asm__ volatile("dsb sy":::"memory");
	IOLockLock(fLock);
	if (restored) fShadowValid = true;
	fBusy = false;
	IOLockWakeup(fLock, &fBusy, false);
	IOLockUnlock(fLock);
	b2l_diag_record("B2L FB: display on, no frame in 1 s, shadow %s\n", restored ? "restored" : "not restored");
}

/* S10 port: geometry for rendering renderWidth wide. The height keeps the
 * panel's aspect and is a whole number of points at 3x, unless the boot
 * argument b2l_fb_render_height names one: boot 17 at 1125x2376 (375x792
 * points, no real iPhone) got UIKit's red "rdar:45025538" fallback status
 * bar, so the loader asks for a real iPhone geometry instead. */
bool
AppleCLCD::scaledSetup(uint32_t renderWidth)
{
	uint32_t renderHeight = (uint32_t)(((uint64_t)fOutHeight * renderWidth + fOutWidth / 2) / fOutWidth);
	renderHeight += (3 - renderHeight % 3) % 3;
	uint32_t requestedHeight = 0;
	if (PE_parse_boot_argn("b2l_fb_render_height", &requestedHeight, sizeof(requestedHeight)) &&
	    requestedHeight >= 480) {
		renderHeight = requestedHeight;
	}
	if (renderHeight == 0 || renderHeight >= fOutHeight || fOutWidth > 65535 || fOutHeight > 65535) {
		return false;
	}
	uint16_t *xmap = (uint16_t *)IOMallocData(fOutWidth * sizeof(uint16_t));
	uint16_t *yfirst = (uint16_t *)IOMallocData(renderHeight * sizeof(uint16_t));
	uint8_t *ycount = (uint8_t *)IOMallocData(renderHeight);
	if (xmap == NULL || yfirst == NULL || ycount == NULL) {
		if (xmap) IOFreeData(xmap, fOutWidth * sizeof(uint16_t));
		if (yfirst) IOFreeData(yfirst, renderHeight * sizeof(uint16_t));
		if (ycount) IOFreeData(ycount, renderHeight);
		return false;
	}
	for (uint32_t x = 0; x < fOutWidth; x++) {
		xmap[x] = (uint16_t)((uint64_t)x * renderWidth / fOutWidth);
	}
	memset(ycount, 0, renderHeight);
	for (uint32_t y = 0; y < fOutHeight; y++) {
		uint32_t sy = (uint32_t)((uint64_t)y * renderHeight / fOutHeight);
		if (ycount[sy] == 0) yfirst[sy] = (uint16_t)y;
		ycount[sy]++;
	}
	uint32_t *expand = (uint32_t *)IOMallocData(fOutWidth * sizeof(uint32_t));
	if (expand == NULL) {
		IOFreeData(xmap, fOutWidth * sizeof(uint16_t));
		IOFreeData(yfirst, renderHeight * sizeof(uint16_t));
		IOFreeData(ycount, renderHeight);
		return false;
	}
	fExpand = expand;
	fXMap = xmap;
	fYFirst = yfirst;
	fYCount = ycount;
	fWidth = renderWidth;
	fHeight = renderHeight;
	fShadowStride = (renderWidth * 4 + 63) & ~63u;
	fDotPitch = (uint32_t)((uint64_t)B2L_FB_DOT_PITCH * renderWidth / fOutWidth);
	fScaled = true;
	return true;
}

/* Shadow row sy, columns [sx0, sx1), to the panel rows it covers. Panel spans
 * are widened to 64-byte boundaries like every other panel write. */
void
AppleCLCD::presentSpan(uint32_t sy, uint32_t sx0, uint32_t sx1)
{
	const uint32_t *row = (const uint32_t *)(fShadow + (size_t)sy * fShadowStride);
	uint32_t dx0 = (uint32_t)(((uint64_t)sx0 * fOutWidth + fWidth - 1) / fWidth);
	uint32_t dx1 = (uint32_t)(((uint64_t)sx1 * fOutWidth + fWidth - 1) / fWidth);
	dx0 &= ~15u;
	dx1 = (dx1 + 15) & ~15u;
	if (dx1 > fOutWidth) dx1 = fOutWidth;
	if (dx1 <= dx0) return;
	/* Scale the span once into cacheable memory, then 64-bit stores per panel
	 * row it covers (one or two). Boot 19: memcpy into the write-combined
	 * panel doubled the copy time (4.9 to 10.7 ms), so plain paired stores, as
	 * in boot 18, write it. dx0 is even (16-aligned). */
	for (uint32_t dx = dx0; dx < dx1; dx++) fExpand[dx] = row[fXMap[dx]];
	const uint64_t *pairs = (const uint64_t *)(fExpand + dx0);
	uint32_t count = (dx1 - dx0) / 2;
	for (uint32_t k = 0; k < fYCount[sy]; k++) {
		uintptr_t base = fPixels + (uintptr_t)(fYFirst[sy] + k) * fRowBytes;
		volatile uint64_t *out = (volatile uint64_t *)(base + (uintptr_t)dx0 * 4);
		for (uint32_t i = 0; i < count; i++) out[i] = pairs[i];
		if ((dx1 - dx0) & 1) ((volatile uint32_t *)base)[dx1 - 1] = fExpand[dx1 - 1];
	}
}

void
AppleCLCD::presentAll(void)
{
	for (uint32_t sy = 0; sy < fHeight; sy++) presentSpan(sy, 0, fWidth);
}

/* One layer into the shadow; every changed row span is presented scaled.
 * Returns 1 when the layer was shown. */
int
AppleCLCD::copyScaled(B2LSurface *s, const Layer *l, bool shadowValid, uint64_t *written,
    uint64_t *pixels, bool *covered)
{
	uintptr_t src = s->kernelAddress();
	if (!src || !s->memory || s->bytesPerElement != 4 || s->elementWidth != 1 || s->elementHeight != 1 ||
	    s->pixelFormat != B2L_FB_FORMAT_BGRA || fShadow == NULL) {
		return 0;
	}
	int32_t w = l->w, h = l->h;
	if (w <= 0 || h <= 0) { w = (int32_t)s->width; h = (int32_t)s->height; }
	const uint64_t shadowBytes = (uint64_t)fShadowStride * fHeight;
	struct b2l_blit_rect r;
	if (!b2l_blit_clip(shadowBytes, fWidth, fHeight, fShadowStride, s->memory->getLength(),
	    s->width, s->height, s->bytesPerRow, s->offset, l->sx, l->sy, l->dx, l->dy, w, h, &r)) {
		return 0;
	}
	uint32_t dy = (uint32_t)(r.dst / fShadowStride), dx = (uint32_t)((r.dst % fShadowStride) / 4);
	for (uint32_t y = 0; y < r.h; y++) {
		const uint32_t *in = (const uint32_t *)(src + r.src + (uint64_t)y * s->bytesPerRow);
		uint32_t *shadow = (uint32_t *)(fShadow + (uint64_t)(dy + y) * fShadowStride + (uint64_t)dx * 4);
		uint32_t first = 0, last = r.w;
		if (shadowValid) {
			/* Most rows of most frames are unchanged: one memcmp settles them. */
			if (memcmp(in, shadow, (size_t)r.w * 4) == 0) continue;
			while (first < r.w && in[first] == shadow[first]) first++;
			while (last > first && in[last - 1] == shadow[last - 1]) last--;
		}
		memcpy(shadow + first, in + first, (size_t)(last - first) * 4);
		presentSpan(dy + y, dx + first, dx + last);
		*written += (uint64_t)(last - first) * 4 * fYCount[dy + y];
	}
	*pixels += (uint64_t)r.w * r.h;
	*covered = r.w == fWidth && r.h == fHeight;
	return 1;
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
	bool blanked = fBlanked;
	IOLockUnlock(fLock);

	if (blanked) {
		/* S10 port, display off: keep the shadow current, leave the panel black. */
		const uint64_t shadowBytes = (uint64_t)fShadowStride * fHeight;
		bool full = false;
		for (uint32_t i = 0; i < job.count; i++) {
			Layer *l = &job.layer[i];
			B2LSurface *s = l->s;
			uintptr_t src = s->kernelAddress();
			if (fShadow != NULL && src && s->memory && s->bytesPerElement == 4 && s->elementWidth == 1 &&
			    s->elementHeight == 1 && s->pixelFormat == B2L_FB_FORMAT_BGRA) {
				int32_t w = l->w, h = l->h;
				if (w <= 0 || h <= 0) { w = (int32_t)s->width; h = (int32_t)s->height; }
				struct b2l_blit_rect r;
				if (b2l_blit_clip(shadowBytes, fWidth, fHeight, fShadowStride, s->memory->getLength(),
				    s->width, s->height, s->bytesPerRow, s->offset, l->sx, l->sy, l->dx, l->dy, w, h, &r) &&
				    b2l_blit(fShadow, shadowBytes, fWidth, fHeight, fShadowStride,
				    (const void *)src, s->memory->getLength(), s->width, s->height, s->bytesPerRow, s->offset,
				    l->sx, l->sy, l->dx, l->dy, w, h) &&
				    r.w == fWidth && r.h == fHeight) {
					full = true;
				}
			}
			s->release();
		}
		IOLockLock(fLock);
		if (full) fShadowValid = true;
		fBusy = false;
		IOLockWakeup(fLock, &fBusy, false);
		IOLockUnlock(fLock);
		return;
	}

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
	if (fScaled && fGpuPresent && job.count == 1) {
		Layer *l = &job.layer[0];
		B2LSurface *s = l->s;
		int32_t w = l->w, h = l->h;
		if (w <= 0 || h <= 0) { w = (int32_t)s->width; h = (int32_t)s->height; }
		if (l->sx == 0 && l->sy == 0 && l->dx == 0 && l->dy == 0 && (uint32_t)w == fWidth && (uint32_t)h == fHeight &&
		    s->width == fWidth && s->height == fHeight && s->pixelFormat == B2L_FB_FORMAT_BGRA && fSurface != NULL) {
			uint64_t gpuStart = mach_absolute_time(), gpuNS = 0;
			if (b2l_mali_present_scaled(s->id, fSurface->id, fOutWidth, fOutHeight, &gpuNS)) {
				uint64_t ticks = mach_absolute_time() - gpuStart;
				b2l_fb_copy_note(ticks);
				if (fGpuLogs < 12) {
					uint64_t ns = 0;
					absolutetime_to_nanoseconds(ticks, &ns);
					fGpuLogs++;
					b2l_diag_record("B2L FB: GPU present %ux%u -> %ux%u in %llu us (GPU %llu us)\n", fWidth, fHeight,
					    fOutWidth, fOutHeight, ns / 1000, gpuNS / 1000);
				}
				s->release();
				IOLockLock(fLock);
				fGpuFrames++;
				fShadowValid = false;
				fPerfCopies++;
				fPerfFull++;
				fPerfPixels += (uint64_t)fWidth * fHeight;
				fPerfTicks += ticks;
				if (ticks > fPerfMaxTicks) fPerfMaxTicks = ticks;
				fBusy = false;
				IOLockWakeup(fLock, &fBusy, false);
				IOLockUnlock(fLock);
				return;
			}
			if (fGpuFallbacks++ < 8) {
				b2l_diag_record("B2L FB: GPU present unavailable, CPU copy (frame %llu)\n", fGpuFrames);
			}
			shadowValid = false;    /* GPU frames left the shadow behind */
		}
	}
	if (fScaled) {
		uint64_t scaledStart = mach_absolute_time(), scaledWritten = 0, scaledPixels = 0;
		uint32_t scaledCopies = 0, scaledFull = 0;
		bool valid = shadowValid, coveredAll = false;
		for (uint32_t i = 0; i < job.count; i++) {
			uint64_t n = 0, px = 0;
			bool cov = false;
			if (copyScaled(job.layer[i].s, &job.layer[i], valid, &n, &px, &cov)) {
				scaledCopies++;
				scaledWritten += n;
				scaledPixels += px;
				if (cov) { scaledFull++; valid = true; coveredAll = true; }
			}
			job.layer[i].s->release();
		}
		__asm__ volatile("dsb sy":::"memory");
		uint64_t scaledTicks = mach_absolute_time() - scaledStart;
		if (scaledCopies) b2l_fb_copy_note(scaledTicks);
		IOLockLock(fLock);
		if (coveredAll) fShadowValid = true;
		fPerfCopies += scaledCopies;
		fPerfFull += scaledFull;
		fPerfPixels += scaledPixels;
		fPerfWritten += scaledWritten;
		if (scaledCopies) {
			fPerfTicks += scaledTicks;
			if (scaledTicks > fPerfMaxTicks) fPerfMaxTicks = scaledTicks;
		}
		fBusy = false;
		IOLockWakeup(fLock, &fBusy, false);
		IOLockUnlock(fLock);
		return;
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
		out[0] = fFB->fDotPitch;
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
	case B2L_FB_SEL_DISPLAY_POWER:
		/* S10 port: iOS's display power request. Boot 16: backboardd sends
		 * selector 25 then 12 with 0 when the display goes off (power key,
		 * auto-lock) and 1 when it comes on. Still answered Unsupported, as
		 * before, so iOS behaves as it did; the panel now follows it. */
		if (args->scalarInputCount == 1 && args->scalarInput != NULL) {
			b2l_fb_display_power(args->scalarInput[0] != 0);
		}
		ret = kIOReturnUnsupported;
		break;
    default:
		ret = kIOReturnUnsupported;
		/* S10 port: up to 24 calls per unsupported scalar-only selector, with
		 * their values and time, to find iOS's display power request. */
		if (selector < 128 && args->structureInputSize == 0 && args->structureOutputSize == 0 &&
		    args->structureInputDescriptor == NULL && args->structureOutputDescriptor == NULL) {
			static uint32_t powerSamples[128];
			if (__atomic_load_n(&powerSamples[selector], __ATOMIC_RELAXED) < 24 &&
			    __atomic_fetch_add(&powerSamples[selector], 1, __ATOMIC_RELAXED) < 24) {
				uint64_t v[2] = { 0, 0 }, ns = 0;
				for (unsigned i = 0; args->scalarInput != NULL && i < args->scalarInputCount && i < 2; ++i)
					v[i] = args->scalarInput[i];
				absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
				b2l_diag_record("B2L FB SEL: pid=%d selector=%u n=%u values=0x%llx,0x%llx ms=%llu\n",
				    proc_selfpid(), selector, args->scalarInputCount, v[0], v[1], ns / 1000000);
			}
		}
		break;
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
