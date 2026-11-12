/* SPDX-License-Identifier: BSD-2-Clause
 * Opt-in, stripped-pointer ARM64e development ABI. This does not authenticate
 * pointers, claim hardware PAC, or change kernel pointer/security contracts.
 */
#ifndef B2L_COMPAT_HOST_TEST
#include <arm64/b2l_arm64e_compat.h>
#include <arm64/b2l_early.h>
#include <arm64/proc_reg.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/misc_protos.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <string.h>
#include <mach/machine.h>
#include <mach/arm/thread_status.h>
#include <mach/arm/vm_param.h>
#include <mach/vm_region.h>
#include <pexpert/pexpert.h>
/* Source-public sys/random.h contract, without mixing BSD and Mach types. */
extern void read_frandom(void *, unsigned int);
#include <vm/vm_map_internal.h>
#include <vm/vm_map_xnu.h>
#include <vm/vm_map_lock_internal.h>
#include <vm/vm_object_internal.h>
#include <vm/memory_object_internal.h>
#include <vm/vm_protos_internal.h>
#include <sys/errno.h>
#include <kern/cpu_number.h>
extern void b2l_native_cache_observe(uint64_t,uint32_t);
#endif
#include "b2l_compat.h"
#include "b2l_fjcvtzs.h"
#include "b2l_pacga_qarma5.h"
#include "b2l_pacga_keyless.h"
extern uint64_t b2l_pacga_qarma5_asm(uint64_t, uint64_t, uint64_t, uint64_t);
extern uint64_t b2l_pacga_siphash(uint64_t, uint64_t, const uint64_t[2]);
/* Smallest AArch64 translation granule; execute permission is never finer. */
#define B2L_COMPAT_PAGE_SHIFT 12

static bool b2l_compat_requested;
/* Written only at TUNABLES, before any userspace object can be signed. */
static bool b2l_compat_siphash;
/* Mask result ABI: PACGA Xd = Xn & 0xffffffff00000000, no key or hash. It is
 * the only PACGA result the native cache may rewrite into one instruction.
 * Written at TUNABLES, or forced by the cache before load_init_program when
 * the installed root already carries rewritten PACGA sites. */
static bool b2l_compat_mask;
static bool b2l_compat_pair;
/* Precompiled-root mode: the host precompiler already rewrote every allowlisted
 * site, so the cache learner is idle and same-page execute checks are elided. */
static bool b2l_compat_precompiled;
/* Keyless PACGA: must match the EL0 routines the precompiler placed in the root. */
static bool b2l_compat_keyless;
/* Samples cover successful wrapper work only, not exception entry/return.
 * perf[0..3]: samples, wrapper/VM/hash ticks (1/1024 traps, low traffic). */
static uint64_t b2l_compat_perf[4];
struct b2l_compat_measurement { bool sample; uint64_t vm_ticks, hash_ticks, trapped_pc; };
/* Every-trap counters live in per-CPU cache lines: eight CPUs incrementing one
 * shared line on every trap made the counters a measurable share of the cost.
 * Migration between cpu_number() and the add only picks another slot; relaxed
 * atomics keep each slot exact. Snapshots sum the slots. */
#define B2L_COMPAT_SLOTS 16u
struct b2l_compat_slot {
    uint64_t traps, done, paired, pair_fallback, elided, lookups;
} __attribute__((aligned(128)));
static struct b2l_compat_slot b2l_compat_slots[B2L_COMPAT_SLOTS];
/* Last observed pid/pc, refreshed every 64th trap per CPU for diagnostics. */
static uint64_t b2l_compat_last[2];
static struct b2l_compat_slot *
b2l_compat_slot(void)
{
    return &b2l_compat_slots[(unsigned)cpu_number() & (B2L_COMPAT_SLOTS - 1u)];
}
static uint64_t
b2l_compat_sum(size_t field)
{
    uint64_t total = 0;
    for (unsigned i = 0; i < B2L_COMPAT_SLOTS; i++)
        total += __atomic_load_n((uint64_t *)((char *)&b2l_compat_slots[i] + field), __ATOMIC_RELAXED);
    return total;
}
#ifndef B2L_COMPAT_HOST_TEST
extern int proc_selfpid(void);
#endif
void b2l_arm64e_compat_snapshot(uint64_t values[4])
{
    values[0]=b2l_compat_sum(offsetof(struct b2l_compat_slot,traps));
    values[1]=b2l_compat_sum(offsetof(struct b2l_compat_slot,done));
    values[2]=__atomic_load_n(&b2l_compat_last[0],__ATOMIC_RELAXED);
    values[3]=__atomic_load_n(&b2l_compat_last[1],__ATOMIC_RELAXED);
}

