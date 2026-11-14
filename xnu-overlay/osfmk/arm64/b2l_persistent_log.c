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
#include <arm64/proc_reg.h>
#include <kern/misc_protos.h>
#include <vm/vm_map_xnu.h>
#include <vm/vm_shared_region_xnu.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "b2l_cpu_clock.h"
#include "b2l_task_dump.h"
void b2l_cpu_clock_measure(void){b2l_cpu_clock_report();}
/* Scheduler ticks of all CPUs so far, for a load figure over an interval. */
void b2l_cpu_load_sample(uint64_t*busy,uint64_t*idle)
{
    *busy=*idle=0;
    for(unsigned cpu=0;cpu<real_ncpus&&cpu<16;cpu++){
        processor_t processor=cpu_to_processor((int)cpu);
        natural_t ticks[CPU_STATE_MAX]={0};
        if(processor==PROCESSOR_NULL)continue;
        processor_cpu_load_info(processor,ticks);
        *busy+=(uint64_t)ticks[CPU_STATE_USER]+ticks[CPU_STATE_SYSTEM];
        *idle+=ticks[CPU_STATE_IDLE];
    }
}
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
/*
 * S10 port diagnostic: where backboardd's CPU time goes, and one app's
 * (b2l_pcs_app=NAME, default MobileNotes). The timer interrupt (sleh_irq,
 * patch 0068) passes the interrupted state; when it interrupted one of those,
 * its PC is counted in a 64-byte bucket. Boot 29 put 93% of backboardd's
 * samples in the kernel, 84% of them where interrupts are enabled again, so a
 * kernel sample also counts the next four return addresses on the thread's
 * kernel stack and the user PC and LR the thread entered the kernel from.
 * Kernel PCs are slid: anchor, on the header line, is where b2l_pcs_report was
 * loaded. A bucket's low six bits say what it counts:
 *   bits 0-2  0 interrupted PC, 1-4 kernel caller at that depth,
 *             5 user PC of a thread in the kernel, 6 its user LR
 *   bit 3     0 backboardd, 1 the app
 * Each report window prints the busiest buckets and starts a new table:
 *   B2L PCS: samples=N user=N kernel=N app=NAME app_samples=N app_user=N lost=N buckets=N shown=N kept=N anchor=0x...
 *   B2L PCS: bucket=count ... (eight per line, busiest first)
 */
