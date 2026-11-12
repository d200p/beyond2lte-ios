/* SPDX-License-Identifier: BSD-2-Clause
 * Real power-off and restart for B2L: without this XNU stops at "CPU halted"
 * with the phone still on. Panics are untouched and keep their halted screen.
 * Holding both volume buttons for 3 s syncs the disks and restarts into
 * recovery, so no key sequence has to be timed against the bootloader.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <kern/thread.h>
#include <sys/reboot.h>
#include <arm64/b2l_diag.h>
#include "b2l_power.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern "C" int reboot_kernel(int, char *);
extern "C" void b2l_power_request_recovery(void);

#define B2L_POWER_PMU          0x15860000ull
#define B2L_POWER_GPIO         0x15850000ull
#define B2L_POWER_KEY_OFFSET   0x44            /* as B2LInput: low while pressed */
#define B2L_POWER_KEY_MASK     (1u << 4)
#define B2L_POWER_LOG_DRAIN_US 6000000         /* the persistent log drains every 5 s at most */

static volatile uint8_t *b2l_power_pmu, *b2l_power_gpio;
static volatile bool b2l_power_to_recovery;

static uint32_t
pmu_read(void *ctx __unused, uint32_t offset)
{
	return *(volatile uint32_t *)(b2l_power_pmu + offset);
}

static void
pmu_write(void *ctx __unused, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(b2l_power_pmu + offset) = value;
	__builtin_arm_dsb(15);
}

static int
power_key_down(void *ctx __unused)
{
	return !(*(volatile uint32_t *)(b2l_power_gpio + B2L_POWER_KEY_OFFSET) & B2L_POWER_KEY_MASK);
}

static void
delay_ms(void *ctx __unused, uint32_t ms)
{
	IODelay(ms * 1000);
}

static const struct b2l_power_io b2l_power_ops = { NULL, pmu_read, pmu_write, power_key_down, delay_ms };

/* Mapped at start: the halt path must not depend on the VM still working. */
extern "C" void
b2l_power_init(void)
{
	b2l_power_pmu = (volatile uint8_t *)ml_io_map(B2L_POWER_PMU, B2L_PMU_BYTES);
	b2l_power_gpio = (volatile uint8_t *)ml_io_map(B2L_POWER_GPIO, 4096);
}

/* True if handled; only the two orderly requests are. */
extern "C" bool
b2l_power_halt_restart(unsigned int type)
{
	if ((type != kPEHaltCPU && type != kPERestartCPU) || b2l_power_pmu == NULL || b2l_power_gpio == NULL) {
		return false;
	}
	bool recovery = b2l_power_to_recovery;
	b2l_diag_record("B2L POWER: %s\n", type == kPEHaltCPU ? "powering off" :
	    (recovery ? "restarting into recovery" : "restarting"));
	IODelay(B2L_POWER_LOG_DRAIN_US);
	if (type == kPEHaltCPU) {
		b2l_power_off(&b2l_power_ops);
		b2l_diag_record("B2L POWER: still on after PS_HOLD, restarting\n");
		IODelay(B2L_POWER_LOG_DRAIN_US);
	}
	b2l_power_restart(&b2l_power_ops, recovery);
	for (;;) {
		__builtin_arm_wfi();
	}
}

static void
recovery_thread(void *unused __unused, wait_result_t result __unused)
{
	char message[] = "B2L volume keys";

	b2l_power_to_recovery = true;
	b2l_diag_record("B2L POWER: volume keys held, syncing for recovery\n");
	reboot_kernel(RB_AUTOBOOT, message);
	thread_terminate(current_thread());
}

/* Called from the input poll, which must not block. One request is enough. */
extern "C" void
b2l_power_request_recovery(void)
{
	static volatile SInt32 requested;
	thread_t thread;

	if (OSCompareAndSwap(0, 1, (volatile UInt32 *)&requested) &&
	    kernel_thread_start(&recovery_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