void
b2l_arm64e_perf_snapshot(uint64_t values[6])
{
    for (unsigned i=0;i<4;i++) values[i]=__atomic_load_n(&b2l_compat_perf[i],__ATOMIC_RELAXED);
    values[4]=b2l_compat_sum(offsetof(struct b2l_compat_slot,paired));
    values[5]=b2l_compat_sum(offsetof(struct b2l_compat_slot,pair_fallback));
#ifndef B2L_COMPAT_HOST_TEST
    for (unsigned i=1;i<=3;i++) {
        uint64_t ns=0;absolutetime_to_nanoseconds(values[i],&ns);values[i]=ns;
    }
#endif
}

static void
b2l_compat_init(void)
{
    int requested = 0;
    b2l_compat_requested =
        PE_parse_boot_argn("b2l_arm64e_compat", &requested, sizeof(requested)) && requested == 1;
    int siphash = 0;
    b2l_compat_siphash = b2l_compat_requested &&
        PE_parse_boot_argn("b2l_pacga_siphash", &siphash, sizeof(siphash)) && siphash == 1;
    int mask = 0;
    b2l_compat_mask = b2l_compat_requested &&
        PE_parse_boot_argn("b2l_pacga_mask", &mask, sizeof(mask)) && mask == 1;
    int pair = 0;
    b2l_compat_pair = b2l_compat_requested &&
        PE_parse_boot_argn("b2l_pacga_pair", &pair, sizeof(pair)) && pair == 1;
    int precompiled = 0;
    b2l_compat_precompiled = b2l_compat_requested &&
        PE_parse_boot_argn("b2l_precompiled", &precompiled, sizeof(precompiled)) && precompiled == 1;
    int keyless = 0;
    b2l_compat_keyless = b2l_compat_precompiled &&
        PE_parse_boot_argn("b2l_pacga_keyless", &keyless, sizeof(keyless)) && keyless == 1;
    if (b2l_compat_requested) {
        /* TUNABLES runs before log_lock initialization. Use only the
         * already mapped, lock-free early framebuffer sink here. */
        const char *message = "B2L ARM64e: development stripped-pointer runtime enabled; hardware PAC unchanged\n";
        while (*message) b2l_early_putc(*message++);
        message = b2l_compat_mask ?
            "B2L PACGA: mask result ABI (Xn & 0xffffffff00000000); cache may rewrite PACGA\n" :
            b2l_compat_keyless ?
            "B2L PACGA: keyless (matches precompiled EL0 routines)\n" : b2l_compat_siphash ?
            "B2L PACGA: SipHash-2-4 experimental boot ABI; immutable boot key\n" :
            "B2L PACGA: QARMA5; immutable boot key\n";
        while (*message) b2l_early_putc(*message++);
        if (b2l_compat_pair) {
            message="B2L PACGA: bounded adjacent pair enabled; execute checks retained\n";
            while (*message) b2l_early_putc(*message++);
        }
        if (b2l_compat_precompiled) {
            message="B2L ARM64e: precompiled root mode; cache learning off, same-page execute checks elided\n";
            while (*message) b2l_early_putc(*message++);
        }
    }
}
STARTUP(TUNABLES, STARTUP_RANK_MIDDLE, b2l_compat_init);

bool
b2l_arm64e_compat_enabled(void)
{
    return b2l_compat_requested;
}

bool
b2l_arm64e_precompiled(void)
{
    return b2l_compat_precompiled;
}

void
b2l_arm64e_precompiled_snapshot(uint64_t values[2])
{
    values[0]=b2l_compat_sum(offsetof(struct b2l_compat_slot,elided));
    values[1]=b2l_compat_sum(offsetof(struct b2l_compat_slot,lookups));
}

const char *
b2l_arm64e_pacga_name(void)
{
    if (!b2l_compat_requested) return "disabled";
    if (b2l_compat_mask) return "mask";
    if (b2l_compat_keyless) return "keyless";
    return b2l_compat_siphash ? "siphash24" : "qarma5";
}

