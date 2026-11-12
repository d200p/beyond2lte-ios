#!/usr/bin/env python3
"""Run the ACPM client against a simulated firmware in SRAM.

The firmware answers on the same queues and in the same layout as the real one,
including replies that arrive out of order. Layouts come from the public Exynos
9820 kernel; real behaviour is judged on the device by the clock it reaches.
"""
from pathlib import Path
import subprocess, tempfile
ROOT = Path(__file__).resolve().parents[1]
TEST = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "b2l_acpm.h"
static uint32_t sram[B2L_ACPM_SRAM_BYTES / 4];
static uint32_t pending_cleared, rung, silent, rate[B2L_DVFS_COUNT], early_reply, full_queue, bad_ret;
#define S(o) sram[(o) / 4]
enum { TABLE = 0x8000, TXQ = 0x9000, RXQ = 0xa000, ELEM = 16, LEN = 8 };
static uint32_t rd(void *c, uint32_t o) { (void)c; assert(o < B2L_ACPM_SRAM_BYTES && !(o & 3)); return S(o); }
static void wr(void *c, uint32_t o, uint32_t v) { (void)c; assert(o < B2L_ACPM_SRAM_BYTES && !(o & 3)); S(o) = v; }
static uint32_t chan(uint32_t id) { return TABLE + id * B2L_ACPM_CHANNEL_BYTES; }
static void reply(uint32_t id, const uint32_t *m) {
 uint32_t c = chan(id), f = S(S(c + B2L_ACPM_CH_RX_FRONT) + 0) ;
 uint32_t base = S(c + B2L_ACPM_CH_RX_BASE);
 memcpy(&S(base + f * ELEM), m, ELEM);
 S(S(c + B2L_ACPM_CH_RX_FRONT)) = (f + 1) % LEN;
}
/* The firmware, on a doorbell: take every queued request and answer it. */
static void apm(uint32_t id) {
 uint32_t c = chan(id);
 while (S(S(c + B2L_ACPM_CH_TX_REAR)) != S(S(c + B2L_ACPM_CH_TX_FRONT))) {
  uint32_t r = S(S(c + B2L_ACPM_CH_TX_REAR)), m[4];
  memcpy(m, &S(S(c + B2L_ACPM_CH_TX_BASE) + r * ELEM), ELEM);
  S(S(c + B2L_ACPM_CH_TX_REAR)) = (r + 1) % LEN;
  if (silent) continue;
  if (id == B2L_ACPM_CHANNEL_DVFS) {
   uint32_t d = m[0] & 0xffff;
   assert(d < B2L_DVFS_COUNT);
   if (m[2] == B2L_ACPM_FREQ_SET) rate[d] = m[1];
   else { assert(m[2] == B2L_ACPM_FREQ_GET); m[1] = rate[d]; }
  } else {
   assert((m[1] & 0xff) == B2L_ACPM_TMU_READ || (m[1] & 0xff) == B2L_ACPM_TMU_INIT);
   m[1] = (m[1] & 0xff) | ((m[1] >> 16 & 0xff) << 16) | ((40u + (m[1] >> 16 & 0xff)) << 24) | (bad_ret << 8);
  }
  if (early_reply) { uint32_t other[4] = { 0x3f0000, 1, 2, 3 }; reply(id, other); early_reply = 0; }
  reply(id, m);
 }
}
static void mb(void *c, uint32_t o, uint32_t v) {
 (void)c;
 if (o == B2L_ACPM_INTGR0) { rung++; apm(__builtin_ctz(v >> 16)); }
 else if (o == B2L_ACPM_INTCR1) pending_cleared++;
}
static void dl(void *c, uint32_t us) { (void)c; (void)us; }
static const struct b2l_acpm_io io = { NULL, rd, wr, mb, dl };
static uint32_t rate_of(struct b2l_acpm_channel *d, uint32_t domain) {
 uint32_t reply[4];
 return b2l_acpm_get_rate(&io, d, domain, reply) ? reply[1] : 0;
}
static void build(uint32_t id) {
 uint32_t c = chan(id), q = 0x100 * id;
 S(c + B2L_ACPM_CH_ID) = id;
 S(c + B2L_ACPM_CH_TX_REAR) = 0x1000 + q; S(c + B2L_ACPM_CH_TX_FRONT) = 0x1004 + q; S(c + B2L_ACPM_CH_TX_BASE) = TXQ + q * 2;
 S(c + B2L_ACPM_CH_RX_REAR) = 0x1008 + q; S(c + B2L_ACPM_CH_RX_FRONT) = 0x100c + q; S(c + B2L_ACPM_CH_RX_BASE) = RXQ + q * 2;
 S(c + B2L_ACPM_CH_LEN) = LEN; S(c + B2L_ACPM_CH_ELEM) = ELEM;
}
int main(void) {
 struct b2l_acpm_channel d, t, bad;
 S(B2L_ACPM_INITDATA + B2L_ACPM_FW_CHANNELS) = TABLE;
 S(B2L_ACPM_INITDATA + B2L_ACPM_FW_AP_MAX) = 12;
 build(B2L_ACPM_CHANNEL_DVFS); build(B2L_ACPM_CHANNEL_TMU);
 assert(b2l_acpm_open(&io, B2L_ACPM_CHANNEL_DVFS, &d) && b2l_acpm_open(&io, B2L_ACPM_CHANNEL_TMU, &t));
 assert(!b2l_acpm_open(&io, 12, &bad));
 /* Garbage instead of a table is refused rather than followed. */
 S(chan(1) + B2L_ACPM_CH_LEN) = 8; S(chan(1) + B2L_ACPM_CH_ID) = 7;
 assert(!b2l_acpm_open(&io, 1, &bad));
 S(chan(1) + B2L_ACPM_CH_ID) = 1; S(chan(1) + B2L_ACPM_CH_ELEM) = 15;
 assert(!b2l_acpm_open(&io, 1, &bad));
 uint32_t saved = S(B2L_ACPM_INITDATA + B2L_ACPM_FW_CHANNELS);
 S(B2L_ACPM_INITDATA + B2L_ACPM_FW_CHANNELS) = 0xfffffff0;
 assert(!b2l_acpm_open(&io, 5, &bad));
 S(B2L_ACPM_INITDATA + B2L_ACPM_FW_CHANNELS) = saved;

 rate[B2L_DVFS_G3D] = 156000;
 assert(b2l_acpm_tmu_init(&io, &t));
 assert(b2l_acpm_big_step(0, 75, 76, 4, 4) == 0 && b2l_acpm_big_step(0, 76, 76, 4, 4) == 1);
 assert(b2l_acpm_big_step(0, 80, 76, 4, 4) == 2 && b2l_acpm_big_step(0, 99, 76, 4, 4) == 3);
 assert(b2l_acpm_big_step(2, 78, 76, 4, 4) == 2 && b2l_acpm_big_step(2, 77, 76, 4, 4) == 1);
 assert(b2l_acpm_big_step(3, 60, 76, 4, 4) == 0 && b2l_acpm_big_step(1, 73, 76, 4, 4) == 0);
 struct b2l_acpm_load load = { 0, 0 };
 b2l_acpm_load_step(&load, 14, 15, 8, 3); assert(!load.boosted);
 b2l_acpm_load_step(&load, 15, 15, 8, 3); assert(load.boosted);
 for (int n = 0; n < 2; n++) { b2l_acpm_load_step(&load, 7, 15, 8, 3); assert(load.boosted); }
 b2l_acpm_load_step(&load, 8, 15, 8, 3); assert(load.boosted && load.quiet == 0);
 for (int n = 0; n < 2; n++) { b2l_acpm_load_step(&load, 0, 15, 8, 3); assert(load.boosted); }
 b2l_acpm_load_step(&load, 0, 15, 8, 3); assert(!load.boosted);
 assert(!b2l_acpm_thermal_hold(0, 79, 80, 70) && b2l_acpm_thermal_hold(0, 80, 80, 70));
 assert(b2l_acpm_thermal_hold(1, 71, 80, 70) && !b2l_acpm_thermal_hold(1, 70, 80, 70));
 assert(rate_of(&d, B2L_DVFS_G3D) == 156000);
 assert(b2l_acpm_set_rate(&io, &d, B2L_DVFS_G3D, 433000) && rate[B2L_DVFS_G3D] == 433000);
 assert(rate_of(&d, B2L_DVFS_G3D) == 433000 && pending_cleared == 4);
 /* Sequence numbers wrap through 63 without ever being 0, queues wrap too. */
 for (int i = 0; i < 300; i++) {
  assert(b2l_acpm_set_rate(&io, &d, B2L_DVFS_G3D, 100000u + (uint32_t)i) && rate[B2L_DVFS_G3D] == 100000u + (uint32_t)i);
  assert(d.seq >= 1 && d.seq <= 63);
 }
 /* A reply for someone else ahead of ours stays queued, ours is found. */
 early_reply = 1;
 assert(rate_of(&d, B2L_DVFS_G3D) == 100299);
 assert(S(S(chan(B2L_ACPM_CHANNEL_DVFS) + B2L_ACPM_CH_RX_REAR)) != S(S(chan(B2L_ACPM_CHANNEL_DVFS) + B2L_ACPM_CH_RX_FRONT)));
 S(S(chan(B2L_ACPM_CHANNEL_DVFS) + B2L_ACPM_CH_RX_REAR)) = S(S(chan(B2L_ACPM_CHANNEL_DVFS) + B2L_ACPM_CH_RX_FRONT));
 assert(b2l_acpm_read_temp(&io, &t, B2L_TMU_G3D) == 43);
 bad_ret = 1;
 assert(b2l_acpm_read_temp(&io, &t, B2L_TMU_G3D) == -1);
 bad_ret = 0;
 /* No reply: an error, not a hang, and nothing changes. */
 silent = 1;
 uint32_t before = rate[B2L_DVFS_G3D];
 assert(!b2l_acpm_set_rate(&io, &d, B2L_DVFS_G3D, 702000));
 assert(rate[B2L_DVFS_G3D] == before && rate_of(&d, B2L_DVFS_G3D) == 0);
 silent = 0;
 /* A full queue refuses without ringing. */
 uint32_t r = S(S(chan(B2L_ACPM_CHANNEL_DVFS) + B2L_ACPM_CH_TX_REAR));
 S(S(chan(B2L_ACPM_CHANNEL_DVFS) + B2L_ACPM_CH_TX_FRONT)) = (r + LEN - 1) % LEN;
 uint32_t rings = rung;
 assert(!b2l_acpm_set_rate(&io, &d, B2L_DVFS_G3D, 1) && rung == rings);
 puts("PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'acpm_test.c'
    src.write_text(TEST)
    exe = Path(tmp) / 'acpm_test'
    subprocess.run(['clang', '-std=gnu11', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable', '-fsanitize=address,undefined',
                    '-I', str(ROOT / 'xnu-overlay/iokit/Kernel/arm'), str(src), '-o', str(exe)], check=True)
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert run.returncode == 0 and run.stdout.strip() == 'PASS', run.stdout + run.stderr[-1500:]
print('acpm tests passed')