#define B2L_PCS_SLOTS 8192u     /* power of two */
#define B2L_PCS_PROBE 32u
#define B2L_PCS_SHOW  384u
#define B2L_PCS_DEPTH 4u
struct b2l_pcs_slot { uint64_t pc; uint32_t count; uint32_t unused; };
static struct b2l_pcs_slot b2l_pcs[2][B2L_PCS_SLOTS];
static struct b2l_pcs_slot b2l_pcs_top[B2L_PCS_SHOW];
static uint64_t b2l_pcs_samples[2][2], b2l_pcs_user[2][2], b2l_pcs_lost[2];
static uint32_t b2l_pcs_set;
static char b2l_pcs_app[20] = "MobileNotes";
static uint32_t b2l_pcs_app_ready;
extern vm_offset_t kernel_stack_size;
extern arm_saved_state_t *get_user_regs(thread_t);
static void b2l_pcs_add(unsigned set, uint64_t key)
{
    unsigned h = (unsigned)(((key >> 6) * 0x9E3779B97F4A7C15ull) >> 51);
    for (unsigned i = 0; i < B2L_PCS_PROBE; i++) {
        struct b2l_pcs_slot *slot = &b2l_pcs[set][(h + i) & (B2L_PCS_SLOTS - 1)];
        uint64_t seen = __atomic_load_n(&slot->pc, __ATOMIC_RELAXED);
        if (seen == 0) {
            uint64_t expected = 0;
            seen = __atomic_compare_exchange_n(&slot->pc, &expected, key, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED) ? key : expected;
        }
        if (seen == key) { __atomic_fetch_add(&slot->count, 1, __ATOMIC_RELAXED); return; }
    }
    __atomic_fetch_add(&b2l_pcs_lost[set], 1, __ATOMIC_RELAXED);
}
void b2l_pc_sample(arm_saved_state_t *state);
void b2l_pc_sample(arm_saved_state_t *state)
{
    task_t task = current_task();
    if (state == NULL || task == TASK_NULL || task == kernel_task) return;
    void *p = get_bsdtask_info(task);
    const char *name = p ? proc_name_address((struct proc *)p) : NULL;
    if (name == NULL) return;
    unsigned who;
    if (strncmp(name, "backboardd", 11) == 0) who = 0;
    else if (__atomic_load_n(&b2l_pcs_app_ready, __ATOMIC_ACQUIRE) && strncmp(name, b2l_pcs_app, sizeof(b2l_pcs_app)) == 0) who = 1;
    else return;
    unsigned set = __atomic_load_n(&b2l_pcs_set, __ATOMIC_ACQUIRE) & 1u;
    uint64_t tag = (uint64_t)who << 3;
    uint64_t pc = (uint64_t)get_saved_state_pc(state);
    bool user = PSR64_IS_USER64(get_saved_state_cpsr(state));
    __atomic_fetch_add(&b2l_pcs_samples[set][who], 1, __ATOMIC_RELAXED);
    if (user) __atomic_fetch_add(&b2l_pcs_user[set][who], 1, __ATOMIC_RELAXED);
    if (pc != 0) b2l_pcs_add(set, (pc & ~63ull) | tag);
    if (user) return;
    /* Frame records on this thread's kernel stack only, each strictly above
     * the last: a bad frame pointer ends the walk instead of being followed. */
    thread_t thread = current_thread();
    uint64_t low = (uint64_t)thread->kernel_stack, high = low + (uint64_t)kernel_stack_size;
    uint64_t fp = (uint64_t)get_saved_state_fp(state);
    for (unsigned depth = 1; low != 0 && depth <= B2L_PCS_DEPTH; depth++) {
        if (fp < low || fp > high - 16 || (fp & 7)) break;
        uint64_t next = ((const uint64_t *)fp)[0], lr = ((const uint64_t *)fp)[1];
        if (lr == 0) break;
        b2l_pcs_add(set, (lr & ~63ull) | tag | depth);
        if (next <= fp) break;
        fp = next;
    }
    arm_saved_state_t *regs = get_user_regs(thread);
    if (regs != NULL) {
        uint64_t upc = (uint64_t)get_saved_state_pc(regs), ulr = (uint64_t)get_saved_state_lr(regs);
        if (upc != 0) b2l_pcs_add(set, (upc & ~63ull) | tag | 5);
        if (ulr != 0) b2l_pcs_add(set, (ulr & ~63ull) | tag | 6);
    }
}
static void b2l_pcs_report(void)
{
    if (!__atomic_load_n(&b2l_pcs_app_ready, __ATOMIC_ACQUIRE)) {
        char app[sizeof(b2l_pcs_app)] = { 0 };
        if (PE_parse_boot_argn("b2l_pcs_app", app, sizeof(app)) && app[0]) strlcpy(b2l_pcs_app, app, sizeof(b2l_pcs_app));
        __atomic_store_n(&b2l_pcs_app_ready, 1, __ATOMIC_RELEASE);
    }
    unsigned set = __atomic_load_n(&b2l_pcs_set, __ATOMIC_ACQUIRE) & 1u;
    __atomic_store_n(&b2l_pcs_set, set ^ 1u, __ATOMIC_RELEASE);
    delay(200);     /* an interrupt that still holds the old set is long done */
    struct b2l_pcs_slot *table = b2l_pcs[set];
    unsigned shown = 0, buckets = 0;
    uint64_t kept = 0;
    for (unsigned i = 0; i < B2L_PCS_SLOTS; i++) {
        uint32_t count = table[i].count;
        if (table[i].pc == 0 || count == 0) continue;
        buckets++;
        if (shown == B2L_PCS_SHOW && count <= b2l_pcs_top[B2L_PCS_SHOW - 1].count) continue;
        unsigned at = shown < B2L_PCS_SHOW ? shown : B2L_PCS_SHOW - 1;
        while (at > 0 && b2l_pcs_top[at - 1].count < count) { b2l_pcs_top[at] = b2l_pcs_top[at - 1]; at--; }
        b2l_pcs_top[at].pc = table[i].pc;
        b2l_pcs_top[at].count = count;
        if (shown < B2L_PCS_SHOW) shown++;
    }
    uint64_t samples = b2l_pcs_samples[set][0], user = b2l_pcs_user[set][0], lost = b2l_pcs_lost[set];
    uint64_t app_samples = b2l_pcs_samples[set][1], app_user = b2l_pcs_user[set][1];
    for (unsigned i = 0; i < shown; i++) kept += b2l_pcs_top[i].count;
    if (samples || app_samples) {
        b2l_early_trace("B2L PCS: samples=%llu user=%llu kernel=%llu app=%s app_samples=%llu app_user=%llu lost=%llu buckets=%u shown=%u kept=%llu anchor=0x%llx\n",
            samples, user, samples - user, b2l_pcs_app, app_samples, app_user, lost, buckets, shown, kept,
            (unsigned long long)(uintptr_t)&b2l_pcs_report);
        char line[256];
        int used = 0;
        for (unsigned i = 0; i < shown; i++) {
            int n = snprintf(line + used, sizeof(line) - (size_t)used, " %llx=%u",
                (unsigned long long)b2l_pcs_top[i].pc, b2l_pcs_top[i].count);
            if (n > 0) used += n;
            if ((i & 7u) == 7u || i + 1 == shown || (size_t)used > sizeof(line) - 40) {
                b2l_early_trace("B2L PCS:%s\n", line);
                used = 0;
            }
        }
    }
    memset(table, 0, sizeof(b2l_pcs[0]));
    memset(b2l_pcs_samples[set], 0, sizeof(b2l_pcs_samples[set]));
    memset(b2l_pcs_user[set], 0, sizeof(b2l_pcs_user[set]));
    b2l_pcs_lost[set] = 0;
}
/*
 * S10 port diagnostic: user call stacks of one app's main thread
 * (b2l_pcs_app, default MobileNotes; b2l_stack=0 turns it off). Boot 33:
 * MobileNotes held a core at 100% with the keyboard idle and wrote nothing
 * about it to the unified log, and the PC sampler only sees single frames.
 * Every 20 ms a kernel thread stops that thread for an instant (thread_hold
 * + thread_stop, as thread_get_state does), reads its user PC, LR and FP,
 * lets it go, then follows its frame records through the app's own map
 * (vm_map_switch_to + copyin). A thread inside a system call shows the user
 * stack it called from. Each 30 s window prints the busiest stacks:
 *   B2L STACK: app=NAME samples=N stacks=N lost=N misses=N slide=0x...
 *   B2L STACK: #i n=count f=pc lr ret ret ... (user addresses, slid)
 */
