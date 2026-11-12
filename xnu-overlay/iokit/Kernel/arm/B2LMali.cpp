/* SPDX-License-Identifier: BSD-2-Clause
 * First stage of the Mali-G76 driver: prove from XNU that the GPU answers and
 * renders in its own address space. About 30 s after boot one thread reads the
 * power domain, identifies the GPU, resets it, powers its cores, clears a
 * 1440x2904 buffer with a fragment job, then draws a textured layer, a solid
 * rectangle and a translucent layer through tiler and fragment jobs, checks
 * every pixel, and powers the cores down again. Nothing else uses the GPU yet.
 *
 * The bootloader leaves the G3D domain on (the stock kernel logs it on at
 * probe). This stage only reads the PMU and stops if the domain is off: no
 * PMU, clock, voltage or secure-monitor request is made. The GPU therefore
 * runs at whatever clock the boot chain left, which is measured and logged.
 * Interrupts stay masked and every wait is bounded. Boot argument b2l_gpu=0
 * skips the test.
 *
 * After the test the service stays up for one user client, backboardd's
 * translation layer. The client's first request powers the GPU and builds its
 * address space; closing powers it down. Buffers are cached kernel memory the
 * client maps; surfaces and client memory are mapped on demand into fixed
 * 32 MiB slots, oldest out first, and stay held while mapped. Before a job the
 * CPU cleans the pass, every source whose seed moved, and the target. Jobs are
 * polled. The cores power down 50 ms after the last job.
 *
 * ACPM sets the GPU clock: the sustained level while jobs run, the boot level
 * once idle, and the boot level again if the GPU sensor reaches its limit. ACPM
 * picks the voltage. Without a working sensor the target is lower still.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <pexpert/pexpert.h>
#include <arm64/b2l_diag.h>
#include "b2l_mali_core.h"
#include "b2l_mali_draw.h"
#include "b2l_mali_uapi.h"
#include "b2l_acpm.h"
#include "B2LSurface.h"

extern "C" void b2l_task_dump(const char *name);
extern "C" void b2l_cpu_clock_measure(void);
extern "C" void b2l_cpu_load_sample(uint64_t *busy, uint64_t *idle);

/*
 * The boot chain's rates and the sustained ones. The targets are entries in the
 * device tree's tables that ask nothing of the memory clock, and big stops at the
 * rate Android boots with, the highest allowed without the overcurrent driver.
 */
static const struct {
	const char *name;
	uint32_t domain, boot_khz, boost_khz;
} b2l_cpu_clusters[] = {
	{ "cl0", B2L_DVFS_CPUCL0, 1053000, 1586000 },
	{ "cl1", B2L_DVFS_CPUCL1, 1222000, 2002000 },
	{ "cl2", B2L_DVFS_CPUCL2, 1378000, 2080000 },
};
#define B2L_CPU_CLUSTERS (sizeof(b2l_cpu_clusters) / sizeof(b2l_cpu_clusters[0]))
/* The big cluster under load, fastest first; the last is its boot rate. */
static const uint32_t b2l_big_steps[] = { 2080000, 1820000, 1560000, 1378000 };
#define B2L_BIG_STEPS (sizeof(b2l_big_steps) / sizeof(b2l_big_steps[0]))

extern "C" void flush_dcache64(addr64_t addr, unsigned count, int phys);
extern "C" int proc_selfpid(void);
extern "C" ppnum_t pmap_find_phys(pmap_t map, addr64_t va);
extern "C" pmap_t get_task_pmap(task_t task);

#define B2L_GPU_PMU_PAGE       0x15862000ull
#define B2L_GPU_PMU_EMBEDDED   0x104           /* EMBEDDED_G3D_STATUS */
#define B2L_GPU_PMU_G3D        0x384           /* G3D_STATUS */
#define B2L_GPU_REGS           0x18500000ull
#define B2L_GPU_REGS_BYTES     0x5000
#define B2L_GPU_START_DELAY_MS 30000
#define B2L_GPU_LOG_DRAIN_MS   1500            /* the persistent log drains each second */
#define B2L_GPU_BOOT_KHZ       156000          /* what the boot chain leaves */
#define B2L_GPU_WIDTH          1440
#define B2L_GPU_HEIGHT         2904
#define B2L_GPU_STRIDE         (B2L_GPU_WIDTH * 4)
#define B2L_GPU_RT_BYTES       (B2L_GPU_STRIDE * B2L_GPU_HEIGHT)
#define B2L_GPU_PT_TABLES      32
#define B2L_GPU_DRAWS          3
#define B2L_GPU_PASS_BYTES     ((B2L_MALI_PASS_BYTES(B2L_GPU_DRAWS) + 4095) & ~4095u)
#define B2L_GPU_MEM_BYTES      ((B2L_GPU_PT_TABLES + 1) * 4096 + B2L_GPU_PASS_BYTES + 4096)
#define B2L_GPU_PASS_OFFSET    ((B2L_GPU_PT_TABLES + 1) * 4096)
#define B2L_GPU_CODE_OFFSET    (B2L_GPU_PASS_OFFSET + B2L_GPU_PASS_BYTES)
#define B2L_GPU_HEAP_BYTES     (16u << 20)
#define B2L_GPU_TEX_SIZE       256
#define B2L_GPU_TEX_BYTES      (B2L_GPU_TEX_SIZE * B2L_GPU_TEX_SIZE * 4)
#define B2L_GPU_JOB_VA         0x10000000ull
#define B2L_GPU_RT_VA          0x20000000ull
#define B2L_GPU_PASS_VA        0x30000000ull
#define B2L_GPU_CODE_VA        0x30800000ull
#define B2L_GPU_TEX_VA         0x31000000ull
#define B2L_GPU_HEAP_VA        0x40000000ull
/* Shaders within the code page. */
#define B2L_GPU_CODE_TEXTURED  0x000
#define B2L_GPU_CODE_SOLID     0x100
#define B2L_GPU_CODE_MODULATED 0x200
#define B2L_GPU_RUNS           4
#define B2L_GPU_COLOUR         0xff2040c0u
#define B2L_GPU_SENTINEL       0x5aa55aa5u
#define B2L_GPU_ADDRESS_MASK   0x0000fffffffff000ull
/* Target is BGRA memory, as IOSurfaces: 0xAARRGGBB. */
#define B2L_GPU_PASS_CLEAR     0xff00ff00u     /* tile order: opaque green */
#define B2L_GPU_MEM_GREEN      0xff00ff00u
#define B2L_GPU_MEM_RED        0xffff0000u
#define B2L_GPU_MEM_HALF_WHITE 0xff80ff80u     /* 0.5 white over green */

#define B2L_RT_TABLES          1024
#define B2L_RT_HEAP_BYTES      (32u << 20)
#define B2L_RT_HEAP_VA         0x40000000ull
#define B2L_RT_BUFFER_VA       0x80000000ull
#define B2L_RT_BUFFER_STEP     0x04000000ull   /* 64 MiB per buffer */
#define B2L_RT_BUFFER_MAX      (32u << 20)
#define B2L_RT_SURFACE_VA      0x100000000ull
#define B2L_RT_SLOT_LOG2       25              /* 32 MiB */
#define B2L_RT_SLOTS           128
#define B2L_RT_MAPPED_LIMIT    (256ull << 20)
#define B2L_RT_JOB_TIMEOUT_US  1000000
#define B2L_RT_IDLE_MS         50
#define B2L_GPU_BOOST_KHZ      433000          /* the stock tree's sustainable level */
#define B2L_GPU_UNGUARDED_KHZ  325000          /* the most without a temperature */
#define B2L_GPU_MAX_KHZ        702000
#define B2L_GPU_HOT_C          85
#define B2L_GPU_COOL_C         75
#define B2L_CPU_BIG_FIRST_C    76              /* the big sensor: that cluster steps down from here */
#define B2L_CPU_BIG_PER_C      4               /* and one more step for each of these */
#define B2L_MIF_KHZ            2093000         /* Android's boot memory clock; 0 leaves it */
#define B2L_MIF_TEST_BYTES     (8u << 20)
#define B2L_CPU_ALL_HOT_C      88              /* any sensor: every cluster goes to its boot rate */
#define B2L_CPU_ALL_COOL_C     78
#define B2L_CPU_POLL_MS        250
#define B2L_CPU_LOAD_UP        15              /* percent of all CPUs: about one core busy */
#define B2L_CPU_LOAD_DOWN      8
#define B2L_CPU_QUIET_POLLS    12              /* 3 s under the lower mark */
#define B2L_CPU_TEMP_POLLS     8               /* polls per temperature reading */
#define B2L_CPU_REPORT_READS   8               /* readings per log line */
#define B2L_GPU_TEMP_EVERY_NS  2000000000ull
#define B2L_GPU_STATS_EVERY    5               /* temperature readings per log line */

struct b2l_mali_slot {
	B2LSurface *surface;            /* retained while mapped */
	IOMemoryDescriptor *range;      /* or wired client memory */
	uint64_t range_va, range_bytes;
	ppnum_t range_first, range_last;
	uint64_t last_use;
	uint32_t cleaned_seed, cleaned;
};

