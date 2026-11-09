/* SPDX-License-Identifier: BSD-2-Clause
 * Exynos 9820 CPU cluster clocks through Samsung's ACPM firmware (the power
 * management core that owns the PLLs and regulators) and its TMU readings.
 *
 * Protocol and structure adapted from y2sios (MIT, Exynos 990) and Samsung's
 * android_kernel_samsung_exynos9820 (acpm_ipc.c, acpm_dvfs.c, fvmap.c,
 * exynos_acpm_tmu.c). Every address below comes from this S10+'s own device
 * tree (private-backups/samsung-s10plus/live-device-tree.dtb):
 *   acpm_ipc   mailbox 0x15900000, SRAM 0x02039000 (0x30000), initdata-base 0x7000
 *   acpm       fvmap_offset 0xa400 (frequency/voltage tables, read only)
 *   acpm_dvfs  channel 5: { domain, kHz, FREQ_REQ 0, 0 }
 *   acpm_tmu   channel 9: READ_TEMP 0x02, zones BIG 0 (M4), MID 1 (A75), LITTLE 2 (A55), G3D 3
 *   domains    cal_id 0x0B04000x: MIF 0, INT 1, CL0 2 (cpus 0-3 A55), CL1 3 (4-5 A75),
 *              CL2 4 (6-7 M4), G3D 10 (not driven here)
 * No PLL or regulator register is written by this kernel; ACPM does that,
 * voltage first, on request. S-Boot leaves the M4 cluster at about 1377 MHz
 * (measured with the dependent-add clock estimate of the LIVE reporter).
 *
 * Boot args:
 *   b2l_dvfs=0      disabled (loader default during panic diagnosis)
 *   b2l_dvfs=1      SRAM tables and measured core clocks only; no mailbox access
 *   b2l_dvfs=2      ... and set the clusters to their FVMAP tops (or the caps below)
 *   b2l_dvfs_cl0/cl1/cl2=kHz   cap for a cluster (0: FVMAP top); a value not in the table is
 *                   rounded down to the nearest table rate
 *   b2l_dvfs_gov=0  pin the clocks instead of governing them by load (default 1: governed)
 *   b2l_dvfs_wait=S seconds after this thread starts before ACPM is touched (default 10)
 * Lines go to the persistent console with a "B2L DVFS:" prefix.
 */
#include <arm64/b2l_early.h>
#include <kern/clock.h>
#include <kern/cpu_data.h>
#include <kern/cpu_number.h>
#include <kern/host.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <mach/machine.h>
#include <mach/processor_info.h>
#include <machine/machine_routines.h>
#include <kern/misc_protos.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <arm64/b2l_acpm_layout.h>

extern unsigned int real_ncpus;
extern kern_return_t processor_info(processor_t, processor_flavor_t, host_t *, processor_info_t, mach_msg_type_number_t *);

#define ACPM_INTR_PHYS   0x15900000UL
#define ACPM_SRAM_PHYS   0x02039000UL
#define ACPM_SRAM_SIZE   B2L_ACPM_SRAM_BYTES
#define ACPM_INITDATA    B2L_ACPM_INITDATA

#define INTGR0           0x08  /* AP -> APM: bit 16 + channel id */
#define INTCR1           0x20  /* APM -> AP: clear */
#define INTMR1           0x24  /*            mask (1 = masked: we poll) */
#define INTSR1           0x28  /*            status */

#define CH_DVFS          5u
#define CH_TMU           9u
#define FREQ_REQ         0u
#define TMU_IPC_INIT     0x01u
#define TMU_IPC_READ_TEMP 0x02u
#define TYPE_BUFFER      2u
#define SEQ_SHIFT        16
#define SEQ_MASK         (0x3fu << SEQ_SHIFT)

enum { D_CL0 = 2, D_CL1 = 3, D_CL2 = 4 };
#define NCLUSTERS 3
#define MAX_RATES B2L_ACPM_MAX_RATES
#define GOV_RUNGS 6