#define B2L_STK_DEPTH   32
#define B2L_STK_SLOTS   768u
#define B2L_STK_SHOW    30u
#define B2L_STK_MS      20u
#define B2L_STK_VA_MASK 0x0000ffffffffffffull   /* drops pointer-authentication bits */
struct b2l_stack { uint64_t f[B2L_STK_DEPTH]; uint32_t depth, count; };
static struct b2l_stack b2l_stacks[B2L_STK_SLOTS];
static uint32_t b2l_stack_used;

static task_t b2l_stack_find(const char *app, size_t size)
{
    task_t task, found = TASK_NULL;
    lck_mtx_lock(&tasks_threads_lock);
    queue_iterate(&tasks, task, task_t, tasks) {
        if (task == kernel_task) continue;
        void *p = get_bsdtask_info(task);
        const char *name = p ? proc_name_address((struct proc *)p) : NULL;
        if (name != NULL && strncmp(name, app, size) == 0) {
            task_reference(task);
            found = task;
            break;
        }
    }
    lck_mtx_unlock(&tasks_threads_lock);
    return found;
}

static unsigned b2l_stack_sample(task_t task, uint64_t *f)
{
    thread_t thread = THREAD_NULL;
    task_lock(task);
    if (!queue_empty(&task->threads)) {
        thread = (thread_t)queue_first(&task->threads);
        thread_reference(thread);
    }
    task_unlock(task);
    if (thread == THREAD_NULL) return 0;
    uint64_t pc = 0, lr = 0, fp = 0;
    thread_mtx_lock(thread);
    if (thread->active) {
        thread_hold(thread);
        thread_mtx_unlock(thread);
        boolean_t stopped = thread_stop(thread, FALSE);
        thread_mtx_lock(thread);
        if (stopped) {
            arm_saved_state_t *regs = get_user_regs(thread);
            if (regs != NULL) {
                pc = (uint64_t)get_saved_state_pc(regs);
                lr = (uint64_t)get_saved_state_lr(regs);
                fp = (uint64_t)get_saved_state_fp(regs);
            }
            thread_unstop(thread);
        }
        thread_release(thread);
    }
    thread_mtx_unlock(thread);
    thread_deallocate(thread);
    if (pc == 0) return 0;
    unsigned depth = 0;
    f[depth++] = pc & B2L_STK_VA_MASK;
    if (lr != 0) f[depth++] = lr & B2L_STK_VA_MASK;
    vm_map_switch_context_t ctx = vm_map_switch_to(get_task_map(task));
    for (unsigned hop = 0; depth < B2L_STK_DEPTH && hop < 64; hop++) {
        uint64_t record[2];
        if (fp == 0 || (fp & 7) || copyin((user_addr_t)fp, record, sizeof(record))) break;
        uint64_t ret = record[1] & B2L_STK_VA_MASK;
        if (ret == 0) break;
        f[depth++] = ret;
        if (record[0] <= fp) break;
        fp = record[0];
    }
    vm_map_switch_back(ctx);
    return depth;
}