class B2LMali : public IOService {
	OSDeclareDefaultStructors(B2LMali);
	friend class B2LMaliUserClient;
	IOMemoryMap *regs;
	volatile uint32_t *base;
	IOBufferMemoryDescriptor *control, *target, *heap, *texture;
	struct b2l_mali_io io;
	struct b2l_mali_pt pt;

	static uint32_t ioRead(void *ctx, uint32_t offset);
	static void ioWrite(void *ctx, uint32_t offset, uint32_t value);
	static void ioDelay(void *ctx, uint32_t us);
	static void threadMain(void *owner, wait_result_t result);
	static void clusterMain(void *owner, wait_result_t result);
	static uint64_t nowNS();

	bool readPowerDomain();
	bool identify();
	bool allocate();
	bool buildAddressSpace();
	void measureClock();
	bool mapScattered(IOBufferMemoryDescriptor *memory, uint64_t va, IOByteCount bytes);
	void runTests();
	void runPass();
	bool checkPass(uint64_t tilerNS, uint64_t fragmentNS);
	void reportFault(unsigned run);
	void selfTest();
	void teardown();

	/* Runtime: everything below is guarded by runtimeLock. */
	IOLock *runtimeLock;
	bool selfTestPassed, runtimeReady, clientOpen, coresOn;
	thread_call_t idleCall;
	uint64_t lastJobNS;

	/* ACPM; also under runtimeLock. */
	IOMemoryMap *acpmMailbox, *acpmSram;
	volatile uint32_t *acpmMailboxBase, *acpmSramBase;
	struct b2l_acpm_io acpm;
	struct b2l_acpm_channel acpmDvfs, acpmTmu;
	bool acpmReady, tmuReady, thermalHold, clockChecked;
	uint32_t gpuBaseKHz, gpuBoostKHz, gpuRateKHz, temperatureReads;
	uint64_t lastTemperatureNS;
	static uint32_t acpmRead(void *ctx, uint32_t offset);
	static void acpmWrite(void *ctx, uint32_t offset, uint32_t value);
	static void acpmMailboxWrite(void *ctx, uint32_t offset, uint32_t value);
	void acpmDump();
	void readTemperatures(int *temperatures);
	void busyDump(unsigned second);
	bool setCluster(unsigned cluster, uint32_t khz);
	void memoryClock();
	void clusterBoost();
	void acpmStart();
	void gpuClock(bool busy);
	void checkTemperature();
	IOBufferMemoryDescriptor *runtimeTables, *runtimeHeap;
	IOBufferMemoryDescriptor *buffers[B2L_MALI_MAX_BUFFERS];
	struct b2l_mali_slot slots[B2L_RT_SLOTS];
	uint64_t useClock, mappedBytes, cleanNS, submits;

	bool runtimeBegin();
	void runtimeEnd();
	bool mapMemory(IOMemoryDescriptor *memory, uint64_t va, IOByteCount bytes, bool code);
	void flushTables();
	static void idleExpired(thread_call_param_t owner, thread_call_param_t unused);
	bool coresUp();
	void evictSlot(unsigned index);
	bool evictOldest();
	int slotFor(B2LSurface *surface);
	IOReturn createBuffer(uint64_t bytes, uint64_t flags, uint64_t *va, uint64_t *type);
	IOReturn mapSurface(uint32_t id, uint64_t *out);
	IOReturn mapRange(task_t task, uint64_t va, uint64_t bytes, uint64_t *gpu_va);
	static uint64_t lastJobTimeout();
	IOReturn submit(const struct b2l_mali_submit *request, uint64_t *out);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(B2LMali, IOService);

class B2LMaliUserClient : public IOUserClient {
	OSDeclareDefaultStructors(B2LMaliUserClient);
	B2LMali *owner;
	task_t clientTask;
	bool opened;
public:
	bool initWithTask(task_t task, void *security, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
	    IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(B2LMaliUserClient, IOUserClient);

uint32_t
B2LMali::ioRead(void *ctx, uint32_t offset)
{
	return ((B2LMali *)ctx)->base[offset / 4];
}

void
B2LMali::ioWrite(void *ctx, uint32_t offset, uint32_t value)
{
	((B2LMali *)ctx)->base[offset / 4] = value;
	__builtin_arm_dsb(15);
}

void
B2LMali::ioDelay(void *ctx __unused, uint32_t us)
{
	IODelay(us);
}

uint64_t
B2LMali::nowNS()
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

/* Read-only: a powered-down block must not be touched. */
bool
B2LMali::readPowerDomain()
{
	IOMemoryDescriptor *d = IOMemoryDescriptor::withPhysicalAddress(B2L_GPU_PMU_PAGE, 4096, kIODirectionIn);
	IOMemoryMap *m = d ? d->map(kIOMapInhibitCache) : NULL;
	OSSafeReleaseNULL(d);
	if (!m) {
		b2l_diag_record("B2L GPU: PMU map failed\n");
		return false;
	}
	volatile const uint32_t *pmu = (volatile const uint32_t *)m->getVirtualAddress();
	uint32_t embedded = pmu[B2L_GPU_PMU_EMBEDDED / 4], g3d = pmu[B2L_GPU_PMU_G3D / 4];
	OSSafeReleaseNULL(m);
	b2l_diag_record("B2L GPU: pmu embedded_g3d=0x%x g3d=0x%x\n", embedded, g3d);
	if (!(embedded & 1)) {
		b2l_diag_record("B2L GPU: power domain off, stopping\n");
		return false;
	}
	return true;
}

bool
B2LMali::identify()
{
	IOMemoryDescriptor *d = IOMemoryDescriptor::withPhysicalAddress(B2L_GPU_REGS, B2L_GPU_REGS_BYTES, kIODirectionInOut);
	regs = d ? d->map(kIOMapInhibitCache) : NULL;
	OSSafeReleaseNULL(d);
	if (!regs) {
		b2l_diag_record("B2L GPU: register map failed\n");
		return false;
	}
	base = (volatile uint32_t *)regs->getVirtualAddress();
	io = { this, ioRead, ioWrite, ioDelay };

	/* A stall on this read means the block is unclocked: date it in the log. */
	b2l_diag_record("B2L GPU: reading GPU_ID\n");
	IOSleep(B2L_GPU_LOG_DRAIN_MS);
	uint32_t id = b2l_mali_r(&io, B2L_MALI_ID);
	b2l_diag_record("B2L GPU: id=0x%x status=0x%x l2=0x%x core=0x%x tiler=0x%x mem=0x%x mmu=0x%x\n",
	    id, b2l_mali_r(&io, B2L_MALI_GPU_STATUS), b2l_mali_r(&io, B2L_MALI_L2_FEATURES),
	    b2l_mali_r(&io, B2L_MALI_CORE_FEATURES), b2l_mali_r(&io, B2L_MALI_TILER_FEATURES),
	    b2l_mali_r(&io, B2L_MALI_MEM_FEATURES), b2l_mali_r(&io, B2L_MALI_MMU_FEATURES));
	b2l_diag_record("B2L GPU: as=0x%x js=0x%x shader=0x%x tiler=0x%x l2=0x%x coherency=0x%x\n",
	    b2l_mali_r(&io, B2L_MALI_AS_PRESENT), b2l_mali_r(&io, B2L_MALI_JS_PRESENT),
	    b2l_mali_r(&io, B2L_MALI_SHADER_PRESENT), b2l_mali_r(&io, B2L_MALI_TILER_PRESENT),
	    b2l_mali_r(&io, B2L_MALI_L2_PRESENT), b2l_mali_r(&io, B2L_MALI_COHERENCY));
	if (id != B2L_MALI_GPU_ID) {
		b2l_diag_record("B2L GPU: unexpected id, stopping\n");
		return false;
	}
	return true;
}

bool
B2LMali::allocate()
{
	control = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIOMemoryPhysicallyContiguous | kIODirectionInOut, B2L_GPU_MEM_BYTES, B2L_GPU_ADDRESS_MASK);
	target = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut,
	    B2L_GPU_RT_BYTES, 4096);
	heap = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut,
	    B2L_GPU_HEAP_BYTES, 4096);
	texture = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut,
	    2 * B2L_GPU_TEX_BYTES, 4096);
	if (!control || !target || !heap || !texture) {
		b2l_diag_record("B2L GPU: allocation failed\n");
		return false;
	}
	return true;
}

bool
B2LMali::mapScattered(IOBufferMemoryDescriptor *memory, uint64_t va, IOByteCount bytes)
{
	for (IOByteCount off = 0; off < bytes; off += 4096) {
		IOByteCount length = 0;
		addr64_t pa = memory->getPhysicalSegment(off, &length, kIOMemoryMapperNone);
		if (!pa || !b2l_mali_pt_map(&pt, va + off, pa)) {
			b2l_diag_record("B2L GPU: mapping 0x%llx failed\n", va + off);
			return false;
		}
	}
	return true;
}