bool
b2l_arm64e_pacga_mask(void)
{
    return b2l_compat_requested && b2l_compat_mask;
}

/* Called only before the first userspace instruction, when durable cache
 * records show that root PACGA sites were already rewritten to the mask
 * result. Trapped and rewritten sites must agree for the whole boot. */
void
b2l_arm64e_force_pacga_mask(void)
{
    if (!b2l_compat_requested || b2l_compat_mask) return;
    b2l_compat_mask = true;
    printf("B2L PACGA: mask result ABI forced by existing native cache records\n");
}

LCK_GRP_DECLARE(b2l_compat_lck_grp, "b2l_arm64e_compat");
LCK_MTX_DECLARE(b2l_compat_key_lock, &b2l_compat_lck_grp);
static uint64_t b2l_compat_key[2];
static bool b2l_compat_key_ready;

/* No top-byte/PAC guessing. Every access stays in the current EL0 address
 * space and remains faultable through copyin. End is exclusive. */
static bool
b2l_user_range(uint64_t address, unsigned width)
{
    return width && address && address < MACH_VM_MAX_ADDRESS &&
           width <= MACH_VM_MAX_ADDRESS - address;
}

static int
b2l_compat_read(uint64_t address, unsigned width, uint64_t *out, void *opaque __unused)
{
    uint64_t value = 0;
    if (!out || (width != 1 && width != 2 && width != 4 && width != 8) ||
        !b2l_user_range(address, width) || copyin(address, &value, width)) return -1;
    *out = value; /* Both kernel and target are little endian. */
    return 0;
}

static int
b2l_compat_read_acquire(uint64_t address, unsigned width, uint64_t *out, void *opaque)
{
    /* Fault-recoverable scalar atomics, followed by a stronger acquire
     * barrier. Byte/halfword use their containing aligned user word; it cannot
     * cross a page and remains inside the same process mapping. */
    if (!out || (width != 1 && width != 2 && width != 4 && width != 8) ||
        !b2l_user_range(address, width) || (address & (width - 1u))) return -1;
    int error;
    if (width == 8) {
        error = copyin_atomic64(address, out);
    } else {
        uint32_t word = 0;
        error = copyin_atomic32(address & ~UINT64_C(3), &word);
        if (!error) *out = (word >> ((address & 3u) * 8u)) &
                          (width == 4 ? UINT32_MAX : ((1u << (width * 8u)) - 1u));
    }
    if (!error) {
#ifndef B2L_COMPAT_HOST_TEST
        __asm__ volatile("dmb ish" ::: "memory");
#else
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
    }
    return error;
}

static int
b2l_compat_validate_pc(uint64_t address, void *opaque)
{
    /* Only alignment and EL0 range are checked here. Executability is not
     * looked up: the trapped PC was fetched by the CPU, and a branch target
     * is handed back in ELR exactly as hardware would do for a native BLR/RET.
     * A non-executable or unmapped target then takes the ordinary EL0
     * instruction-fetch fault at that PC. Nothing is dereferenced, no map
     * lock is taken and no permission is cached. Earlier builds spent most of
     * every trap in this lookup (sampled vm_ns about 6 us per trap). */
    struct b2l_compat_measurement *measurement=opaque;
    uint64_t began=measurement && measurement->sample ? mach_absolute_time() : 0;
    int result = ((address & 3u) || !b2l_user_range(address, 4u)) ? -1 : 0;
    if (measurement && measurement->sample) measurement->vm_ticks += mach_absolute_time()-began;
    return result;
}

