/* SPDX-License-Identifier: BSD-2-Clause
 * Real power-off and restart for B2L: without this XNU stops at "CPU halted"
 * with the phone still on. Panics are untouched and keep their halted screen.
 * Holding both volume buttons for 3 s syncs the disks and restarts into
 * recovery, so no key sequence has to be timed against the bootloader.
 * S10 port: b2l_recovery_after=<s> does the same by itself after s seconds.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <sys/reboot.h>
#include <arm64/b2l_diag.h>
#include "b2l_power.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern "C" int reboot_kernel(int, char *);
extern "C" void b2l_power_request_recovery(void);
extern "C" void b2l_power_schedule_recovery(void);

#define B2L_POWER_PMU          0x15860000ull
#define B2L_POWER_GPIO         0x15850000ull
#define B2L_POWER_KEY_OFFSET   0x44            /* as B2LInput: low while pressed */
#define B2L_POWER_KEY_MASK     (1u << 4)
#define B2L_POWER_LOG_DRAIN_US 6000000         /* the persistent log drains every 5 s at most */
#define B2L_POWER_STEP_MS      100             /* "off" on a charger: power key poll */
#define B2L_POWER_KICK_MS      10000           /* and charger watchdog kick */
#define B2L_POWER_ON_HOLD_MS   1000            /* power key held this long starts the phone */

extern "C" unsigned b2l_fb_panel_off_now(void);
extern "C" void b2l_pmic_power_off_prepare(void);
extern "C" void b2l_battery_halt_begin(void);
extern "C" void b2l_battery_halt_kick(void);

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
	/* Off: the panel goes dark at once, not after the log has drained. */
	unsigned panel = type == kPEHaltCPU ? b2l_fb_panel_off_now() : 0;
	b2l_diag_record("B2L POWER: %s\n", type == kPEHaltCPU ? "powering off" :
	    (recovery ? "restarting into recovery" : "restarting"));
	if (type == kPEHaltCPU) {
		/* The PMIC must not start the phone again by itself (B2LMali.cpp,
		 * pmicPowerOffPrepare); both lines are let drain, power may go next. */
		b2l_pmic_power_off_prepare();
	}
	IODelay(B2L_POWER_LOG_DRAIN_US);
	if (type == kPEHaltCPU) {
		b2l_power_off(&b2l_power_ops);
		/*
		 * 2026-11-16: still powered, which a charger does (the phone is
		 * usually on USB). This used to restart, so iOS came straight back.
		 * Stay off instead, as a phone switched off on a charger: panel
		 * asleep, CPUs idle, the charger's watchdog kicked so it keeps
		 * charging; a 1 s press of the power key starts the phone.
		 * PS_HOLD is already low, so unplugging the charger cuts power.
		 */
		b2l_diag_record("B2L POWER: still powered after PS_HOLD (charger), panel off (%u DCS writes), "
		    "staying off; hold the power key to start\n", panel);
		IODelay(B2L_POWER_LOG_DRAIN_US);
		b2l_battery_halt_begin();
		IODelay(200000);        /* a battery poll's I2C transfer in flight ends */
		uint32_t held = 0;
		for (uint32_t ms = 0;; ms += B2L_POWER_STEP_MS) {
			if (ms % B2L_POWER_KICK_MS == 0) {
				b2l_battery_halt_kick();
			}
			held = power_key_down(NULL) ? held + B2L_POWER_STEP_MS : 0;
			if (held >= B2L_POWER_ON_HOLD_MS) {
				break;
			}
			IODelay(B2L_POWER_STEP_MS * 1000);
		}
		recovery = false;
	}
	b2l_power_restart(&b2l_power_ops, recovery);
	for (;;) {
		__builtin_arm_wfi();
	}
}

enum { B2L_RECOVERY_KEYS = 1, B2L_RECOVERY_TIMER = 2 };

static void
recovery_thread(void *why, wait_result_t result __unused)
{
	char message[] = "B2L recovery request";

	b2l_power_to_recovery = true;
	b2l_diag_record("B2L POWER: %s, syncing for recovery\n",
	    (uintptr_t)why == B2L_RECOVERY_TIMER ? "b2l_recovery_after reached" : "volume keys held");
	reboot_kernel(RB_AUTOBOOT, message);
	thread_terminate(current_thread());
}

/* Must not block. One request is enough, whoever makes it. */
static void
request_recovery(uintptr_t why)
{
	static volatile SInt32 requested;
	thread_t thread;

	if (OSCompareAndSwap(0, 1, (volatile UInt32 *)&requested) &&
	    kernel_thread_start(&recovery_thread, (void *)why, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* Called from the input poll. */
extern "C" void
b2l_power_request_recovery(void)
{
	request_recovery(B2L_RECOVERY_KEYS);
}

/*
 * S10 port: b2l_recovery_after=<seconds> restarts into recovery that long
 * after start, the same synced way as the volume keys, so a test boot comes
 * back to TWRP for its log with nobody at the phone. Absent or 0: never;
 * at least 120 s.
 */
static uint32_t b2l_recovery_after_s;

static void
recovery_timer_thread(void *unused __unused, wait_result_t result __unused)
{
	b2l_diag_record("B2L POWER: restart into recovery scheduled in %u s (b2l_recovery_after)\n",
	    b2l_recovery_after_s);
	IOSleep(b2l_recovery_after_s * 1000u);
	request_recovery(B2L_RECOVERY_TIMER);
	thread_terminate(current_thread());
}

extern "C" void
b2l_power_schedule_recovery(void)
{
	uint32_t seconds = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("b2l_recovery_after", &seconds, sizeof(seconds)) || seconds == 0) {
		return;
	}
	b2l_recovery_after_s = seconds < 120 ? 120 : seconds;
	if (kernel_thread_start(&recovery_timer_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