/* `control` holds the page tables, job, pass and shaders, physically contiguous. */
bool
B2LMali::buildAddressSpace()
{
	IOByteCount length = 0;
	addr64_t tables_pa = control->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
	if (!tables_pa || length < B2L_GPU_MEM_BYTES ||
	    !b2l_mali_pt_init(&pt, (uint64_t *)control->getBytesNoCopy(), tables_pa, B2L_GPU_PT_TABLES) ||
	    !b2l_mali_pt_map(&pt, B2L_GPU_JOB_VA, tables_pa + B2L_GPU_PT_TABLES * 4096)) {
		b2l_diag_record("B2L GPU: address space setup failed\n");
		return false;
	}
	for (uint32_t off = 0; off < B2L_GPU_PASS_BYTES; off += 4096) {
		if (!b2l_mali_pt_map(&pt, B2L_GPU_PASS_VA + off, tables_pa + B2L_GPU_PASS_OFFSET + off)) {
			b2l_diag_record("B2L GPU: mapping pass failed\n");
			return false;
		}
	}
	if (!b2l_mali_pt_map_code(&pt, B2L_GPU_CODE_VA, tables_pa + B2L_GPU_CODE_OFFSET)) {
		b2l_diag_record("B2L GPU: mapping shaders failed\n");
		return false;
	}
	if (!mapScattered(target, B2L_GPU_RT_VA, B2L_GPU_RT_BYTES) ||
	    !mapScattered(texture, B2L_GPU_TEX_VA, 2 * B2L_GPU_TEX_BYTES) ||
	    !mapScattered(heap, B2L_GPU_HEAP_VA, B2L_GPU_HEAP_BYTES)) {
		return false;
	}
	b2l_diag_record("B2L GPU: address space built, %u of %u tables\n", pt.used, pt.capacity);
	flush_dcache64((addr64_t)control->getBytesNoCopy(), B2L_GPU_MEM_BYTES, FALSE);
	return true;
}

void
B2LMali::measureClock()
{
	b2l_mali_w(&io, B2L_MALI_GPU_COMMAND, B2L_MALI_CMD_CYCLES_START);
	uint32_t first = b2l_mali_r(&io, B2L_MALI_CYCLE_COUNT_LO);
	uint64_t begin = nowNS();
	IODelay(10000);
	uint32_t cycles = b2l_mali_r(&io, B2L_MALI_CYCLE_COUNT_LO) - first;
	uint64_t elapsed = nowNS() - begin;
	b2l_mali_w(&io, B2L_MALI_GPU_COMMAND, B2L_MALI_CMD_CYCLES_STOP);
	b2l_diag_record("B2L GPU: clock %llu kHz (%u cycles in %llu ns)\n",
	    elapsed ? (uint64_t)cycles * 1000000ull / elapsed : 0, cycles, elapsed);
}

void
B2LMali::reportFault(unsigned run)
{
	b2l_diag_record("B2L GPU: run=%u js_status=0x%x job_irq=0x%x mmu_irq=0x%x as_fault=0x%x addr=0x%x%08x gpu_fault=0x%x\n",
	    run, b2l_mali_r(&io, B2L_MALI_JS0 + B2L_MALI_JS_STATUS), b2l_mali_r(&io, B2L_MALI_JOB_IRQ_RAWSTAT),
	    b2l_mali_r(&io, B2L_MALI_MMU_IRQ_RAWSTAT), b2l_mali_r(&io, B2L_MALI_AS0 + B2L_MALI_AS_FAULTSTATUS),
	    b2l_mali_r(&io, B2L_MALI_AS0 + B2L_MALI_AS_FAULTADDRESS_HI),
	    b2l_mali_r(&io, B2L_MALI_AS0 + B2L_MALI_AS_FAULTADDRESS_LO),
	    b2l_mali_r(&io, B2L_MALI_GPU_FAULTSTATUS));
}

void
B2LMali::runTests()
{
	uint8_t *job = (uint8_t *)control->getBytesNoCopy() + B2L_GPU_PT_TABLES * 4096;
	uint32_t *pixels = (uint32_t *)target->getBytesNoCopy();
	const size_t count = B2L_GPU_RT_BYTES / 4;

	for (unsigned run = 0; run < B2L_GPU_RUNS; run++) {
		for (size_t i = 0; i < count; i++) {
			pixels[i] = B2L_GPU_SENTINEL;
		}
		b2l_mali_clear_job(job, B2L_GPU_JOB_VA, B2L_GPU_RT_VA, B2L_GPU_WIDTH, B2L_GPU_HEIGHT,
		    B2L_GPU_STRIDE, B2L_GPU_COLOUR);
		flush_dcache64((addr64_t)job, B2L_MALI_JOB_BYTES, FALSE);
		flush_dcache64((addr64_t)pixels, B2L_GPU_RT_BYTES, FALSE);

		uint64_t begin = nowNS();
		uint32_t status = b2l_mali_run_job(&io, B2L_MALI_SLOT_FRAGMENT, B2L_GPU_JOB_VA, 2000000);
		uint64_t elapsed = nowNS() - begin;
		if (status != 1) {
			reportFault(run);
			return;
		}
		flush_dcache64((addr64_t)pixels, B2L_GPU_RT_BYTES, FALSE);
		size_t bad = 0;
		for (size_t i = 0; i < count; i++) {
			bad += pixels[i] != B2L_GPU_COLOUR;
		}
		b2l_diag_record("B2L GPU: run=%u job done ns=%llu bad_pixels=%llu first=0x%x\n",
		    run, elapsed, (uint64_t)bad, pixels[0]);
		if (bad) {
			return;
		}
	}
	runPass();
}

/* Every pixel of the frame runPass draws, in BGRA memory order. */
bool
B2LMali::checkPass(uint64_t tilerNS, uint64_t fragmentNS)
{
	const uint32_t *pixels = (const uint32_t *)target->getBytesNoCopy();
	size_t bad = 0;
	uint32_t got = 0, want = 0, badX = 0, badY = 0;

	flush_dcache64((addr64_t)pixels, B2L_GPU_RT_BYTES, FALSE);
	for (uint32_t y = 0; y < B2L_GPU_HEIGHT; y++) {
		for (uint32_t x = 0; x < B2L_GPU_WIDTH; x++) {
			uint32_t expect = B2L_GPU_MEM_GREEN;
			if (x >= 100 && x < 356 && y >= 200 && y < 456) {
				expect = 0xff000055u | ((x - 100) << 16) | ((y - 200) << 8);
			} else if (x >= 400 && x < 500 && y >= 800 && y < 900) {
				expect = B2L_GPU_MEM_RED;
			} else if (x >= 600 && x < 856 && y >= 1000 && y < 1256) {
				expect = B2L_GPU_MEM_HALF_WHITE;
			}
			uint32_t value = pixels[y * B2L_GPU_WIDTH + x], difference = 0;
			for (unsigned shift = 0; shift < 32; shift += 8) {
				uint32_t a = (value >> shift) & 255, b = (expect >> shift) & 255;
				difference |= (a > b ? a - b : b - a) > 2;
			}
			if (difference && !bad++) {
				got = value; want = expect; badX = x; badY = y;
			}
		}
	}
	b2l_diag_record("B2L GPU: pass tiler_ns=%llu fragment_ns=%llu bad_pixels=%llu\n",
	    tilerNS, fragmentNS, (uint64_t)bad);
	if (bad) {
		b2l_diag_record("B2L GPU: first bad x=%u y=%u got=0x%x want=0x%x\n", badX, badY, got, want);
	}
	return !bad;
}

/*
 * The UI's three kinds of layer in one pass: a textured BGRA copy, a solid
 * rectangle and a translucent layer, on a BGRA target.
 */