static int
b2l_compat_pacga(uint64_t a, uint64_t b, uint64_t *out, void *opaque)
{
    if (!out || !b2l_compat_requested) return -1;
    struct b2l_compat_measurement *measurement=opaque;
    uint64_t began=measurement && measurement->sample ? mach_absolute_time() : 0;
    if (b2l_compat_mask || b2l_compat_keyless) {
        /* mask: same function as the cache's AND-immediate rewrite;
         * keyless: same function as the precompiler's EL0 routines. No key. */
        *out = b2l_compat_mask ? (a & UINT64_C(0xffffffff00000000)) : b2l_pacga_keyless(a, b);
        if (measurement && measurement->sample) measurement->hash_ticks += mach_absolute_time()-began;
        return 0;
    }
    /* First invocation is an EL0 trap after BSD startup and PRNG registration.
     * One key and one algorithm cover every task and CPU for the entire boot.
     * Release publishes the completed key; acquire readers never need a mutex.
     * The key is never replaced, persisted or printed. */
    if (!__atomic_load_n(&b2l_compat_key_ready, __ATOMIC_ACQUIRE)) {
        lck_mtx_lock(&b2l_compat_key_lock);
        if (!__atomic_load_n(&b2l_compat_key_ready, __ATOMIC_RELAXED)) {
            read_frandom(b2l_compat_key, sizeof(b2l_compat_key));
            __atomic_store_n(&b2l_compat_key_ready, true, __ATOMIC_RELEASE);
        }
        lck_mtx_unlock(&b2l_compat_key_lock);
    }
    if (b2l_compat_siphash) {
        /* BSD owns the SipHash context; do not mix its types into Mach VM.
         * This experimental result ABI does not reproduce QARMA outputs. */
        *out = b2l_pacga_siphash(a, b, b2l_compat_key);
    } else {
        *out = b2l_pacga_qarma5_asm(a, b,
            b2l_compat_key[0], b2l_compat_key[1]);
    }
    if (measurement && measurement->sample) measurement->hash_ticks += mach_absolute_time()-began;
    return 0;
}

static bool
b2l_compat_is_pacga(uint32_t instruction)
{
    return (instruction & UINT32_C(0xffe0fc00)) == UINT32_C(0x9ac03000);
}

#ifndef B2L_COMPAT_HOST_TEST
extern pmap_t get_task_pmap(task_t);
extern ppnum_t pmap_find_phys(pmap_t, addr64_t);

/* JavaScriptCore converts doubles with it: one trap per conversion on these cores. */
static void
b2l_compat_fjcvtzs(arm_saved_state_t *state, uint32_t instruction)
{
    arm_neon_saved_state64_t *neon = neon_state64(current_thread()->machine.uNeon);
    unsigned rd = instruction & 31, rn = (instruction >> 5) & 31;
    int exact;
    uint32_t value = b2l_fjcvtzs((uint64_t)neon->v.q[rn], &exact);

    if (rd == 29) {
        set_saved_state_fp(state, value);
    } else if (rd == 30) {
        set_user_saved_state_lr(state, value);
    } else if (rd != 31) {
        set_user_saved_state_reg(state, rd, value);
    }
    uint32_t flags = get_saved_state_cpsr(state) & ~UINT32_C(0xf0000000);
    set_user_saved_state_cpsr(state, flags | (exact ? UINT32_C(0x40000000) : 0));
    set_user_saved_state_pc(state, get_saved_state_pc(state) + 4);
}

/*
 * iOS's sys_icache_invalidate is only ic ivau: Apple's cores see their own data
 * cache from the instruction side, and these do not. So JIT code can reach the
 * fetch unit as the bytes that were there before, which traps as undefined while
 * memory holds a valid instruction. Clean the page and run it again, once per
 * thread and address, so a truly undefined instruction still raises its signal.
 */
static bool
b2l_compat_stale_code(arm_saved_state_t *state, uint32_t instruction)
{
    static uint64_t seen[64];
    static unsigned logged;
    uint64_t pc = get_saved_state_pc(state);
    uint64_t key = pc ^ (thread_tid(current_thread()) << 48);
    uint64_t *entry = &seen[(pc >> 2) & 63];

    if (__atomic_exchange_n(entry, key, __ATOMIC_RELAXED) == key) {
        __atomic_store_n(entry, 0, __ATOMIC_RELAXED);
        return false;
    }
    ppnum_t page = pmap_find_phys(get_task_pmap(current_task()), trunc_page_64(pc));
    if (page == 0 || !isphysmem(ptoa(page))) {
        return false;
    }
    vm_offset_t alias = phystokv(ptoa(page));
    for (vm_offset_t line = alias; line < alias + PAGE_SIZE; line += 64) {
        __asm__ volatile ("dc cvau, %0" : : "r"(line));
    }
    __asm__ volatile ("dsb ish\n ic ialluis\n dsb ish\n isb");
    if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 16) {
        b2l_early_trace("B2L COMPAT: stale code retried pc=0x%llx insn=0x%x\n", pc, instruction);
    }
    return true;
}
#endif