struct cluster {
    unsigned domain, first_cpu, ncpus, zone;
    const char *name, *arg;
    uint32_t rates[MAX_RATES]; unsigned nrates;   /* FVMAP, ascending kHz */
    uint32_t cap, target, mid, safe;              /* kHz */
    uint32_t rung[GOV_RUNGS]; unsigned nrungs;
    unsigned level;                               /* thermal step 0..2 */
    unsigned gov_rung, gov_low; uint32_t sent;
    uint64_t mhz_before, mhz_after;
};
static struct cluster clusters[NCLUSTERS] = {
    { .domain = D_CL0, .first_cpu = 0, .ncpus = 4, .zone = 2, .name = "cl0 (A55)", .arg = "b2l_dvfs_cl0" },
    { .domain = D_CL1, .first_cpu = 4, .ncpus = 2, .zone = 1, .name = "cl1 (A75)", .arg = "b2l_dvfs_cl1" },
    { .domain = D_CL2, .first_cpu = 6, .ncpus = 2, .zone = 0, .name = "cl2 (M4)", .arg = "b2l_dvfs_cl2" },
};
static const char *const zones[] = { "big", "mid", "little", "g3d" };

struct channel {
    uint32_t id, polling, elem, len, type, seq;
    volatile uint8_t *tx_front, *tx_rear, *tx_base;   /* AP -> APM (descriptor "rx") */
    volatile uint8_t *rx_front, *rx_rear, *rx_base;   /* APM -> AP (descriptor "tx") */
};
static volatile uint8_t *intr, *sram;
static struct channel ch_dvfs, ch_tmu;
static uint32_t mode;
uint64_t b2l_dvfs_touch_at;                            /* set by the input path */
static uint64_t b2l_dvfs_requests, b2l_dvfs_answered;
static int32_t b2l_dvfs_last_temp[4] = { -1, -1, -1, -1 };

#define dlog(...) b2l_early_trace("B2L DVFS: " __VA_ARGS__)

static inline uint32_t rd(volatile uint8_t *at) { return *(volatile uint32_t *)at; }
static inline void wr(volatile uint8_t *at, uint32_t v)
{
    *(volatile uint32_t *)at = v;
    __asm__ volatile ("dsb sy" ::: "memory");
}

/* The descriptor at POSITION `want` (acpm_ipc.c sends through channel[id]).
 * struct ipc_channel { id, field, owner, type, channel_info ch, ap_poll }:
 * channel_info { rx_rear, rx_front, rx_base, rx_indr_buf, rx_indr_buf_base,
 * rx_indr_buf_size, tx_rear, tx_front, tx_base, q_len, q_elem_size, credit,
 * mbox_addr } = 18 words, 72 bytes. */