void
B2LMali::runPass()
{
	uint8_t *memory = (uint8_t *)control->getBytesNoCopy();
	uint8_t *code = memory + B2L_GPU_CODE_OFFSET;
	uint32_t *ramp = (uint32_t *)texture->getBytesNoCopy();
	uint32_t *white = ramp + B2L_GPU_TEX_BYTES / 4;
	uint32_t *pixels = (uint32_t *)target->getBytesNoCopy();
	const struct b2l_mali_target rt = { B2L_GPU_RT_VA, B2L_GPU_WIDTH, B2L_GPU_HEIGHT, B2L_GPU_STRIDE,
		B2L_GPU_PASS_CLEAR, B2L_RT_BGRA, B2L_GPU_HEAP_VA, B2L_GPU_HEAP_BYTES };
	const uint32_t one = B2L_FLOAT_ONE;
	struct b2l_mali_pass pass;

	bcopy(b2l_mali_shader_textured, code + B2L_GPU_CODE_TEXTURED, sizeof(b2l_mali_shader_textured));
	bcopy(b2l_mali_shader_solid, code + B2L_GPU_CODE_SOLID, sizeof(b2l_mali_shader_solid));
	bcopy(b2l_mali_shader_modulated, code + B2L_GPU_CODE_MODULATED, sizeof(b2l_mali_shader_modulated));
	for (uint32_t y = 0; y < B2L_GPU_TEX_SIZE; y++) {
		for (uint32_t x = 0; x < B2L_GPU_TEX_SIZE; x++) {
			ramp[y * B2L_GPU_TEX_SIZE + x] = 0xff000055u | (x << 16) | (y << 8);
			white[y * B2L_GPU_TEX_SIZE + x] = 0xffffffffu;
		}
	}
	for (size_t i = 0; i < B2L_GPU_RT_BYTES / 4; i++) {
		pixels[i] = B2L_GPU_SENTINEL;
	}

	b2l_mali_pass_begin(&pass, memory + B2L_GPU_PASS_OFFSET, B2L_GPU_PASS_VA, &rt);
	const struct b2l_mali_quad copy = {
		b2l_int_float(100), b2l_int_float(200), b2l_int_float(356), b2l_int_float(456),
		0, 0, one, one, { 0, 0, 0, 0 }, B2L_GPU_CODE_VA + B2L_GPU_CODE_TEXTURED, B2L_GPU_TEX_VA,
		B2L_GPU_TEX_SIZE, B2L_GPU_TEX_SIZE, B2L_GPU_TEX_SIZE * 4, B2L_FORMAT_BGRA8_UNORM,
		B2L_EQ_COPY, 0, 1, 0,
	};
	const struct b2l_mali_quad solid = {
		b2l_int_float(400), b2l_int_float(800), b2l_int_float(500), b2l_int_float(900),
		0, 0, one, one, { one, 0, 0, one }, B2L_GPU_CODE_VA + B2L_GPU_CODE_SOLID, 0, 0, 0, 0, 0,
		B2L_EQ_COPY, 2, 0, 0,
	};
	const struct b2l_mali_quad layer = {
		b2l_int_float(600), b2l_int_float(1000), b2l_int_float(856), b2l_int_float(1256),
		0, 0, one, one, { B2L_FLOAT_HALF, 0, 0, 0 }, B2L_GPU_CODE_VA + B2L_GPU_CODE_MODULATED,
		B2L_GPU_TEX_VA + B2L_GPU_TEX_BYTES, B2L_GPU_TEX_SIZE, B2L_GPU_TEX_SIZE,
		B2L_GPU_TEX_SIZE * 4, B2L_FORMAT_BGRA8_UNORM, B2L_EQ_SRC_OVER, 1, 0, 0,
	};
	b2l_mali_pass_add(&pass, &copy);
	b2l_mali_pass_add(&pass, &solid);
	b2l_mali_pass_add(&pass, &layer);

	flush_dcache64((addr64_t)(memory + B2L_GPU_PASS_OFFSET), B2L_GPU_PASS_BYTES + 4096, FALSE);
	flush_dcache64((addr64_t)ramp, 2 * B2L_GPU_TEX_BYTES, FALSE);
	flush_dcache64((addr64_t)pixels, B2L_GPU_RT_BYTES, FALSE);

	uint64_t begin = nowNS();
	uint32_t status = b2l_mali_run_job(&io, B2L_MALI_SLOT_TILER,
	    B2L_GPU_PASS_VA + B2L_MALI_PASS_DRAWS, 2000000);
	uint64_t tilerNS = nowNS() - begin;
	if (status != 1) {
		b2l_diag_record("B2L GPU: tiler job status=0x%x\n", status);
		reportFault(0);
		return;
	}
	begin = nowNS();
	status = b2l_mali_run_job(&io, B2L_MALI_SLOT_FRAGMENT,
	    B2L_GPU_PASS_VA + B2L_MALI_PASS_FRAGMENT, 2000000);
	uint64_t fragmentNS = nowNS() - begin;
	if (status != 1) {
		b2l_diag_record("B2L GPU: fragment job status=0x%x\n", status);
		reportFault(0);
		return;
	}
	if (checkPass(tilerNS, fragmentNS)) {
		selfTestPassed = true;
		b2l_diag_record("B2L GPU: self test passed\n");
	}
}

void
B2LMali::selfTest()
{
	if (!readPowerDomain() || !identify()) {
		return;
	}
	if (!allocate() || !buildAddressSpace()) {
		return;
	}
	if (!b2l_mali_soft_reset(&io)) {
		b2l_diag_record("B2L GPU: soft reset timed out\n");
		return;
	}
	const char *failed = b2l_mali_power_on(&io);
	if (failed) {
		b2l_diag_record("B2L GPU: %s power-on timed out\n", failed);
		b2l_mali_power_off(&io);
		return;
	}
	b2l_diag_record("B2L GPU: cores ready shader=0x%x\n", b2l_mali_r(&io, B2L_MALI_SHADER_READY));
	measureClock();
	if (b2l_mali_set_as0(&io, pt.tables_pa)) {
		runTests();
	} else {
		b2l_diag_record("B2L GPU: address space update timed out\n");
	}
	b2l_mali_power_off(&io);
}

void
B2LMali::teardown()
{
	OSSafeReleaseNULL(control);
	OSSafeReleaseNULL(target);
	OSSafeReleaseNULL(heap);
	OSSafeReleaseNULL(texture);
}

void
B2LMali::threadMain(void *owner, wait_result_t result __unused)
{
	B2LMali *self = (B2LMali *)owner;

	IOSleep(B2L_GPU_START_DELAY_MS);
	self->selfTest();
	self->teardown();
	if (self->selfTestPassed) {
		IOLockLock(self->runtimeLock);
		self->runtimeReady = true;
		IOLockUnlock(self->runtimeLock);
		self->setProperty(kIOUserClientClassKey, "B2LMaliUserClient");
		self->registerService();
	}
	b2l_diag_record("B2L GPU: stage finished\n");
	self->release();
	thread_terminate(current_thread());
}

void
B2LMali::flushTables()
{
	flush_dcache64((addr64_t)runtimeTables->getBytesNoCopy(), pt.used * 4096, FALSE);
}

bool
B2LMali::runtimeBegin()
{
	IOByteCount length = 0;

	runtimeTables = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIOMemoryPhysicallyContiguous | kIODirectionInOut, B2L_RT_TABLES * 4096, B2L_GPU_ADDRESS_MASK);
	runtimeHeap = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut,
	    B2L_RT_HEAP_BYTES, 4096);
	if (runtimeTables == NULL || runtimeHeap == NULL) {
		return false;
	}
	addr64_t tables_pa = runtimeTables->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
	if (!tables_pa || length < B2L_RT_TABLES * 4096 ||
	    !b2l_mali_pt_init(&pt, (uint64_t *)runtimeTables->getBytesNoCopy(), tables_pa, B2L_RT_TABLES) ||
	    !mapMemory(runtimeHeap, B2L_RT_HEAP_VA, B2L_RT_HEAP_BYTES, false)) {
		return false;
	}
	flushTables();
	if (!b2l_mali_soft_reset(&io)) {
		return false;
	}
	coresOn = false;
	if (idleCall == NULL) {
		idleCall = thread_call_allocate(&B2LMali::idleExpired, this);
	}
	if (acpmMailbox == NULL) {
		acpmStart();
	}
	return idleCall != NULL;
}

uint32_t
B2LMali::acpmRead(void *ctx, uint32_t offset)
{
	return ((B2LMali *)ctx)->acpmSramBase[offset / 4];
}

void
B2LMali::acpmWrite(void *ctx, uint32_t offset, uint32_t value)
{
	((B2LMali *)ctx)->acpmSramBase[offset / 4] = value;
	__builtin_arm_dsb(15);
}

void
B2LMali::acpmMailboxWrite(void *ctx, uint32_t offset, uint32_t value)
{
	((B2LMali *)ctx)->acpmMailboxBase[offset / 4] = value;
	__builtin_arm_dsb(15);
}

/*
 * The firmware's description of the channels in use, as words, for offline
 * reading. Not all of the SRAM can be read: walking every channel and following
 * its offsets froze the bus in 0133. So this reads only these, and each step
 * reaches the log before the next, to date any stall.
 */
void
B2LMali::acpmDump()
{
	const uint32_t fw = B2L_ACPM_INITDATA;
	uint32_t table = acpmRead(this, fw + B2L_ACPM_FW_CHANNELS);
	static const uint32_t channels[] = { B2L_ACPM_CHANNEL_DVFS, 1, B2L_ACPM_CHANNEL_TMU };

	b2l_diag_record("B2L ACPM FW: plugins %x/%u channels %x/%u ap_max %u\n", acpmRead(this, fw), acpmRead(this, fw + 4),
	    table, acpmRead(this, fw + 12), acpmRead(this, fw + B2L_ACPM_FW_AP_MAX));
	IOSleep(B2L_GPU_LOG_DRAIN_MS);
	if (table >= B2L_ACPM_SRAM_BYTES - 16 * B2L_ACPM_CHANNEL_BYTES) {
		return;
	}
	for (unsigned n = 0; n < sizeof(channels) / sizeof(channels[0]); n++) {
		uint32_t at = table + channels[n] * B2L_ACPM_CHANNEL_BYTES;
		for (uint32_t i = 0; i < 18; i += 6) {
			b2l_diag_record("B2L ACPM CH %u +%02x: %x %x %x %x %x %x\n", channels[n], i * 4, acpmRead(this, at + i * 4),
			    acpmRead(this, at + (i + 1) * 4), acpmRead(this, at + (i + 2) * 4), acpmRead(this, at + (i + 3) * 4),
			    acpmRead(this, at + (i + 4) * 4), acpmRead(this, at + (i + 5) * 4));
		}
		IOSleep(B2L_GPU_LOG_DRAIN_MS);
	}
}

