/* SPDX-License-Identifier: BSD-2-Clause
 * One-shot measurement of each CPU's actual core clock.
 *
 * The bootloader sets the Exynos 9820 CPU clocks and nothing in this port
 * changes them. For each CPU the calling kernel thread binds itself to it and
 * times a chain of dependent 64-bit adds, which retire one per cycle on every
 * core type here (Cortex-A55, Cortex-A75, Exynos M4). The best of several runs
 * gives MHz = adds / microseconds. Only arithmetic and the timebase are used;
 * no PMU, clock or voltage register is touched.
 */
#ifndef B2L_CPU_CLOCK_H
#define B2L_CPU_CLOCK_H
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <arm/cpu_internal.h>

#define B2L_CLOCK_ITERATIONS 400000u   /* x16 adds: ~2.3 ms at 2.73 GHz */

static uint64_t __attribute__((noinline))
b2l_cpu_clock_run(void)
{
    uint64_t x = 0, start = mach_absolute_time();
    for (uint32_t i = 0; i < B2L_CLOCK_ITERATIONS; i++) {
        __asm__ volatile(
            "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
            "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
            "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
            "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
            : "+r"(x));
    }
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    return ns;
}

/* Report every CPU's clock once; must run in a kernel thread that may block. */
static void
b2l_cpu_clock_report(void)
{
    for (unsigned cpu = 0; cpu < real_ncpus && cpu < 16; cpu++) {
        processor_t processor = cpu_to_processor((int)cpu);
        if (processor == PROCESSOR_NULL) continue;
        thread_bind(processor);
        thread_block(THREAD_CONTINUE_NULL);          /* migrate onto it */
        uint64_t best = UINT64_MAX;
        unsigned where = (unsigned)cpu_number();
        for (int run = 0; run < 7; run++) {
            uint64_t ns = b2l_cpu_clock_run();
            if (ns && ns < best) best = ns;
        }
        uint64_t adds = (uint64_t)B2L_CLOCK_ITERATIONS * 16u;
        b2l_early_trace("B2L CPU CLOCK: cpu=%u ran_on=%u mhz=%llu (%llu adds in %llu ns)\n",
            cpu, where, best && best != UINT64_MAX ? (adds * 1000u) / best : 0, adds, best);
    }
    thread_bind(PROCESSOR_NULL);
    thread_block(THREAD_CONTINUE_NULL);
}
#endif