static int
find_channel(uint32_t want, struct channel *c)
{
    uint32_t channels = rd(sram + ACPM_INITDATA + 8);
    uint32_t count = rd(sram + ACPM_INITDATA + 24);   /* ipc_ap_max */
    if (channels == 0 || (channels & 3) || count == 0 || count > 32 || want >= count ||
        !b2l_acpm_range(channels, count * 72)) {
        dlog("framework at SRAM+%#x names %u AP channels at %#x: not usable\n", ACPM_INITDATA, count, channels);
        return -1;
    }
    volatile uint8_t *d = sram + channels + want * 72;
    uint32_t rx_rear = rd(d + 16), rx_front = rd(d + 20), rx_base = rd(d + 24);
    uint32_t tx_rear = rd(d + 40), tx_front = rd(d + 44), tx_base = rd(d + 48);
    c->id = rd(d); c->type = rd(d + 12); c->len = rd(d + 52); c->elem = rd(d + 56); c->polling = rd(d + 68);
    if (c->id >= 16 || (c->type != 1 && c->type != TYPE_BUFFER) ||
        c->len == 0 || c->len > 64 || c->elem < 16 || c->elem > 64 || (c->elem & 3) ||
        ((rx_base | tx_base | rx_front | tx_front | rx_rear | tx_rear) & 3) ||
        !b2l_acpm_range(rx_front, 4) || !b2l_acpm_range(tx_front, 4) ||
        !b2l_acpm_range(rx_rear, 4) || !b2l_acpm_range(tx_rear, 4) ||
        rx_base + (uint64_t)c->len * c->elem > ACPM_SRAM_SIZE || tx_base + (uint64_t)c->len * c->elem > ACPM_SRAM_SIZE) {
        dlog("channel %u: queue %u x %u, rx %#x/%#x/%#x tx %#x/%#x/%#x: out of range\n", want, c->len, c->elem,
            rx_rear, rx_front, rx_base, tx_rear, tx_front, tx_base);
        return -1;
    }
    c->tx_rear = sram + rx_rear; c->tx_front = sram + rx_front; c->tx_base = sram + rx_base;
    c->rx_rear = sram + tx_rear; c->rx_front = sram + tx_front; c->rx_base = sram + tx_base;
    c->seq = 0;
    if (rd(c->tx_front) >= c->len || rd(c->tx_rear) >= c->len ||
        rd(c->rx_front) >= c->len || rd(c->rx_rear) >= c->len) {
        dlog("channel %u: queue indices outside %u entries\n", want, c->len);
        return -1;
    }
    dlog("channel %u (id %u): %s, %u x %u bytes, %s; to ACPM front %u rear %u, from ACPM front %u rear %u\n",
        want, c->id, c->type == TYPE_BUFFER ? "buffer" : "queue", c->len, c->elem,
        c->polling ? "polled" : "interrupt-driven", rd(c->tx_front), rd(c->tx_rear), rd(c->rx_front), rd(c->rx_rear));
    return 0;
}

/* One command and its answer. 0 answered, 1 sent without answer (buffer channel), -1 not sent. */
static int
ipc(struct channel *c, uint32_t cmd[4])
{
    uint32_t front = rd(c->tx_front), rear = rd(c->tx_rear), seq, i;
    if (front >= c->len || rear >= c->len) return -1;
    uint32_t next = front + 1 == c->len ? 0 : front + 1;
    uint64_t deadline;
    if (c->type == TYPE_BUFFER) {
        front = 0; next = 0;
    } else if (next == rear) {
        dlog("channel %u: queue to ACPM full (front %u rear %u)\n", c->id, front, rd(c->tx_rear));
        return -1;
    }
    seq = c->seq = c->seq >= 63 ? 1 : c->seq + 1;
    cmd[0] = (cmd[0] & ~SEQ_MASK) | (seq << SEQ_SHIFT);
    for (i = 0; i < c->elem / 4; i++) wr(c->tx_base + c->elem * front + i * 4, i < 4 ? cmd[i] : 0);
    wr(c->tx_front, next);
    wr(intr + INTGR0, (1u << c->id) << 16);
    b2l_dvfs_requests++;
    clock_interval_to_deadline(20, NSEC_PER_MSEC, &deadline);
    while (mach_absolute_time() < deadline) {
        if (rd(intr + INTSR1) & (1u << c->id)) {
            if (c->type == TYPE_BUFFER) {
                for (uint32_t w = 0; w < 4; w++) cmd[w] = rd(c->rx_base + w * 4);
                wr(intr + INTCR1, 1u << c->id);
                b2l_dvfs_answered++;
                return 0;
            }
            uint32_t rear = rd(c->rx_rear), last = rd(c->rx_front);
            if (rear >= c->len || last >= c->len) return -1;
            for (i = rear; i != last; i = i + 1 == c->len ? 0 : i + 1) {
                volatile uint8_t *e = c->rx_base + c->elem * i;
                if (((rd(e) & SEQ_MASK) >> SEQ_SHIFT) != seq) continue;
                for (uint32_t w = 0; w < 4; w++) cmd[w] = rd(e + w * 4);
                if (i != rear) for (uint32_t w = 0; w < c->elem / 4; w++) wr(e + w * 4, rd(c->rx_base + c->elem * rear + w * 4));
                rear = rear + 1 == c->len ? 0 : rear + 1;
                wr(c->rx_rear, rear);
                if (rear == rd(c->rx_front)) wr(intr + INTCR1, 1u << c->id);
                b2l_dvfs_answered++;
                return 0;
            }
        }
        delay(10);
    }
    if (c->type == TYPE_BUFFER) return 1;
    dlog("channel %u: no answer to %#x %#x %#x %#x in 20 ms (status %#x)\n", c->id, cmd[0], cmd[1], cmd[2], cmd[3], rd(intr + INTSR1));
    return -1;
}