/* Who keeps IOKit from going quiet, and where the daemons behind Settings are. */
void
B2LMali::busyDump(unsigned second)
{
	IOService *root = IOService::getServiceRoot();
	unsigned listed = 0;

	b2l_diag_record("B2L BUSY: sec=%u root=%u\n", second, root ? root->getBusyState() : 0);
	IORegistryIterator *services = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively);
	while (services != NULL && listed < 24) {
		IOService *service = OSDynamicCast(IOService, services->getNextObject());
		if (service == NULL) {
			if (services->isValid()) {
				break;
			}
			services->reset();
			continue;
		}
		if (service->getBusyState() != 0) {
			b2l_diag_record("B2L BUSY: %s (%s) %u\n", service->getName(), service->getMetaClass()->getClassName(),
			    service->getBusyState());
			listed++;
		}
	}
	OSSafeReleaseNULL(services);
	b2l_task_dump("configd");
	b2l_task_dump("timed");
	IOSleep(B2L_GPU_LOG_DRAIN_MS);
}

bool
B2LMali::setCluster(unsigned cluster, uint32_t khz)
{
	IOLockLock(runtimeLock);
	int answered = acpmReady && b2l_acpm_set_rate(&acpm, &acpmDvfs, b2l_cpu_clusters[cluster].domain, khz);
	IOLockUnlock(runtimeLock);
	if (!answered) {
		b2l_diag_record("B2L ACPM: %s %u kHz got no reply\n", b2l_cpu_clusters[cluster].name, khz);
	}
	return answered;
}

/* Bytes per second a kernel memcpy reaches; the best of three 8 MiB runs. */
static uint64_t
b2l_copy_bandwidth(void *from, void *to)
{
	uint64_t best = 0;

	for (int run = 0; run < 3; run++) {
		uint64_t began = mach_absolute_time();
		memcpy(to, from, B2L_MIF_TEST_BYTES);
		uint64_t ns = 0;
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		uint64_t rate = ns ? (uint64_t)B2L_MIF_TEST_BYTES * 2 * 1000000000ull / ns : 0;
		best = rate > best ? rate : best;
	}
	return best;
}

/*
 * The memory clock: measure a copy, ask ACPM for Android's boot rate, measure
 * again. The two figures say whether the request did anything, since a rate read
 * returns 0. The display copy ran at about 5 GB/s in 0144, far under LPDDR4X.
 */
void
B2LMali::memoryClock()
{
	int khz = B2L_MIF_KHZ;
	PE_parse_boot_argn("b2l_mif_khz", &khz, sizeof(khz));
	if (khz <= 0) {
		b2l_diag_record("B2L ACPM: memory clock left alone\n");
		return;
	}
	void *from = IOMalloc(B2L_MIF_TEST_BYTES), *to = IOMalloc(B2L_MIF_TEST_BYTES);
	if (from == NULL || to == NULL) {
		if (from) IOFree(from, B2L_MIF_TEST_BYTES);
		if (to) IOFree(to, B2L_MIF_TEST_BYTES);
		return;
	}
	memset(from, 0x5a, B2L_MIF_TEST_BYTES);
	uint64_t before = b2l_copy_bandwidth(from, to);
	b2l_diag_record("B2L ACPM: copy %llu MB/s before, asking mif %d kHz\n", before >> 20, khz);
	IOSleep(B2L_GPU_LOG_DRAIN_MS);
	IOLockLock(runtimeLock);
	int answered = acpmReady && b2l_acpm_set_rate(&acpm, &acpmDvfs, B2L_DVFS_MIF, (uint32_t)khz);
	IOLockUnlock(runtimeLock);
	uint64_t after = b2l_copy_bandwidth(from, to);
	b2l_diag_record("B2L ACPM: mif reply=%d, copy %llu MB/s after\n", answered, after >> 20);
	IOFree(from, B2L_MIF_TEST_BYTES);
	IOFree(to, B2L_MIF_TEST_BYTES);
}

/*
 * The CPUs run fast only under load: left high, an idle phone reached 80 C in 0139.
 * Under load the big cores heat far faster than the rest, so they alone step down
 * when hot. Any sensor past the higher mark stops the boost until it cools, and a
 * lost sensor ends it.
 */
void
B2LMali::clusterBoost()
{
	static const unsigned dumps[] = { 60, 120 };
	int enabled = 1;
	PE_parse_boot_argn("b2l_cpu_boost", &enabled, sizeof(enabled));
	IOLockLock(runtimeLock);
	bool ready = acpmReady && tmuReady;
	IOLockUnlock(runtimeLock);
	if (!ready) {
		b2l_diag_record("B2L ACPM: CPU clocks left alone, no sensors\n");
		return;
	}
	memoryClock();
	struct b2l_acpm_load load = { 0, 0 };
	unsigned bigStep = 0;
	int holding = 0;
	bool measured = false;
	unsigned dumped = 0, changes = 0, reads = 0, percent = 0;
	uint32_t rates[B2L_CPU_CLUSTERS];

	for (unsigned i = 0; i < B2L_CPU_CLUSTERS; i++) {
		rates[i] = b2l_cpu_clusters[i].boot_khz;
	}
	uint64_t busy = 0, idle = 0;

	b2l_cpu_load_sample(&busy, &idle);
	for (unsigned poll = 1;; poll++) {
		IOSleep(B2L_CPU_POLL_MS);
		if (dumped < sizeof(dumps) / sizeof(dumps[0]) && nowNS() >= dumps[dumped] * 1000000000ull) {
			busyDump(dumps[dumped++]);
		}
		uint64_t nowBusy, nowIdle;
		b2l_cpu_load_sample(&nowBusy, &nowIdle);
		uint64_t total = (nowBusy - busy) + (nowIdle - idle);
		if (total != 0) {
			percent = (unsigned)((nowBusy - busy) * 100 / total);
			b2l_acpm_load_step(&load, percent, B2L_CPU_LOAD_UP, B2L_CPU_LOAD_DOWN, B2L_CPU_QUIET_POLLS);
			busy = nowBusy;
			idle = nowIdle;
		}
		if (poll % B2L_CPU_TEMP_POLLS == 0) {
			int temperatures[B2L_TMU_COUNT], hottest = -1, lost = 0;
			IOLockLock(runtimeLock);
			readTemperatures(temperatures);
			IOLockUnlock(runtimeLock);
			for (unsigned i = 0; i < B2L_TMU_COUNT; i++) {
				lost |= temperatures[i] < 0;
				hottest = temperatures[i] > hottest ? temperatures[i] : hottest;
			}
			if (lost) {
				b2l_diag_record("B2L ACPM: sensor lost, CPUs back at boot clocks\n");
				for (unsigned i = 0; i < B2L_CPU_CLUSTERS; i++) {
					setCluster(i, b2l_cpu_clusters[i].boot_khz);
				}
				return;
			}
			bigStep = b2l_acpm_big_step(bigStep, temperatures[B2L_TMU_BIG], B2L_CPU_BIG_FIRST_C, B2L_CPU_BIG_PER_C, B2L_BIG_STEPS);
			int hold = b2l_acpm_thermal_hold(holding, hottest, B2L_CPU_ALL_HOT_C, B2L_CPU_ALL_COOL_C);
			if (hold != holding) {
				b2l_diag_record("B2L ACPM: hottest %d C, CPUs %s\n", hottest, hold ? "held at boot clocks" : "released");
				holding = hold;
			}
			if (++reads % B2L_CPU_REPORT_READS == 0) {
				b2l_diag_record("B2L ACPM: cpu big %d mid %d little %d g3d %d C load %u%% big %u kHz changes=%u holding=%d\n",
				    temperatures[B2L_TMU_BIG], temperatures[B2L_TMU_MID], temperatures[B2L_TMU_LITTLE],
				    temperatures[B2L_TMU_G3D], percent, rates[B2L_CPU_CLUSTERS - 1], changes, holding);
			}
		}
		bool raised = false;
		for (unsigned i = 0; i < B2L_CPU_CLUSTERS; i++) {
			uint32_t want = b2l_cpu_clusters[i].boot_khz;
			if (enabled && load.boosted && !holding) {
				want = i == B2L_CPU_CLUSTERS - 1 ? b2l_big_steps[bigStep] : b2l_cpu_clusters[i].boost_khz;
			}
			if (want != rates[i]) {
				if (!setCluster(i, want)) {
					return;
				}
				raised |= want == b2l_cpu_clusters[i].boost_khz;
				rates[i] = want;
				changes++;
			}
		}
		if (raised && !measured) {
			measured = true;
			b2l_cpu_clock_measure();
		}
	}
}

