/* SPDX-License-Identifier: BSD-2-Clause */
#include "b2l_persistent_log.h"
#include <arm64/b2l_arm64e_compat.h>
#include <arm64/b2l_early.h>
#include <arm64/b2l_diag.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/cpu_data.h>
#include <mach/mach_time.h>
#include <kern/task.h>
#include <kern/processor.h>
#include <kern/recount.h>
#include <string.h>
static struct b2l_log_ring console_ring;
extern void b2l_fb_swap_snapshot(uint64_t out[7]);
extern void b2l_acpm_dvfs_snapshot(int32_t temps[4], uint32_t sent_khz[3]);
extern void b2l_iokit_busy_report(void);
struct proc;
extern char *proc_name_address(struct proc *p);

/*
 * S10 port diagnostic: every report window, the eight tasks that used the most
 * CPU time since the previous window (recount lifetime user+system time, read
 * without task locks), so a slow UI can be attributed to a process.
 *   B2L CPUTOP: window_ms=N busy_ms=N name(pid)=ms ...
 */
#define B2L_CPUTOP_SLOTS 768u
#define B2L_CPUTOP_SHOW  8u
struct b2l_cputop_entry { int pid; uint64_t total; };
static struct b2l_cputop_entry b2l_cputop_prev[B2L_CPUTOP_SLOTS], b2l_cputop_next[B2L_CPUTOP_SLOTS];
static unsigned b2l_cputop_prev_count;
static uint64_t b2l_cputop_last_abs;
static void b2l_cputop_report(void)
{
    struct { int pid; uint64_t delta; char name[20]; } top[B2L_CPUTOP_SHOW];
    unsigned shown = 0, count = 0;
    uint64_t now = mach_absolute_time(), busy = 0;
    bool first = b2l_cputop_last_abs == 0;
    task_t task;
    memset(top, 0, sizeof(top));
    lck_mtx_lock(&tasks_threads_lock);
    queue_iterate(&tasks, task, task_t, tasks) {
        if (count >= B2L_CPUTOP_SLOTS) break;
        struct recount_times_mach t = recount_task_times(task);
        uint64_t total = t.rtm_user + t.rtm_system, prev = 0;
        int pid = task == kernel_task ? 0 : task_pid(task);
        bool known = false;
        for (unsigned i = 0; i < b2l_cputop_prev_count; i++) {
            if (b2l_cputop_prev[i].pid == pid) { prev = b2l_cputop_prev[i].total; known = true; break; }
        }
        b2l_cputop_next[count].pid = pid;
        b2l_cputop_next[count].total = total;
        count++;
        uint64_t delta = known ? (total >= prev ? total - prev : 0) : total;
        busy += delta;
        if (first || delta == 0) continue;
        unsigned at = shown < B2L_CPUTOP_SHOW ? shown : B2L_CPUTOP_SHOW - 1;
        if (shown == B2L_CPUTOP_SHOW && delta <= top[at].delta) continue;
        while (at > 0 && top[at - 1].delta < delta) { top[at] = top[at - 1]; at--; }
        top[at].pid = pid;
        top[at].delta = delta;
        const char *name = "kernel_task";
        if (task != kernel_task) {
            void *p = get_bsdtask_info(task);
            name = p ? proc_name_address((struct proc *)p) : "?";
        }
        strlcpy(top[at].name, name ? name : "?", sizeof(top[at].name));
        if (shown < B2L_CPUTOP_SHOW) shown++;
    }
    lck_mtx_unlock(&tasks_threads_lock);
    memcpy(b2l_cputop_prev, b2l_cputop_next, count * sizeof(b2l_cputop_next[0]));
    b2l_cputop_prev_count = count;
    uint64_t window_ns = 0, busy_ns = 0;
    if (!first) absolutetime_to_nanoseconds(now - b2l_cputop_last_abs, &window_ns);
    absolutetime_to_nanoseconds(busy, &busy_ns);
    b2l_cputop_last_abs = now;
    if (first) return;
    char line[512];
    int used = snprintf(line, sizeof(line), "B2L CPUTOP: window_ms=%llu busy_ms=%llu tasks=%u",
        window_ns / 1000000, busy_ns / 1000000, count);
    for (unsigned i = 0; i < shown && used > 0 && (size_t)used < sizeof(line); i++) {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(top[i].delta, &ns);
        used += snprintf(line + used, sizeof(line) - (size_t)used, " %s(%d)=%llu", top[i].name, top[i].pid, ns / 1000000);
    }
    b2l_early_trace("%s\n", line);
}
/* Estimate the current CPU's core clock from a dependent add chain of
 * 1,048,576 single-cycle instructions timed by the generic timer. No PMU or
 * clock-management register is touched. The value is the clock of whichever
 * CPU runs the reporter at that moment; successive lines sample others. */