static int
set_rate(struct cluster *k, uint32_t khz)
{
    uint32_t cmd[4] = { k->domain, khz, FREQ_REQ, 0 };
    int ret = ipc(&ch_dvfs, cmd);
    if (ret < 0) dlog("%s: %u kHz NOT sent\n", k->name, khz);
    return ret < 0 ? -1 : 0;
}

/* degrees C, or -1 (exynos_acpm_tmu.c: type | tzid << 16; answer's temperature in byte 3 of word 1) */
static int
read_temp(unsigned zone)
{
    uint32_t cmd[4] = { 0, TMU_IPC_READ_TEMP | (zone << 16), 0, 0 };
    if (ch_tmu.len == 0 || ipc(&ch_tmu, cmd) != 0 || (int8_t)(cmd[1] >> 8) != 0) return -1;
    int t = (int)((cmd[1] >> 24) & 0xff);
    if (zone < 4) b2l_dvfs_last_temp[zone] = t;
    return t;
}

static int
read_all_temps(int temp[4])
{
    int worst = -1;
    for (unsigned z = 0; z < 4; z++) { temp[z] = read_temp(z); worst = temp[z] > worst ? temp[z] : worst; }
    return worst;
}

/* Dependent single-cycle adds on one CPU, timed by the generic timer. */
static uint64_t
mhz_on_cpu(unsigned cpu)
{
    if (cpu >= real_ncpus || processor_array[cpu] == PROCESSOR_NULL) return 0;
    processor_t previous = thread_bind(processor_array[cpu]);
    thread_block(THREAD_CONTINUE_NULL);
    uint64_t x = 0, ns = 0;
    disable_preemption();
    unsigned actual = (unsigned)cpu_number();
    uint64_t began = mach_absolute_time();
    for (unsigned i = 0; i < 65536; i++) {
        __asm__ volatile(
            "add %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\t"
            "add %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\t"
            "add %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\t"
            "add %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1\n\tadd %0,%0,#1" : "+r"(x));
    }
    uint64_t elapsed = mach_absolute_time() - began;
    enable_preemption();
    thread_bind(previous);
    thread_block(THREAD_CONTINUE_NULL);
    absolutetime_to_nanoseconds(elapsed, &ns);
    return actual == cpu && ns && x == 65536ull * 16 ? (65536ull * 16 * 1000) / ns : 0;
}

/* The parser validates alignment and bounds before each SRAM word read. */
static int
sram_word(void *context, uint32_t offset, uint32_t *word)
{
    (void)context;
    *word = rd(sram + offset);
    return 0;
}

static int
read_fvmap(void)
{
    const struct b2l_acpm_reader reader = { NULL, sram_word };
    struct b2l_acpm_cpu_rates tables;
    uint32_t base;
    dlog("reading attached DVFS plugin descriptors (no SRAM scan)\n");
    if (b2l_acpm_find_fvmap(&reader, &base)) {
        dlog("no unique, bounded DVFS plugin: FVMAP unavailable\n");
        return -1;
    }
    dlog("DVFS plugin names FVMAP at SRAM+%#x; validating CPU tables\n", base);
    if (b2l_acpm_read_cpu_rates(&reader, base, &tables)) {
        dlog("CPU FVMAP headers or rates invalid: no tables accepted\n");
        return -1;
    }
    for (unsigned c = 0; c < NCLUSTERS; c++) {
        struct cluster *k = &clusters[c];
        k->nrates = tables.count[c];
        char line[200]; size_t at = 0;
        for (unsigned l = 0; l < k->nrates; l++) {
            k->rates[l] = tables.khz[c][l];
            if (at < sizeof(line) - 12)
                at += (size_t)snprintf(line + at, sizeof(line) - at, " %u", k->rates[l] / 1000);
        }
        dlog("%s: %u validated rates, MHz%s\n", k->name, k->nrates, line);
    }
    return 0;
}