void
B2LMali::clusterMain(void *owner, wait_result_t result __unused)
{
	B2LMali *self = (B2LMali *)owner;

	self->clusterBoost();
	self->release();
	thread_terminate(current_thread());
}

void
B2LMali::readTemperatures(int *temperatures)
{
	for (unsigned i = 0; i < B2L_TMU_COUNT; i++) {
		temperatures[i] = b2l_acpm_read_temp(&acpm, &acpmTmu, i);
	}
}

/* Find the channels and log what the firmware reports. No clock is changed. */
void
B2LMali::acpmStart()
{
	int requested = B2L_GPU_BOOST_KHZ;
	PE_parse_boot_argn("b2l_gpu_khz", &requested, sizeof(requested));
	if (requested <= 0 || requested > B2L_GPU_MAX_KHZ) {
		b2l_diag_record("B2L ACPM: not used (b2l_gpu_khz=%d)\n", requested);
		return;
	}
	IOMemoryDescriptor *d = IOMemoryDescriptor::withPhysicalAddress(B2L_ACPM_MAILBOX, B2L_ACPM_MAILBOX_BYTES, kIODirectionInOut);
	acpmMailbox = d ? d->map(kIOMapInhibitCache) : NULL;
	OSSafeReleaseNULL(d);
	d = IOMemoryDescriptor::withPhysicalAddress(B2L_ACPM_SRAM, B2L_ACPM_SRAM_BYTES, kIODirectionInOut);
	acpmSram = d ? d->map(kIOMapInhibitCache) : NULL;
	OSSafeReleaseNULL(d);
	if (acpmMailbox == NULL || acpmSram == NULL) {
		b2l_diag_record("B2L ACPM: map failed\n");
		return;
	}
	acpmMailboxBase = (volatile uint32_t *)acpmMailbox->getVirtualAddress();
	acpmSramBase = (volatile uint32_t *)acpmSram->getVirtualAddress();
	acpm = { this, acpmRead, acpmWrite, acpmMailboxWrite, ioDelay };

	b2l_diag_record("B2L ACPM: reading channel table\n");
	IOSleep(B2L_GPU_LOG_DRAIN_MS);
	acpmDump();
	int temperatures[B2L_TMU_COUNT] = { -1, -1, -1, -1, -1 };
	if (b2l_acpm_open(&acpm, B2L_ACPM_CHANNEL_TMU, &acpmTmu)) {
		readTemperatures(temperatures);
		if (temperatures[B2L_TMU_G3D] <= 0) {
			b2l_diag_record("B2L ACPM: tmu init reply=%d\n", b2l_acpm_tmu_init(&acpm, &acpmTmu));
			IOSleep(B2L_GPU_LOG_DRAIN_MS);
			readTemperatures(temperatures);
		}
	}
	tmuReady = temperatures[B2L_TMU_G3D] > 0 && temperatures[B2L_TMU_G3D] < 125;
	b2l_diag_record("B2L ACPM: temp big=%d mid=%d little=%d g3d=%d isp=%d guard=%d\n", temperatures[0], temperatures[1],
	    temperatures[2], temperatures[3], temperatures[4], tmuReady);
	IOSleep(B2L_GPU_LOG_DRAIN_MS);

	if (!b2l_acpm_open(&acpm, B2L_ACPM_CHANNEL_DVFS, &acpmDvfs)) {
		b2l_diag_record("B2L ACPM: no DVFS channel\n");
		return;
	}
	b2l_diag_record("B2L ACPM: dvfs channel len=%u elem=%u queue tx %u/%u rx %u/%u\n", acpmDvfs.len, acpmDvfs.elem,
	    acpmRead(this, acpmDvfs.tx_rear), acpmRead(this, acpmDvfs.tx_front), acpmRead(this, acpmDvfs.rx_rear),
	    acpmRead(this, acpmDvfs.rx_front));
	IOSleep(B2L_GPU_LOG_DRAIN_MS);
	static const char *const names[] = { "mif", "int", "cl0", "cl1", "cl2", "npu", "disp", "score", "aud", "cp", "g3d" };
	gpuBaseKHz = B2L_GPU_BOOT_KHZ;
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		uint32_t reply[4];
		int answered = b2l_acpm_get_rate(&acpm, &acpmDvfs, i, reply);
		b2l_diag_record("B2L ACPM: %s reply=%d %u kHz (%x %x %x)\n", names[i], answered, reply[1], reply[0], reply[2], reply[3]);
		if (answered && i == B2L_DVFS_G3D && reply[1] != 0) {
			gpuBaseKHz = reply[1];
		}
	}
	b2l_diag_record("B2L ACPM: dvfs queue tx %u/%u rx %u/%u\n", acpmRead(this, acpmDvfs.tx_rear), acpmRead(this, acpmDvfs.tx_front),
	    acpmRead(this, acpmDvfs.rx_rear), acpmRead(this, acpmDvfs.rx_front));
	IOSleep(B2L_GPU_LOG_DRAIN_MS);
	gpuRateKHz = gpuBaseKHz;
	gpuBoostKHz = (uint32_t)requested;
	if (!tmuReady && gpuBoostKHz > B2L_GPU_UNGUARDED_KHZ) {
		gpuBoostKHz = B2L_GPU_UNGUARDED_KHZ;
	}
	acpmReady = true;

	thread_t thread;
	retain();
	if (kernel_thread_start(&B2LMali::clusterMain, this, &thread) != KERN_SUCCESS) {
		release();
		return;
	}
	thread_deallocate(thread);
}

void
B2LMali::gpuClock(bool busy)
{
	if (!acpmReady) {
		return;
	}
	uint32_t want = busy && !thermalHold ? gpuBoostKHz : gpuBaseKHz;
	if (want == gpuRateKHz) {
		return;
	}
	if (!b2l_acpm_set_rate(&acpm, &acpmDvfs, B2L_DVFS_G3D, want)) {
		b2l_diag_record("B2L ACPM: set %u kHz got no reply, leaving ACPM alone\n", want);
		acpmReady = false;
		return;
	}
	gpuRateKHz = want;
}

/* Called between jobs. Over the limit, stay at the boot level until it cools. */
void
B2LMali::checkTemperature()
{
	uint64_t now = nowNS();

	if (!acpmReady || !tmuReady || now - lastTemperatureNS < B2L_GPU_TEMP_EVERY_NS) {
		return;
	}
	lastTemperatureNS = now;
	int g3d = b2l_acpm_read_temp(&acpm, &acpmTmu, B2L_TMU_G3D);
	if (g3d < 0) {
		gpuBoostKHz = gpuBoostKHz > B2L_GPU_UNGUARDED_KHZ ? B2L_GPU_UNGUARDED_KHZ : gpuBoostKHz;
		tmuReady = false;
		b2l_diag_record("B2L ACPM: sensor lost, capped at %u kHz\n", gpuBoostKHz);
		return;
	}
	if (!thermalHold && g3d >= B2L_GPU_HOT_C) {
		thermalHold = true;
		b2l_diag_record("B2L ACPM: g3d %d C, holding boot clock\n", g3d);
	} else if (thermalHold && g3d <= B2L_GPU_COOL_C) {
		thermalHold = false;
		b2l_diag_record("B2L ACPM: g3d %d C, clock released\n", g3d);
	}
	if (++temperatureReads % B2L_GPU_STATS_EVERY == 0) {
		b2l_diag_record("B2L ACPM: g3d %d C big %d C rate %u kHz clean_ms %llu\n", g3d,
		    b2l_acpm_read_temp(&acpm, &acpmTmu, B2L_TMU_BIG), gpuRateKHz, cleanNS / 1000000);
	}
	gpuClock(true);
}

bool
B2LMali::coresUp()
{
	if (coresOn) {
		return true;
	}
	if (b2l_mali_power_on(&io) != NULL || !b2l_mali_set_as0(&io, pt.tables_pa)) {
		b2l_mali_power_off(&io);
		return false;
	}
	coresOn = true;
	if (!clockChecked && acpmReady) {
		b2l_diag_record("B2L ACPM: first change, %u to %u kHz\n", gpuBaseKHz, gpuBoostKHz);
		IOSleep(B2L_GPU_LOG_DRAIN_MS);
	}
	uint64_t before = nowNS();
	gpuClock(true);
	if (!clockChecked && gpuRateKHz != gpuBaseKHz) {
		clockChecked = true;    /* trust the cycle counter, not the reply */
		b2l_diag_record("B2L ACPM: change took %llu ns\n", nowNS() - before);
		measureClock();
	}
	return true;
}

/* The client has gone quiet: drop the L2, tiler and shader cores. */
void
B2LMali::idleExpired(thread_call_param_t owner, thread_call_param_t unused __unused)
{
	B2LMali *self = (B2LMali *)owner;

	IOLockLock(self->runtimeLock);
	if (self->coresOn && nowNS() - self->lastJobNS >= B2L_RT_IDLE_MS * 1000000ull) {
		self->gpuClock(false);
		b2l_mali_power_off(&self->io);
		self->coresOn = false;
	}
	IOLockUnlock(self->runtimeLock);
}