bool
b2l_arm64e_compat_emulate(arm_saved_state_t *state, uint32_t instruction)
{
    static unsigned wrapper_samples;
    bool trace = wrapper_samples < 2;
    if (trace) { ++wrapper_samples; b2l_early_trace("B2L COMPAT: wrapper entered insn=0x%x\n", instruction); }
    if (!b2l_compat_requested || !state || !is_saved_state64(state) ||
        !PSR64_IS_USER64(get_saved_state_cpsr(state)) ||
        !b2l_arm64e_current_task()) return false;
    if (trace) b2l_early_trace("B2L COMPAT: process and EL0 gates passed; pc checks are range-only\n");
#ifndef B2L_COMPAT_HOST_TEST
    if ((instruction & B2L_FJCVTZS_MASK) == B2L_FJCVTZS_VALUE) {
        b2l_compat_fjcvtzs(state, instruction);
        return true;
    }
#endif
    struct b2l_compat_slot *slot=b2l_compat_slot();
    uint64_t ticket=__atomic_fetch_add(&slot->traps,1,__ATOMIC_RELAXED);
    struct b2l_compat_measurement measurement={.sample=(ticket & 1023u)==0,
        .trapped_pc=get_saved_state_pc(state)};
    uint64_t began=measurement.sample ? mach_absolute_time() : 0;
    if (!(ticket & 63u)) {
#ifndef B2L_COMPAT_HOST_TEST
        __atomic_store_n(&b2l_compat_last[0],(uint64_t)proc_selfpid(),__ATOMIC_RELAXED);
#endif
        __atomic_store_n(&b2l_compat_last[1],get_saved_state_pc(state),__ATOMIC_RELAXED);
    }
    struct b2l_compat_ctx context = {0};
    for (unsigned i = 0; i < 29; ++i) context.x[i] = get_saved_state_reg(state, i);
    context.x[29] = get_saved_state_fp(state);
    context.x[30] = get_saved_state_lr(state);
    context.sp = get_saved_state_sp(state);
    context.pc = get_saved_state_pc(state);
    context.cpsr = get_saved_state_cpsr(state);
    if (b2l_compat_validate_pc(context.pc, &measurement)) return false;
    const struct b2l_compat_ops operations = {
        .mode = B2L_COMPAT_MODE_STRIPPED,
        .read = b2l_compat_read, .read_acquire = b2l_compat_read_acquire,
        .validate_pc = b2l_compat_validate_pc, .pacga = b2l_compat_pacga,
        .opaque = &measurement
    };
    if (trace) b2l_early_trace("B2L COMPAT: decoder begin\n");
    enum b2l_compat_result result = b2l_compat_emulate(instruction, &context, &operations);
    if (trace) b2l_early_trace("B2L COMPAT: decoder result=%u\n", result);
    if (result != B2L_COMPAT_OK) {
        b2l_early_trace("B2L COMPAT: rejected result=%u pc=0x%llx x8=0x%llx x16=0x%llx lr=0x%llx\n",
            result, context.pc, context.x[8], context.x[16], context.x[30]);
#ifndef B2L_COMPAT_HOST_TEST
        return b2l_compat_stale_code(state, instruction);
#else
        return false;
#endif
    }
    if (b2l_compat_pair && !(context.cpsr & PSR64_SS) &&
        current_thread()->machine.DebugData == NULL && b2l_compat_is_pacga(instruction)) {
        /* Fetch only one following instruction through recoverable EL0 copyin.
         * Reuse the actual decoder for all operand/ZR/SP and next-PC checks.
         * Failure to consume the second instruction commits ONLY the first;
         * the next EL0 instruction then executes/traps normally. No loop,
         * branch, data operation or speculative cache entry is consumed.
         * Debug contexts and software single-step retain one-instruction traps. */
        uint32_t following=0;
        if (copyin_atomic32(context.pc,&following)) {
            __atomic_fetch_add(&slot->pair_fallback,1,__ATOMIC_RELAXED);
        } else if (b2l_compat_is_pacga(following)) {
            struct b2l_compat_ctx candidate=context;
            if (b2l_compat_emulate(following,&candidate,&operations)==B2L_COMPAT_OK) {
                context=candidate;
                __atomic_fetch_add(&slot->paired,1,__ATOMIC_RELAXED);
            } else {
                __atomic_fetch_add(&slot->pair_fallback,1,__ATOMIC_RELAXED);
            }
        }
    }
#ifndef B2L_COMPAT_HOST_TEST
    /* Enqueue only after successful emulation, using the original PC. A
     * precompiled root has nothing left for the learner to convert. */
    if (!b2l_compat_precompiled) b2l_native_cache_observe(get_saved_state_pc(state),instruction);
#endif
    __atomic_fetch_add(&slot->done,1,__ATOMIC_RELAXED);
    for (unsigned i = 0; i < 29; ++i) set_user_saved_state_reg(state, i, context.x[i]);
    set_saved_state_fp(state, context.x[29]);
    set_user_saved_state_lr(state, context.x[30]);
    set_saved_state_sp(state, context.sp);
    set_user_saved_state_pc(state, context.pc);
    if (measurement.sample) {
        __atomic_fetch_add(&b2l_compat_perf[0],1,__ATOMIC_RELAXED);
        __atomic_fetch_add(&b2l_compat_perf[1],mach_absolute_time()-began,__ATOMIC_RELAXED);
        __atomic_fetch_add(&b2l_compat_perf[2],measurement.vm_ticks,__ATOMIC_RELAXED);
        __atomic_fetch_add(&b2l_compat_perf[3],measurement.hash_ticks,__ATOMIC_RELAXED);
    }
    /* Decoder preserves CPSR; never let emulation modify mode/masks/NZCV. */
    return true;
}

