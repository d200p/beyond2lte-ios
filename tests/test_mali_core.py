#!/usr/bin/env python3
"""Run the Mali bring-up, page table and job builder against a simulated GPU.

The simulator enforces what the hardware requires: no job before the L2,
tiler and shaders report ready, no translation without a valid AS0 update, and
memory reached only through the 4-level table the code built. It executes the
clear job by decoding the descriptors, so a wrong offset leaves wrong pixels.
Device evidence that the descriptors themselves are right comes from
tools/b2l-mali-probe.c, which runs the same builder on the real GPU.
"""
from pathlib import Path
import subprocess, tempfile
ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "xnu-overlay/iokit/Kernel/arm"
TEST = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "b2l_mali_core.h"
#define MEM_PAGES 8192u
#define PA_BASE 0x880000000ull
static uint8_t *mem;
static uint8_t *pa_ptr(uint64_t pa) {
 assert(pa >= PA_BASE && pa < PA_BASE + MEM_PAGES * 4096ull);
 return mem + (pa - PA_BASE);
}
struct gpu {
 uint32_t regs[0x5000 / 4], l2, tiler, shader, irq, job_irq, reset_seen, jobs, tiler_jobs;
 uint64_t transtab; int as_valid, hang, fault_va, ignore_reset, ignore_shader;
 uint32_t reads_before_ready;
} g;
static int walk(uint64_t va, uint64_t *pa, int write) {
 uint64_t table = g.transtab;
 for (int level = 0; level < 4; level++) {
  uint64_t e = ((uint64_t *)pa_ptr(table))[(va >> (39 - 9 * level)) & 511];
  if ((e & 3) != 3) return 0;
  if (level == 3) {
   assert(e & (1ull << 10)); assert((e & (3ull << 6)) == (1ull << 6));
   assert(e & (1ull << 54)); assert(!(e & (7ull << 2)));
   *pa = (e & B2L_MALI_PT_ADDRESS) | (va & 0xfff); return 1;
  }
  table = e & B2L_MALI_PT_ADDRESS;
 }
 (void)write; return 0;
}
static int gpu_read(uint64_t va, void *p, unsigned n) {
 uint64_t pa; if (!walk(va, &pa, 0)) return 0;
 memcpy(p, pa_ptr(pa), n); return 1;
}
static int gpu_write32(uint64_t va, uint32_t v) {
 uint64_t pa; if (!walk(va, &pa, 1)) return 0;
 memcpy(pa_ptr(pa), &v, 4); return 1;
}
/* Decode and execute the fragment job as the hardware would. */
static int execute(uint64_t job_va) {
 uint32_t job[12], fbd[16], rtd[16];
 if (!gpu_read(job_va, job, sizeof job)) return 0;
 assert((job[4] & 1) && ((job[4] >> 1) & 0x7f) == 9 && job[4] >> 16 == 1);
 uint64_t ptr = job[10] | ((uint64_t)job[11] << 32);
 assert(ptr & 1); assert(!(ptr & 2)); assert(((ptr >> 2) & 15) == 0);
 uint64_t fbd_va = ptr & ~63ull;
 if (!gpu_read(fbd_va, fbd, sizeof fbd) || !gpu_read(fbd_va + 128, rtd, sizeof rtd)) return 0;
 uint32_t w = (fbd[8] & 0xffff) + 1, h = (fbd[8] >> 16) + 1;
 assert(fbd[10] == fbd[8]);
 assert((job[9] & 0xfff) == (w - 1) >> 4 && ((job[9] >> 16) & 0xfff) == (h - 1) >> 4);
 assert(((fbd[11] >> 9) & 15) == 8 && (fbd[11] >> 24) == 1);
 uint64_t sv = fbd[4] | ((uint64_t)fbd[5] << 32); uint16_t s[2];
 if (!gpu_read(sv, s, 4)) return 0;
 assert(s[0] == 128 && s[1] == 128);
 assert(rtd[0] >> 26 == 1 && (rtd[1] & 1) && ((rtd[1] >> 3) & 31) == 19 && ((rtd[1] >> 8) & 15) == 2);
 assert(((rtd[1] >> 16) & 0xfff) == 0x688 && (rtd[1] >> 31));
 uint64_t rt = rtd[8] | ((uint64_t)rtd[9] << 32);
 assert(rtd[11] == rtd[10] * h);
 for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++)
  if (!gpu_write32(rt + y * rtd[10] + x * 4, rtd[12])) return 0;
 return 1;
}
static uint32_t r(void *c, uint32_t o) {
 (void)c; assert(o < 0x5000 && !(o & 3));
 switch (o) {
 case B2L_MALI_ID: return B2L_MALI_GPU_ID;
 case B2L_MALI_L2_PRESENT: return 1;
 case B2L_MALI_TILER_PRESENT: return 1;
 case B2L_MALI_SHADER_PRESENT: return 0x770077;
 case B2L_MALI_L2_READY: return g.l2;
 case B2L_MALI_TILER_READY: return g.tiler;
 case B2L_MALI_SHADER_READY: return g.shader;
 case B2L_MALI_GPU_IRQ_RAWSTAT: return g.irq;
 case B2L_MALI_JOB_IRQ_RAWSTAT: return g.job_irq;
 }
 return g.regs[o / 4];
}
static void w(void *c, uint32_t o, uint32_t v) {
 (void)c; assert(o < 0x5000 && !(o & 3));
 g.regs[o / 4] = v;
 switch (o) {
 case B2L_MALI_GPU_COMMAND:
  if (v == B2L_MALI_CMD_SOFT_RESET) {
   memset(g.regs, 0, sizeof g.regs); g.l2 = g.tiler = g.shader = 0; g.as_valid = 0;
   g.irq = g.ignore_reset ? 0 : B2L_MALI_IRQ_RESET_DONE; g.reset_seen++;
  }
  break;
 case B2L_MALI_GPU_IRQ_CLEAR: g.irq &= ~v; break;
 case B2L_MALI_JOB_IRQ_CLEAR: g.job_irq &= ~v; break;
 case B2L_MALI_L2_PWRON: g.l2 |= v; break;
 case B2L_MALI_TILER_PWRON: assert(g.l2); g.tiler |= v; break;
 case B2L_MALI_SHADER_PWRON: assert(g.l2 && g.tiler); if (!g.ignore_shader) g.shader |= v; break;
 case B2L_MALI_SHADER_PWROFF: g.shader &= ~v; break;
 case B2L_MALI_TILER_PWROFF: assert(!g.shader); g.tiler &= ~v; break;
 case B2L_MALI_L2_PWROFF: assert(!g.shader && !g.tiler); g.l2 &= ~v; break;
 case B2L_MALI_AS0 + B2L_MALI_AS_COMMAND:
  assert(v == B2L_MALI_AS_CMD_UPDATE);
  assert((g.regs[(B2L_MALI_AS0 + B2L_MALI_AS_TRANSCFG_LO) / 4] & 15) == 6);
  g.transtab = g.regs[(B2L_MALI_AS0 + B2L_MALI_AS_TRANSTAB_LO) / 4] |
   ((uint64_t)g.regs[(B2L_MALI_AS0 + B2L_MALI_AS_TRANSTAB_HI) / 4] << 32);
  g.as_valid = 1;
  break;
 case B2L_MALI_JS0 + B2L_MALI_JS_COMMAND_NEXT: {
  assert(v == B2L_MALI_JS_START && g.shader && g.tiler && g.l2 && g.as_valid);
  uint32_t cfg = g.regs[(B2L_MALI_JS0 + B2L_MALI_JS_CONFIG_NEXT) / 4];
  assert(!(cfg & 15) && (cfg & (1u << 10)) && ((cfg >> 8) & 3) == 3 && ((cfg >> 12) & 3) == 3);
  assert(g.regs[(B2L_MALI_JS0 + B2L_MALI_JS_AFFINITY_NEXT_LO) / 4] == g.shader);
  if (g.hang) break;
  uint64_t va = g.regs[(B2L_MALI_JS0 + B2L_MALI_JS_HEAD_NEXT_LO) / 4] |
   ((uint64_t)g.regs[(B2L_MALI_JS0 + B2L_MALI_JS_HEAD_NEXT_HI) / 4] << 32);
  g.jobs++;
  if (execute(va)) g.job_irq |= B2L_MALI_JOB_DONE(0);
  else { g.job_irq |= B2L_MALI_JOB_FAILED(0); g.regs[(B2L_MALI_JS0 + B2L_MALI_JS_STATUS) / 4] = 0x42; }
  break; }
 case B2L_MALI_JS0 + B2L_MALI_JS_STRIDE + B2L_MALI_JS_COMMAND_NEXT: {
  assert(v == B2L_MALI_JS_START && g.shader && g.tiler && g.l2 && g.as_valid);
  uint32_t cfg = g.regs[(B2L_MALI_JS0 + B2L_MALI_JS_STRIDE + B2L_MALI_JS_CONFIG_NEXT) / 4];
  assert(!(cfg & 15) && (cfg & (1u << 10)));
  uint64_t head = g.regs[(B2L_MALI_JS0 + B2L_MALI_JS_STRIDE + B2L_MALI_JS_HEAD_NEXT_LO) / 4];
  uint32_t word; assert(gpu_read(head, &word, 4));
  g.tiler_jobs++; g.job_irq |= B2L_MALI_JOB_DONE(1);
  break; }
 case B2L_MALI_JS0 + B2L_MALI_JS_COMMAND:
  assert(v == B2L_MALI_JS_HARD_STOP);
  g.job_irq |= B2L_MALI_JOB_FAILED(0);
  break;
 }
}
static void d(void *c, uint32_t us) { (void)c; (void)us; }
static const struct b2l_mali_io io = { NULL, r, w, d };
static uint64_t alloc_next;
static uint64_t pa_alloc(unsigned pages) { uint64_t p = alloc_next; alloc_next += pages * 4096ull; return p; }
int main(void) {
 mem = calloc(MEM_PAGES, 4096); assert(mem); alloc_next = PA_BASE;
 const unsigned W = 1440, H = 2904, S = W * 4, TABLES = 16;
 uint64_t tables = pa_alloc(TABLES), jobpa = pa_alloc(1);
 struct b2l_mali_pt pt;
 assert(!b2l_mali_pt_init(&pt, (uint64_t *)pa_ptr(tables), tables | 8, TABLES));
 assert(b2l_mali_pt_init(&pt, (uint64_t *)pa_ptr(tables), tables, TABLES));
 const uint64_t JOB = 0x10000000, RT = 0x20000000;
 assert(!b2l_mali_pt_map(&pt, JOB + 1, jobpa));
 assert(!b2l_mali_pt_map(&pt, JOB, jobpa + 1));
 assert(!b2l_mali_pt_map(&pt, 1ull << 48, jobpa));
 assert(!b2l_mali_pt_map(&pt, JOB, 1ull << 48));
 assert(b2l_mali_pt_map(&pt, JOB, jobpa));
 unsigned pages = (S * H + 4095) / 4096;
 /* Scattered backing: reverse order, so linear mapping bugs show as bad pixels. */
 uint64_t rtpa = pa_alloc(pages);
 for (unsigned i = 0; i < pages; i++)
  assert(b2l_mali_pt_map(&pt, RT + i * 4096ull, rtpa + (pages - 1 - i) * 4096ull));
 /* 16 tables hold this space (1 root + 1 L1 + 1 L2 + L3s); exhaustion is refused. */
 assert(pt.used <= TABLES);
 struct b2l_mali_pt tiny; uint64_t *tt = calloc(2, 4096);
 assert(b2l_mali_pt_init(&tiny, tt, 0x1000, 2));
 assert(!b2l_mali_pt_map(&tiny, 0, 0x2000));
 /* Unmapping clears only the leaf, so a later map of that VA reuses the tables. */
 /* Only code pages may execute; both kinds stay readable and writable. */
 uint64_t ct[512 * 4] = {0}, *leaf;
 struct b2l_mali_pt code;
 assert(b2l_mali_pt_init(&code, ct, 0x100000, 4));
 assert(b2l_mali_pt_map(&code, 0x1000, 0x5000));
 assert(b2l_mali_pt_map_code(&code, 0x2000, 0x6000));
 leaf = ct + 3 * 512;
 assert(leaf[1] & B2L_MALI_PT_EXECUTE && !(leaf[2] & B2L_MALI_PT_EXECUTE));
 assert((leaf[2] | B2L_MALI_PT_EXECUTE) == (0x6000 | B2L_MALI_PT_PAGE));
 unsigned tables_before = code.used;
 b2l_mali_pt_unmap(&code, 0x2000);
 assert(!leaf[2] && leaf[1]);
 b2l_mali_pt_unmap(&code, 0x40000000);      /* never mapped: nothing happens */
 assert(b2l_mali_pt_map(&code, 0x2000, 0x7000) && code.used == tables_before && leaf[2]);

 assert(b2l_mali_soft_reset(&io));
 assert(g.regs[B2L_MALI_SHADER_CONFIG / 4] & B2L_MALI_SC_TLS_HASH);
 assert((g.regs[B2L_MALI_JM_CONFIG / 4] >> 16) == 0xf);
 assert(b2l_mali_power_on(&io) == NULL);
 assert(b2l_mali_set_as0(&io, tables));
 for (unsigned i = 0; i < pages * 4096 / 4; i++) ((uint32_t *)pa_ptr(rtpa))[i] = 0x5aa55aa5;
 b2l_mali_clear_job(pa_ptr(jobpa), JOB, RT, W, H, S, 0xff2040c0);
 assert(b2l_mali_run_job(&io, B2L_MALI_SLOT_FRAGMENT, JOB, 1000) == 1);
 for (unsigned i = 0; i < S * H / 4; i++) {
  uint32_t v; assert(gpu_read(RT + i * 4ull, &v, 4)); assert(v == 0xff2040c0);
 }
 /* The tail of the last page lies beyond the surface and stays untouched. */
 { uint32_t v; assert(gpu_read(RT + S * H, &v, 4)); assert(v == 0x5aa55aa5); }
 assert(g.jobs == 1);

 /* A job that reads an unmapped address reports the JS_STATUS fault. */
 assert(b2l_mali_run_job(&io, B2L_MALI_SLOT_FRAGMENT, 0x30000000, 1000) == 0x42);
 /* The tiler slot reports through its own interrupt bits. */
 assert(b2l_mali_run_job(&io, B2L_MALI_SLOT_TILER, JOB, 1000) == 1);
 assert(g.tiler_jobs == 1);
 /* A hung job is hard-stopped and reported as a timeout. */
 g.hang = 1;
 assert(b2l_mali_run_job(&io, B2L_MALI_SLOT_FRAGMENT, JOB, 100) == 0);
 g.hang = 0;
 b2l_mali_power_off(&io);
 assert(!g.l2 && !g.tiler && !g.shader);

 /* Bounded failures: reset that never completes, shaders that never power. */
 g.ignore_reset = 1;
 assert(!b2l_mali_soft_reset(&io));
 g.ignore_reset = 0;
 assert(b2l_mali_soft_reset(&io));
 g.ignore_shader = 1;
 const char *failed = b2l_mali_power_on(&io);
 assert(failed && !strcmp(failed, "shader"));
 puts("PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "mali_test.c"
    src.write_text(TEST)
    exe = Path(tmp) / "mali_test"
    subprocess.run(["clang", "-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(CORE), str(src), "-o", str(exe)], check=True)
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    assert run.returncode == 0 and run.stdout.strip() == "PASS", run.stdout + run.stderr[-1500:]
source = (CORE / "B2LMali.cpp").read_text()
# This stage must stay read-only towards the PMU: only the status page is
# mapped, and only for input. Secure monitor calls and CMU addresses stay out.
assert "kIODirectionIn)" in source and "B2L_GPU_PMU_PAGE, 4096, kIODirectionIn)" in source
for forbidden in ("0x18400000", "0x1A240000", "0x1a240000", "smc #", "__smc"):
    assert forbidden not in source, forbidden
print("mali core tests passed")