void
B2LMali::runtimeEnd()
{
	if (idleCall != NULL) {
		thread_call_cancel(idleCall);
	}
	for (unsigned i = 0; i < B2L_RT_SLOTS; i++) {
		if (slots[i].range != NULL) {
			slots[i].range->complete();
			OSSafeReleaseNULL(slots[i].range);
		}
		OSSafeReleaseNULL(slots[i].surface);
		slots[i].last_use = 0;
	}
	for (unsigned i = 0; i < B2L_MALI_MAX_BUFFERS; i++) {
		OSSafeReleaseNULL(buffers[i]);
	}
	if (coresOn) {
		gpuClock(false);
		b2l_mali_power_off(&io);
		coresOn = false;
	}
	OSSafeReleaseNULL(runtimeTables);
	OSSafeReleaseNULL(runtimeHeap);
	mappedBytes = 0;
}

bool
B2LMali::mapMemory(IOMemoryDescriptor *memory, uint64_t va, IOByteCount bytes, bool code)
{
	for (IOByteCount off = 0; off < bytes; off += 4096) {
		IOByteCount length = 0;
		addr64_t pa = memory->getPhysicalSegment(off, &length, kIOMemoryMapperNone);
		if (!pa || !(code ? b2l_mali_pt_map_code(&pt, va + off, pa) :
		    b2l_mali_pt_map(&pt, va + off, pa))) {
			return false;
		}
	}
	return true;
}

void
B2LMali::evictSlot(unsigned index)
{
	struct b2l_mali_slot *slot = &slots[index];
	uint64_t va = B2L_RT_SURFACE_VA + ((uint64_t)index << B2L_RT_SLOT_LOG2);
	IOByteCount bytes = slot->range ? slot->range_bytes : round_page(slot->surface->allocSize);

	for (IOByteCount off = 0; off < bytes; off += 4096) {
		b2l_mali_pt_unmap(&pt, va + off);
	}
	flushTables();
	if (coresOn) {
		b2l_mali_flush_region(&io, va, B2L_RT_SLOT_LOG2);
	}
	mappedBytes -= bytes;
	if (slot->range != NULL) {
		slot->range->complete();
		OSSafeReleaseNULL(slot->range);
	}
	OSSafeReleaseNULL(slot->surface);
	slot->last_use = 0;
}

bool
B2LMali::evictOldest()
{
	unsigned oldest = B2L_RT_SLOTS;

	for (unsigned i = 0; i < B2L_RT_SLOTS; i++) {
		if ((slots[i].surface != NULL || slots[i].range != NULL) &&
		    (oldest == B2L_RT_SLOTS || slots[i].last_use < slots[oldest].last_use)) {
			oldest = i;
		}
	}
	if (oldest == B2L_RT_SLOTS) {
		return false;
	}
	evictSlot(oldest);
	return true;
}

/* The surface's slot, mapping it first if needed. Consumes nothing; -1 on failure. */
int
B2LMali::slotFor(B2LSurface *surface)
{
	IOByteCount bytes = round_page(surface->allocSize);
	int free_slot = -1;

	for (unsigned i = 0; i < B2L_RT_SLOTS; i++) {
		if (slots[i].surface == surface) {
			slots[i].last_use = ++useClock;
			return (int)i;
		}
		if (slots[i].surface == NULL && slots[i].range == NULL && free_slot < 0) {
			free_slot = (int)i;
		}
	}
	if (surface->memory == NULL || bytes > (1ull << B2L_RT_SLOT_LOG2) || bytes > B2L_RT_MAPPED_LIMIT) {
		return -1;
	}
	while (free_slot < 0 || mappedBytes + bytes > B2L_RT_MAPPED_LIMIT) {
		if (!evictOldest()) {
			return -1;
		}
		for (unsigned i = 0; free_slot < 0 && i < B2L_RT_SLOTS; i++) {
			if (slots[i].surface == NULL && slots[i].range == NULL) {
				free_slot = (int)i;
			}
		}
	}
	uint64_t va = B2L_RT_SURFACE_VA + ((uint64_t)free_slot << B2L_RT_SLOT_LOG2);
	if (!mapMemory(surface->memory, va, bytes, false)) {
		return -1;
	}
	flushTables();
	surface->retain();
	slots[free_slot].surface = surface;
	slots[free_slot].last_use = ++useClock;
	slots[free_slot].cleaned = 0;
	mappedBytes += bytes;
	return free_slot;
}

IOReturn
B2LMali::createBuffer(uint64_t bytes, uint64_t flags, uint64_t *va, uint64_t *type)
{
	if (bytes == 0 || bytes > B2L_RT_BUFFER_MAX || (flags & ~(uint64_t)B2L_MALI_BUFFER_CODE)) {
		return kIOReturnBadArgument;
	}
	bytes = round_page(bytes);
	for (unsigned i = 0; i < B2L_MALI_MAX_BUFFERS; i++) {
		if (buffers[i] != NULL) {
			continue;
		}
		buffers[i] = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
		    kIODirectionInOut | kIOMemoryKernelUserShared, bytes, 4096);
		if (buffers[i] == NULL) {
			return kIOReturnNoMemory;
		}
		bzero(buffers[i]->getBytesNoCopy(), bytes);
		uint64_t base_va = B2L_RT_BUFFER_VA + i * B2L_RT_BUFFER_STEP;
		if (!mapMemory(buffers[i], base_va, bytes, flags & B2L_MALI_BUFFER_CODE)) {
			OSSafeReleaseNULL(buffers[i]);
			return kIOReturnNoSpace;
		}
		flushTables();
		flush_dcache64((addr64_t)buffers[i]->getBytesNoCopy(), (unsigned)bytes, FALSE);
		*va = base_va;
		*type = i;
		return kIOReturnSuccess;
	}
	return kIOReturnNoResources;
}

IOReturn
B2LMali::mapSurface(uint32_t id, uint64_t *out)
{
	B2LSurface *surface = b2l_surface_copy(id);
	if (surface == NULL) {
		return kIOReturnNotFound;
	}
	int slot = slotFor(surface);
	if (slot >= 0) {
		out[0] = B2L_RT_SURFACE_VA + ((uint64_t)slot << B2L_RT_SLOT_LOG2) + surface->offset;
		out[1] = surface->bytesPerRow;
		out[2] = surface->width;
		out[3] = surface->height;
		out[4] = surface->pixelFormat;
	}
	surface->release();
	return slot >= 0 ? kIOReturnSuccess : kIOReturnNoSpace;
}

/*
 * Client memory, wired while mapped. The same range is reused only while its
 * first and last pages are still the ones wired: freed and reallocated memory
 * at the same address must not reach the old pages.
 */
IOReturn
B2LMali::mapRange(task_t task, uint64_t va, uint64_t bytes, uint64_t *gpu_va)
{
	uint64_t base = trunc_page_64(va), end = round_page_64(va + bytes), length = end - base;
	pmap_t map = get_task_pmap(task);
	int free_slot = -1;

	if (bytes == 0 || end < base || length > (1ull << B2L_RT_SLOT_LOG2) || length > B2L_RT_MAPPED_LIMIT) {
		return kIOReturnBadArgument;
	}
	ppnum_t first = pmap_find_phys(map, base), last = pmap_find_phys(map, end - 4096);
	for (unsigned i = 0; i < B2L_RT_SLOTS; i++) {
		struct b2l_mali_slot *slot = &slots[i];
		if (slot->range != NULL && slot->range_va == base && slot->range_bytes == length) {
			if (slot->range_first == first && slot->range_last == last) {
				slot->last_use = ++useClock;
				*gpu_va = B2L_RT_SURFACE_VA + ((uint64_t)i << B2L_RT_SLOT_LOG2) + (va - base);
				return kIOReturnSuccess;
			}
			evictSlot(i);
		}
	}
	IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(base, length, kIODirectionInOut, task);
	if (memory == NULL) {
		return kIOReturnNoMemory;
	}
	if (memory->prepare() != kIOReturnSuccess) {
		memory->release();
		return kIOReturnVMError;
	}
	while (mappedBytes + length > B2L_RT_MAPPED_LIMIT) {
		if (!evictOldest()) {
			break;
		}
	}
	for (unsigned i = 0; free_slot < 0 && i < B2L_RT_SLOTS; i++) {
		if (slots[i].surface == NULL && slots[i].range == NULL) {
			free_slot = (int)i;
		}
	}
	if (free_slot < 0 && evictOldest()) {
		for (unsigned i = 0; free_slot < 0 && i < B2L_RT_SLOTS; i++) {
			if (slots[i].surface == NULL && slots[i].range == NULL) {
				free_slot = (int)i;
			}
		}
	}
	uint64_t slot_va = B2L_RT_SURFACE_VA + ((uint64_t)free_slot << B2L_RT_SLOT_LOG2);
	if (free_slot < 0 || !mapMemory(memory, slot_va, length, false)) {
		memory->complete();
		memory->release();
		return kIOReturnNoSpace;
	}
	flushTables();
	struct b2l_mali_slot *slot = &slots[free_slot];
	slot->range = memory;
	slot->range_va = base;
	slot->range_bytes = length;
	slot->range_first = pmap_find_phys(map, base);
	slot->range_last = pmap_find_phys(map, end - 4096);
	slot->last_use = ++useClock;
	mappedBytes += length;
	*gpu_va = slot_va + (va - base);
	return kIOReturnSuccess;
}