static void b2l_stack_report(task_t task, const char *app, uint32_t samples, uint32_t lost, uint32_t misses)
{
    uint32_t slide = 0;
    if (task != TASK_NULL) {
        vm_shared_region_t region = vm_shared_region_get(task);
        if (region != NULL) {
            slide = region->sr_slide;
            vm_shared_region_deallocate(region);
        }
    }
    if (samples == 0) return;
    b2l_early_trace("B2L STACK: app=%s samples=%u stacks=%u lost=%u misses=%u slide=0x%x\n",
        app, samples, b2l_stack_used, lost, misses, slide);
    static bool shown[B2L_STK_SLOTS];
    memset(shown, 0, sizeof(shown));
    for (unsigned rank = 0; rank < B2L_STK_SHOW; rank++) {
        unsigned best = B2L_STK_SLOTS;
        for (unsigned i = 0; i < b2l_stack_used; i++)
            if (!shown[i] && (best == B2L_STK_SLOTS || b2l_stacks[i].count > b2l_stacks[best].count)) best = i;
        if (best == B2L_STK_SLOTS) break;
        shown[best] = true;
        /* Up to 12 frames per line; later lines are "#i+ f=" continuations. */
        for (unsigned d0 = 0; d0 < b2l_stacks[best].depth; d0 += 12) {
            char line[256];
            int used = d0 == 0 ?
                snprintf(line, sizeof(line), "B2L STACK: #%u n=%u f=", rank, b2l_stacks[best].count) :
                snprintf(line, sizeof(line), "B2L STACK: #%u+ f=", rank);
            for (unsigned d = d0; d < d0 + 12 && d < b2l_stacks[best].depth && used > 0 &&
                (size_t)used < sizeof(line) - 18; d++)
                used += snprintf(line + used, sizeof(line) - (size_t)used, " %llx", (unsigned long long)b2l_stacks[best].f[d]);
            b2l_early_trace("%s\n", line);
        }
    }
}

static void b2l_stack_thread(void *arg __unused, wait_result_t result __unused)
{
    int enabled = 1;
    (void)PE_parse_boot_argn("b2l_stack", &enabled, sizeof(enabled));
    if (!enabled) return;
    char app[20] = "MobileNotes";
    char given[20] = { 0 };
    if (PE_parse_boot_argn("b2l_pcs_app", given, sizeof(given)) && given[0]) strlcpy(app, given, sizeof(app));
    task_t task = TASK_NULL;
    uint32_t samples = 0, lost = 0, misses = 0, tick = 0;
    uint64_t window = mach_absolute_time();
    for (;;) {
        delay_for_interval(B2L_STK_MS, NSEC_PER_MSEC);
        if (task == TASK_NULL && (tick++ % 50) == 0) task = b2l_stack_find(app, sizeof(app));
        if (task != TASK_NULL) {
            uint64_t f[B2L_STK_DEPTH];
            unsigned depth = b2l_stack_sample(task, f);
            if (depth == 0) {
                misses++;
            } else {
                samples++;
                unsigned slot = B2L_STK_SLOTS;
                for (unsigned i = 0; i < b2l_stack_used; i++) {
                    if (b2l_stacks[i].depth == depth && !memcmp(b2l_stacks[i].f, f, depth * sizeof(f[0]))) { slot = i; break; }
                }
                if (slot == B2L_STK_SLOTS && b2l_stack_used < B2L_STK_SLOTS) {
                    slot = b2l_stack_used++;
                    memcpy(b2l_stacks[slot].f, f, depth * sizeof(f[0]));
                    b2l_stacks[slot].depth = depth;
                    b2l_stacks[slot].count = 0;
                }
                if (slot == B2L_STK_SLOTS) lost++; else b2l_stacks[slot].count++;
            }
        }
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - window, &ns);
        if (ns >= 30ull * NSEC_PER_SEC) {
            b2l_stack_report(task, app, samples, lost, misses);
            b2l_stack_used = 0;
            samples = lost = misses = 0;
            window = mach_absolute_time();
            /* Look the app up again each window: it may have quit or restarted. */
            if (task != TASK_NULL) { task_deallocate(task); task = TASK_NULL; }
            tick = 0;
        }
    }
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
    bool clocks_reported=false;
    for(;;){
        /* For the first 300 s, drain every second and leave a TICK line, so
         * the last line on disk dates a stall to within about one second. */
        uint64_t up=0;absolutetime_to_nanoseconds(mach_absolute_time(),&up);up/=NSEC_PER_SEC;
        bool early=up<300;
        if(early)b2l_early_trace("B2L TICK sec=%llu\n",up);
        /* Once, after SMP bring-up and before the UI loads the CPUs. */
        if(!clocks_reported&&up>=20){clocks_reported=true;b2l_cpu_clock_report();}
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
            b2l_pcs_report();
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
    thread_deallocate(worker);__atomic_store_n(&start_state,2,__ATOMIC_RELEASE);
    thread_t sampler;if(kernel_thread_start(b2l_stack_thread,0,&sampler)==KERN_SUCCESS)thread_deallocate(sampler);
    return 0;
}
