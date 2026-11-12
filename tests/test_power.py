#!/usr/bin/env python3
"""Run the power-off and restart sequences against a simulated PMU.

The simulator holds the rules the hardware and bootloader impose: the reason
must be in place before power drops or the reset fires, a held power key turns
the phone back on, and neighbouring register bits survive.
"""
from pathlib import Path
import subprocess, tempfile
ROOT = Path(__file__).resolve().parents[1]
TEST = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "b2l_power.h"
static uint32_t regs[B2L_PMU_BYTES / 4];
static int key_polls, resets, dropped;
static uint32_t waited, reason_at_drop, magic_at_drop, reason_at_reset, magic_at_reset;
static int key_down_polls;
static uint32_t rd(void *c, uint32_t o) { (void)c; assert(o < B2L_PMU_BYTES && !(o & 3)); return regs[o / 4]; }
static void wr(void *c, uint32_t o, uint32_t v) {
 (void)c; assert(o < B2L_PMU_BYTES && !(o & 3)); regs[o / 4] = v;
 if (o == B2L_PMU_SWRESET) {
  assert(v == B2L_PMU_SWRESET_TRIGGER); assert(regs[B2L_PMU_RST_STAT / 4] == 0);
  resets++; reason_at_reset = regs[B2L_PMU_INFORM3 / 4]; magic_at_reset = regs[B2L_PMU_INFORM2 / 4];
 }
 if (o == B2L_PMU_PS_HOLD && !(v & B2L_PMU_PS_HOLD_OUT)) {
  assert(key_polls >= key_down_polls && "power dropped with the key held");
  dropped++; reason_at_drop = regs[B2L_PMU_INFORM3 / 4]; magic_at_drop = regs[B2L_PMU_INFORM2 / 4];
 }
}
static int key(void *c) { (void)c; return key_polls++ < key_down_polls; }
static void delay(void *c, uint32_t ms) { (void)c; waited += ms; }
static const struct b2l_power_io io = { NULL, rd, wr, key, delay };
static void reset_all(int down) {
 memset(regs, 0, sizeof regs); key_polls = resets = dropped = 0; waited = 0; key_down_polls = down;
 regs[B2L_PMU_INFORM2 / 4] = 0xaa11bb22u;
 regs[B2L_PMU_PS_HOLD / 4] = 0x5300u;
 regs[B2L_PMU_RST_STAT / 4] = 0x4;
}
int main(void) {
 reset_all(0);
 b2l_power_restart(&io, 0);
 assert(resets == 1 && reason_at_reset == 0x12345670u && !dropped);
 assert(magic_at_reset == 0xaa4ebb22u);         /* only the magic byte moves */
 reset_all(0);
 b2l_power_restart(&io, 1);
 assert(resets == 1 && reason_at_reset == 0x12345674u && magic_at_reset == 0xaa4ebb22u);
 reset_all(0);
 b2l_power_off(&io);
 assert(dropped == 1 && !resets && reason_at_drop == 0x12345670u && magic_at_drop == 0xaa70bb22u);
 assert(regs[B2L_PMU_PS_HOLD / 4] == 0x5200u);  /* only bit 8 cleared */
 assert(waited == B2L_POWER_SETTLE_MS);         /* returns so the caller can restart */
 reset_all(5);                                  /* key released after five polls */
 b2l_power_off(&io);
 assert(dropped == 1 && waited == 500 + B2L_POWER_SETTLE_MS);
 puts("PASS");
 return 0;
}
'''
STUCK = r'''
#include <assert.h>
#include <stdio.h>
#include "b2l_power.h"
static uint32_t held = 0xffffffffu, waited, drops;
static uint32_t rd(void *c, uint32_t o) { (void)c; return o == B2L_PMU_PS_HOLD ? held : 0; }
static void wr(void *c, uint32_t o, uint32_t v) { (void)c; if (o == B2L_PMU_PS_HOLD) { held = v; drops++; } }
static int key(void *c) { (void)c; return 1; }
static void delay(void *c, uint32_t ms) { (void)c; waited += ms; }
int main(void) {
 const struct b2l_power_io io = { 0, rd, wr, key, delay };
 b2l_power_off(&io);
 assert(drops == 1 && waited == B2L_POWER_RELEASE_MS + B2L_POWER_SETTLE_MS);
 puts("PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    for name, code in (('power', TEST), ('stuck', STUCK)):
        src = Path(tmp) / (name + '.c')
        src.write_text(code)
        exe = Path(tmp) / name
        subprocess.run(['clang', '-std=gnu11', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-I', str(ROOT / 'xnu-overlay/iokit/Kernel/arm'), str(src), '-o', str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        assert run.returncode == 0 and run.stdout.strip() == 'PASS', run.stdout + run.stderr[-1200:]
source = (ROOT / 'xnu-overlay/iokit/Kernel/arm/B2LPower.cpp').read_text()
assert 'type != kPEHaltCPU && type != kPERestartCPU' in source, 'panics must stay out of this path'
print('power tests passed')