static uint32_t
table_floor(const struct cluster *k, uint32_t khz)
{
    uint32_t best = 0;
    for (unsigned l = 0; l < k->nrates; l++) if (k->rates[l] <= khz) best = k->rates[l];
    return best ? best : k->rates[0];
}

static int
all_cores_up(void)
{
    return processor_avail_count >= real_ncpus;
}

static void
plan(struct cluster *k)
{
    uint32_t top = k->rates[k->nrates - 1];
    k->cap = 0;
    (void)PE_parse_boot_argn(k->arg, &k->cap, sizeof(k->cap));
    k->target = k->cap ? table_floor(k, k->cap) : top;
    k->mid = table_floor(k, (uint32_t)((uint64_t)k->target * 3 / 4));
    k->safe = table_floor(k, (uint32_t)((uint64_t)k->target / 2));
    /* up to six rungs, the target last, evenly spread over the table below it */
    unsigned usable = 0;
    while (usable < k->nrates && k->rates[usable] <= k->target) usable++;
    k->nrungs = usable < GOV_RUNGS ? usable : GOV_RUNGS;
    for (unsigned r = 0; r < k->nrungs; r++) k->rung[r] = k->rates[(usable - 1) * r / (k->nrungs - 1 ? k->nrungs - 1 : 1)];
    k->rung[k->nrungs - 1] = k->target;
    k->gov_rung = k->nrungs - 1; k->gov_low = 0; k->sent = 0; k->level = 0;
    char line[160]; size_t at = 0;
    for (unsigned r = 0; r < k->nrungs && at < sizeof(line) - 10; r++) at += (size_t)snprintf(line + at, sizeof(line) - at, " %u", k->rung[r] / 1000);
    dlog("%s: target %u MHz (cap %u) mid %u safe %u rungs%s\n", k->name, k->target / 1000, k->cap / 1000, k->mid / 1000, k->safe / 1000, line);
}

static uint32_t gov_ticks[8][2];

static int
cluster_busy(const struct cluster *k)
{
    int worst = -1;
    for (unsigned c = k->first_cpu; c < k->first_cpu + k->ncpus && c < real_ncpus; c++) {
        processor_cpu_load_info_data_t load;
        mach_msg_type_number_t count = PROCESSOR_CPU_LOAD_INFO_COUNT;
        host_t host;
        if (processor_array[c] == PROCESSOR_NULL ||
            processor_info(processor_array[c], PROCESSOR_CPU_LOAD_INFO, &host, (processor_info_t)&load, &count) != KERN_SUCCESS) continue;
        uint32_t busy = load.cpu_ticks[CPU_STATE_USER] + load.cpu_ticks[CPU_STATE_SYSTEM] + load.cpu_ticks[CPU_STATE_NICE];
        uint32_t all = busy + load.cpu_ticks[CPU_STATE_IDLE];
        uint32_t db = busy - gov_ticks[c][0], da = all - gov_ticks[c][1];
        gov_ticks[c][0] = busy; gov_ticks[c][1] = all;
        if (da) { int pct = (int)(db * 100 / da); worst = pct > worst ? pct : worst; }
    }
    return worst;
}

static unsigned
touch_floor(const struct cluster *k)
{
    uint64_t at = __atomic_load_n(&b2l_dvfs_touch_at, __ATOMIC_RELAXED), now = mach_absolute_time(), tail;
    nanoseconds_to_absolutetime(500 * NSEC_PER_MSEC, &tail);
    if (!at || now - at >= tail) return 0;
    /* hold at least the middle rung for half a second after a finger */
    return k->nrungs / 2;
}

static uint32_t
capped(const struct cluster *k, uint32_t khz)
{
    uint32_t cap = k->level == 2 ? k->safe : k->level == 1 ? k->mid : k->target;
    khz = khz > k->target ? k->target : khz;
    return khz > cap ? cap : khz;
}