static uint64_t b2l_cpu_mhz_sample(unsigned*cpu)
{
    uint64_t x=0,ns=0;
    disable_preemption();
    *cpu=(unsigned)cpu_number();
    uint64_t began=mach_absolute_time();
#if defined(__arm64__)
    for(unsigned i=0;i<65536;i++){
        __asm__ volatile(
            "add %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\t"
            "add %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\t"
            "add %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\t"
            "add %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1"
            :"+r"(x));
    }
#endif
    uint64_t elapsed=mach_absolute_time()-began;
    enable_preemption();
    absolutetime_to_nanoseconds(elapsed,&ns);
    return ns&&x==65536ull*16?(65536ull*16*1000)/ns:0;
}
static struct b2l_log_writer disk_writer;
static uint32_t start_state;
static uint64_t flush_failures;
static int last_error;
void b2l_log_append(char c){b2l_log_ring_append(&console_ring,(uint8_t)c);}
void b2l_log_append_bytes(const void*p,size_t n){b2l_log_ring_append_bytes(&console_ring,p,n);}
static void b2l_log_worker(void*argument __unused,wait_result_t result __unused)
{
    unsigned reports=0,ticks=0;
    for(;;){
        /* For the first 300 s, drain every second and leave a TICK line, so
         * the last line on disk dates a stall to within about one second. */
        uint64_t up=0;absolutetime_to_nanoseconds(mach_absolute_time(),&up);up/=NSEC_PER_SEC;
        bool early=up<300;
        if(early)b2l_early_trace("B2L TICK sec=%llu\n",up);
        int err=b2l_log_writer_drain(&disk_writer,&console_ring,128);
        if(err){__atomic_fetch_add(&flush_failures,1,__ATOMIC_RELAXED);__atomic_store_n(&last_error,err,__ATOMIC_RELAXED);}
        /* Outside every storage lock/callback. One bounded progress line per
         * ~30s distinguishes an awake scheduler from silent EL0 execution. */
        if(early){if(++ticks<5)goto sleep;ticks=0;}
        if(++reports==6){
            uint64_t v[4];b2l_arm64e_compat_snapshot(v);reports=0;
            uint64_t perf[6];b2l_arm64e_perf_snapshot(perf);
            uint64_t elided[2];b2l_arm64e_precompiled_snapshot(elided);
            b2l_early_trace("B2L EMU PERF: samples=%llu wrapper_ns=%llu vm_ns=%llu hash_ns=%llu paired=%llu pair_fallback=%llu elided=%llu lookups=%llu\n",
                perf[0],perf[1],perf[2],perf[3],perf[4],perf[5],elided[0],elided[1]);
            b2l_diag_progress();
            uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
            unsigned cpu=0;uint64_t mhz=b2l_cpu_mhz_sample(&cpu);
            b2l_early_trace("B2L LIVE: traps=%llu done=%llu pid=%llu pc=0x%llx flush_errors=%llu last_error=%d sec=%llu pacga=%s cpu=%u mhz=%llu\n",
                v[0],v[1],v[2],v[3],__atomic_load_n(&flush_failures,__ATOMIC_RELAXED),__atomic_load_n(&last_error,__ATOMIC_RELAXED),ns/1000000000,b2l_arm64e_pacga_name(),cpu,mhz);
            uint64_t swaps[7];b2l_fb_swap_snapshot(swaps);uint64_t gap_ns=0,copy_total_ns=0,copy_max_ns=0;
            absolutetime_to_nanoseconds(swaps[1],&gap_ns);
            absolutetime_to_nanoseconds(swaps[5],&copy_total_ns);
            absolutetime_to_nanoseconds(swaps[6],&copy_max_ns);
            int32_t temps[4];uint32_t sent[3];b2l_acpm_dvfs_snapshot(temps,sent);
            b2l_early_trace("B2L FRAMES: swaps=%llu max_gap_ms=%llu gaps_over_2s=%llu temps=%d/%d/%d/%d sent_mhz=%u/%u/%u copies=%llu copy_total_ms=%llu copy_max_ms=%llu\n",
                swaps[0],gap_ns/1000000,swaps[2],temps[0],temps[1],temps[2],temps[3],sent[0]/1000,sent[1]/1000,sent[2]/1000,swaps[4],copy_total_ns/1000000,copy_max_ns/1000000);
            b2l_iokit_busy_report();
            b2l_cputop_report();
        }
sleep:
        assert_wait_timeout((event_t)&disk_writer,THREAD_UNINT,early?1:5,NSEC_PER_SEC);
        thread_block(THREAD_CONTINUE_NULL);
    }
}
int b2l_log_start(const struct b2l_log_sink*sink)
{
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&start_state,&expected,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return B2L_LOG_BAD_ARGUMENT;
    int err=b2l_log_writer_open(&disk_writer,sink);
    if(err){__atomic_store_n(&start_state,0,__ATOMIC_RELEASE);return err;}
    thread_t worker; kern_return_t kr=kernel_thread_start(b2l_log_worker,0,&worker);
    if(kr!=KERN_SUCCESS){disk_writer.active=0;__atomic_store_n(&start_state,0,__ATOMIC_RELEASE);return (int)kr;}
    thread_deallocate(worker);__atomic_store_n(&start_state,2,__ATOMIC_RELEASE);return 0;
}