#ifndef B2L_COMPAT_HOST_TEST

#if defined(B2L)
/* Read-only address->file resolution for the background cache learner.
 * Parent-address translation handles transparent and constant submaps; shadow
 * and backing-pager offsets are accumulated exactly as code-signing mapping.
 * The caller owns a map reference. No VFS I/O occurs while a VM lock is held.
 */
extern int vnode_getwithvid(struct vnode *,uint32_t);
extern uint32_t vnode_vid(struct vnode *);
int
b2l_cache_resolve(vm_map_t source,uint64_t address,struct vnode **out,uint64_t *file_offset)
{
 VM_MAP_LOCK_CTX_DECLARE(ctx);
 vm_map_t map=source;vm_map_entry_t entry;vm_object_t object,next;
 uint64_t offset;struct vnode *vp=NULL;uint32_t vid=0;int error=EINVAL;
 *out=NULL;*file_offset=0;
 if((address&3)||address>UINT64_MAX-4)return EINVAL;
 if(vm_map_range_sh_lock(ctx,&map,address,address+4,
    VMRL_SH_STREAM_NO_HOLES|VMRL_SH_DESCEND_INTO_CONSTANT)!=KERN_SUCCESS)return EFAULT;
 entry=vm_map_range_stream_next(ctx);
 if(!entry||entry->is_sub_map||!(entry->protection&VM_PROT_EXECUTE)||VME_OBJECT(entry)==VM_OBJECT_NULL)goto unlock;
 offset=vm_map_lock_ctx_offset_for_parent_address(ctx,address);
 object=VME_OBJECT(entry);vm_object_lock_shared(object);
 for(unsigned depth=0;depth<128;depth++){
  if(object->shadow!=VM_OBJECT_NULL){
   if(offset>UINT64_MAX-object->vo_shadow_offset)break;
   offset+=object->vo_shadow_offset;next=object->shadow;
   vm_object_lock_shared(next);vm_object_unlock(object);object=next;continue;
  }
  if(object->internal||!object->pager_ready||object->terminating||!object->alive||object->pager==MEMORY_OBJECT_NULL||offset>UINT64_MAX-object->paging_offset)break;
  offset+=object->paging_offset;
  vm_object_offset_t backing_offset;
  if(memory_object_backing_object(object->pager,offset,&next,&backing_offset)){
   if(next==VM_OBJECT_NULL||next==object)break;
   vm_object_lock_shared(next);vm_object_unlock(object);object=next;offset=backing_offset;continue;
  }
  vp=vnode_pager_lookup_vnode(object->pager);
  if(vp){vid=vnode_vid(vp);error=0;}
  break;
 }
 vm_object_unlock(object);
 unlock:vm_map_range_sh_unlock(ctx,&map);
 if(error||!vp)return EINVAL;
 if(vnode_getwithvid(vp,vid))return EAGAIN;
 *out=vp;*file_offset=offset;return 0;
}
#endif

#endif