static void
dvfs_thread(void *arg __unused, wait_result_t wr_unused __unused)
{
    uint32_t wait = 10, govern = 1, tick = 0;
    (void)PE_parse_boot_argn("b2l_dvfs_wait", &wait, sizeof(wait));
    (void)PE_parse_boot_argn("b2l_dvfs_gov", &govern, sizeof(govern));
    delay_for_interval(wait, NSEC_PER_SEC);
    for (unsigned waited = 0; !all_cores_up() && waited < 120; waited++) delay_for_interval(1, NSEC_PER_SEC);
    if (!all_cores_up()) { dlog("only %u of %u cores up: clocks left alone\n", processor_avail_count, real_ncpus); mode = 1; }

    dlog("mode %u: mapping SRAM for descriptor-based FVMAP reads\n", mode);
    sram = (volatile uint8_t *)ml_io_map(ACPM_SRAM_PHYS, ACPM_SRAM_SIZE);
    if (!sram) { dlog("SRAM not mappable\n"); return; }
    dlog("mode %u: framework at SRAM+%#x: %u plugins, %u channels (%u for the AP)\n", mode, ACPM_INITDATA,
        rd(sram + ACPM_INITDATA + 4), rd(sram + ACPM_INITDATA + 12), rd(sram + ACPM_INITDATA + 24));
    int tables_valid = read_fvmap() == 0;
    for (unsigned c = 0; c < NCLUSTERS; c++) {
        clusters[c].mhz_before = mhz_on_cpu(clusters[c].first_cpu);
        dlog("%s: measured %llu MHz before any request\n", clusters[c].name, clusters[c].mhz_before);
    }
    if (mode < 2) { dlog("SRAM-only diagnosis complete; mailbox untouched\n"); return; }
    if (!tables_valid) { dlog("no validated CPU tables: mailbox untouched\n"); return; }
    intr = (volatile uint8_t *)ml_io_map(ACPM_INTR_PHYS, 0x1000);
    if (!intr) { dlog("mailbox not mappable\n"); return; }
    if (find_channel(CH_DVFS, &ch_dvfs)) return;
    if (find_channel(CH_TMU, &ch_tmu)) ch_tmu.len = 0;
    wr(intr + INTMR1, rd(intr + INTMR1) | (1u << CH_DVFS) | (ch_tmu.len ? 1u << CH_TMU : 0));
    if (ch_tmu.len) { uint32_t cmd[4] = { 0, TMU_IPC_INIT, 0, 0 }; if (ipc(&ch_tmu, cmd)) ch_tmu.len = 0; }
    int temp[4]; int worst = read_all_temps(temp);
    dlog("temperatures as S-Boot left them: big %d mid %d little %d g3d %d\n", temp[0], temp[1], temp[2], temp[3]);
    if (ch_tmu.len == 0 || worst < 0) { dlog("no temperatures from ACPM: clocks are not raised without thermal readings\n"); mode = 1; return; }
    for (unsigned c = 0; c < NCLUSTERS; c++) {
        if (clusters[c].nrates == 0) { dlog("%s: no FVMAP table: clocks are not raised\n", clusters[c].name); mode = 1; return; }
        plan(&clusters[c]);
    }
    /* one cluster at a time, little first, each proven by measurement and by ACPM still answering */
    for (unsigned c = 0; c < NCLUSTERS; c++) {
        struct cluster *k = &clusters[c];
        if (set_rate(k, k->target)) { mode = 1; return; }
        k->sent = k->target;
        delay_for_interval(50, NSEC_PER_MSEC);
        k->mhz_after = mhz_on_cpu(k->first_cpu);
        int after = read_all_temps(temp);
        dlog("%s: asked %u MHz: measured %llu -> %llu MHz; temperatures big %d mid %d little %d (%s)\n", k->name, k->target / 1000,
            k->mhz_before, k->mhz_after, temp[0], temp[1], temp[2], after < 0 ? "ACPM STOPPED ANSWERING" : "ACPM answering");
        if (after < 0) { dlog("no further requests this boot\n"); mode = 1; return; }
    }
    dlog("clocks set; %s\n", govern ? "governing by load every 200 ms with thermal steps at 85/95 C" : "pinned with thermal steps at 85/95 C");
    for (;; tick++) {
        delay_for_interval(200, NSEC_PER_MSEC);
        for (unsigned c = 0; govern && c < NCLUSTERS; c++) {
            struct cluster *k = &clusters[c];
            int busy = cluster_busy(k);
            unsigned top = k->nrungs - 1, want = k->gov_rung, floor = touch_floor(k);
            if (busy >= 0) {
                if (busy >= 70) { want = top; k->gov_low = 0; }
                else {
                    uint64_t need = (uint64_t)k->rung[k->gov_rung] * (uint64_t)busy / 60;
                    unsigned fit = 0;
                    while (fit < top && k->rung[fit] < need) fit++;
                    if (fit > k->gov_rung) { want = fit; k->gov_low = 0; }
                    else if (fit < k->gov_rung && ++k->gov_low >= 3) { want = k->gov_rung - 1; k->gov_low = 0; }
                }
            }
            if (want < floor) { want = floor; k->gov_low = 0; }
            k->gov_rung = want;
            uint32_t khz = capped(k, k->rung[want]);
            if (khz != k->sent && set_rate(k, khz) == 0) k->sent = khz;
        }
        if (tick % 5) continue;
        /* every second: temperatures and thermal steps (85 C mid, 95 C safe, released 6 C below) */
        int w = read_all_temps(temp);
        for (unsigned c = 0; c < NCLUSTERS; c++) {
            struct cluster *k = &clusters[c];
            int t = temp[k->zone]; unsigned want = k->level;
            if (t < 0) continue;
            if (t >= 95) want = 2;
            else if (t >= 85 && want < 1) want = 1;
            else if (want == 2 && t < 89) want = t >= 85 ? 1 : 0;
            else if (want == 1 && t < 79) want = 0;
            if (want != k->level) {
                k->level = want;
                uint32_t khz = capped(k, govern ? k->rung[k->gov_rung] : k->target);
                if (khz != k->sent && set_rate(k, khz) == 0) k->sent = khz;
                dlog("%s zone %s %d C: thermal step %u, %u MHz\n", k->name, zones[k->zone], t, want, khz / 1000);
            }
        }
        if (w >= 105) {
            for (unsigned c = 0; c < NCLUSTERS; c++) { struct cluster *k = &clusters[c]; k->level = 2; if (k->sent != k->safe && set_rate(k, k->safe) == 0) k->sent = k->safe; }
            dlog("hottest zone %d C: every cluster at its safe clock\n", w);
        }
        if (tick % 150 == 0) {
            dlog("30 s: big %d mid %d little %d g3d %d C; sent cl0 %u cl1 %u cl2 %u MHz; steps %u/%u/%u; ipc %llu/%llu answered\n",
                temp[0], temp[1], temp[2], temp[3], clusters[0].sent / 1000, clusters[1].sent / 1000, clusters[2].sent / 1000,
                clusters[0].level, clusters[1].level, clusters[2].level, b2l_dvfs_answered, b2l_dvfs_requests);
        }
    }
}

void
b2l_acpm_dvfs_start(void)
{
    thread_t thread;
    mode = 0;
    if (!PE_parse_boot_argn("b2l_dvfs", &mode, sizeof(mode)) || mode < 1 || mode > 2) {
        printf("B2L DVFS: disabled; clocks stay as S-Boot left them\n");
        return;
    }
    if (kernel_thread_start(dvfs_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}

/* For the LIVE reporter: last temperatures and requested clocks. */
void
b2l_acpm_dvfs_snapshot(int32_t temps[4], uint32_t sent_khz[3])
{
    for (unsigned z = 0; z < 4; z++) temps[z] = __atomic_load_n(&b2l_dvfs_last_temp[z], __ATOMIC_RELAXED);
    for (unsigned c = 0; c < NCLUSTERS; c++) sent_khz[c] = __atomic_load_n(&clusters[c].sent, __ATOMIC_RELAXED);
}