IOReturn
B2LMali::submit(const struct b2l_mali_submit *request, uint64_t *out)
{
	uint32_t ids[B2L_MALI_MAX_SUBMIT + 1];
	uint32_t count = request->source_count;

	if (count > B2L_MALI_MAX_SUBMIT || request->pass_buffer >= B2L_MALI_MAX_BUFFERS ||
	    buffers[request->pass_buffer] == NULL ||
	    request->pass_bytes > buffers[request->pass_buffer]->getLength()) {
		return kIOReturnBadArgument;
	}
	bcopy(request->sources, ids, count * sizeof(ids[0]));
	uint64_t begin = nowNS();
	flush_dcache64((addr64_t)buffers[request->pass_buffer]->getBytesNoCopy(), request->pass_bytes, FALSE);
	if (request->target) {
		ids[count++] = request->target;
	}
	/* Sources clean only when written since; the target always: the CPU may
	 * hold dirty lines there, and must not read stale ones afterwards. */
	for (uint32_t i = 0; i < count; i++) {
		B2LSurface *surface = b2l_surface_copy(ids[i]);
		if (surface == NULL) {
			return kIOReturnNotFound;
		}
		int slot = slotFor(surface);
		uint32_t seed = b2l_surface_current_seed(surface);
		bool is_target = request->target && i == count - 1;
		if (slot >= 0 && (is_target || !slots[slot].cleaned || slots[slot].cleaned_seed != seed)) {
			uintptr_t address = surface->kernelAddress();
			if (address != 0) {
				flush_dcache64((addr64_t)address + surface->offset, surface->bytesPerRow * surface->height, FALSE);
			}
			slots[slot].cleaned = 1;
			slots[slot].cleaned_seed = seed;
		}
		surface->release();
		if (slot < 0) {
			return kIOReturnNoSpace;
		}
	}
	uint64_t cleaned = nowNS();
	cleanNS += cleaned - begin;

	if (!coresUp()) {
		return kIOReturnNotReady;
	}
	checkTemperature();
	uint32_t status = B2L_MALI_STATUS_DONE;
	uint64_t tiler = 0;
	if (request->tiler_va) {
		status = b2l_mali_run_job(&io, B2L_MALI_SLOT_TILER, request->tiler_va, B2L_RT_JOB_TIMEOUT_US);
		tiler = nowNS() - cleaned;
	}
	uint64_t fragment_begin = nowNS();
	if (status == B2L_MALI_STATUS_DONE) {
		status = b2l_mali_run_job(&io, B2L_MALI_SLOT_FRAGMENT, request->fragment_va, B2L_RT_JOB_TIMEOUT_US);
	}
	out[0] = status;
	out[1] = tiler;
	out[2] = nowNS() - fragment_begin;
	if (status != B2L_MALI_STATUS_DONE) {
		reportFault((unsigned)submits);
		/* A faulted address space stays stopped until the GPU is reset. */
		if (b2l_mali_soft_reset(&io) && b2l_mali_power_on(&io) == NULL) {
			b2l_mali_set_as0(&io, pt.tables_pa);
		}
	} else if (request->target) {
		B2LSurface *surface = b2l_surface_copy(request->target);
		if (surface != NULL) {
			uintptr_t address = surface->kernelAddress();
			if (address != 0) {
				flush_dcache64((addr64_t)address + surface->offset, surface->bytesPerRow * surface->height, FALSE);
			}
			surface->release();
		}
	}
	submits++;
	lastJobNS = nowNS();
	if (idleCall != NULL) {
		thread_call_enter_delayed(idleCall, lastJobTimeout());
	}
	return kIOReturnSuccess;
}

uint64_t
B2LMali::lastJobTimeout()
{
	uint64_t deadline = 0;
	clock_interval_to_deadline(B2L_RT_IDLE_MS + 1, kMillisecondScale, &deadline);
	return deadline;
}

bool
B2LMaliUserClient::initWithTask(task_t task, void *security, UInt32 type)
{
	clientTask = task;
	return IOUserClient::initWithTask(task, security, type);
}

bool
B2LMaliUserClient::start(IOService *provider)
{
	owner = OSDynamicCast(B2LMali, provider);
	return owner != NULL && IOUserClient::start(provider);
}

IOReturn
B2LMaliUserClient::clientClose(void)
{
	if (opened) {
		IOLockLock(owner->runtimeLock);
		owner->runtimeEnd();
		owner->clientOpen = false;
		IOLockUnlock(owner->runtimeLock);
		opened = false;
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
B2LMaliUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	if (!opened || type >= B2L_MALI_MAX_BUFFERS) {
		return kIOReturnBadArgument;
	}
	IOLockLock(owner->runtimeLock);
	IOBufferMemoryDescriptor *buffer = owner->buffers[type];
	if (buffer != NULL) {
		buffer->retain();
	}
	IOLockUnlock(owner->runtimeLock);
	if (buffer == NULL) {
		return kIOReturnNotFound;
	}
	*options = 0;
	*memory = buffer;
	return kIOReturnSuccess;
}

IOReturn
B2LMaliUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target, __unused void *reference)
{
	if (selector == kB2LMaliLog) {
		char text[B2L_MALI_MAX_LOG];
		size_t n = args->structureInputSize;
		if (args->structureInput == NULL || n >= sizeof(text)) {
			return kIOReturnBadArgument;
		}
		bcopy(args->structureInput, text, n);
		text[n] = 0;
		b2l_diag_record("B2L GPU UI: %s\n", text);
		return kIOReturnSuccess;
	}

	IOReturn ret = kIOReturnUnsupported;
	IOLockLock(owner->runtimeLock);
	if (!opened) {
		if (!owner->runtimeReady || owner->clientOpen) {
			IOLockUnlock(owner->runtimeLock);
			return kIOReturnExclusiveAccess;
		}
		if (!owner->runtimeBegin()) {
			owner->runtimeEnd();
			IOLockUnlock(owner->runtimeLock);
			b2l_diag_record("B2L GPU: runtime start failed\n");
			return kIOReturnNotReady;
		}
		owner->clientOpen = true;
		opened = true;
		b2l_diag_record("B2L GPU: client opened by pid %d\n", proc_selfpid());
	}
	switch (selector) {
	case kB2LMaliCreateBuffer:
		if (args->scalarInputCount == 2 && args->scalarOutputCount == 2) {
			ret = owner->createBuffer(args->scalarInput[0], args->scalarInput[1],
			    &args->scalarOutput[0], &args->scalarOutput[1]);
		} else {
			ret = kIOReturnBadArgument;
		}
		break;
	case kB2LMaliMapSurface:
		if (args->scalarInputCount == 1 && args->scalarOutputCount == 5) {
			ret = owner->mapSurface((uint32_t)args->scalarInput[0], args->scalarOutput);
		} else {
			ret = kIOReturnBadArgument;
		}
		break;
	case kB2LMaliMapRange:
		if (args->scalarInputCount == 2 && args->scalarOutputCount == 1) {
			ret = owner->mapRange(clientTask, args->scalarInput[0], args->scalarInput[1], args->scalarOutput);
		} else {
			ret = kIOReturnBadArgument;
		}
		break;
	case kB2LMaliSubmit:
		if (args->structureInputSize == sizeof(struct b2l_mali_submit) && args->scalarOutputCount == 3 &&
		    args->structureInput != NULL) {
			struct b2l_mali_submit request;
			bcopy(args->structureInput, &request, sizeof(request));
			ret = owner->submit(&request, args->scalarOutput);
		} else {
			ret = kIOReturnBadArgument;
		}
		break;
	case kB2LMaliInfo:
		if (args->scalarOutputCount == 3) {
			args->scalarOutput[0] = B2L_RT_HEAP_VA;
			args->scalarOutput[1] = B2L_RT_HEAP_BYTES;
			args->scalarOutput[2] = owner->cleanNS;
			ret = kIOReturnSuccess;
		} else {
			ret = kIOReturnBadArgument;
		}
		break;
	default:
		break;
	}
	IOLockUnlock(owner->runtimeLock);
	return ret;
}

bool
B2LMali::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	int enabled = 1;
	if (PE_parse_boot_argn("b2l_gpu", &enabled, sizeof(enabled)) && !enabled) {
		b2l_diag_record("B2L GPU: disabled by boot argument\n");
		return false;
	}
	runtimeLock = IOLockAlloc();
	if (runtimeLock == NULL) {
		return false;
	}
	thread_t thread;
	retain();
	if (kernel_thread_start(&B2LMali::threadMain, this, &thread) != KERN_SUCCESS) {
		release();
		return false;
	}
	thread_deallocate(thread);
	return true;
}
