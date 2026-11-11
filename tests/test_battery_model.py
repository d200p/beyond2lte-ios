#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise the actual b2l_battery_model.h: MAX77705 units, charger state and HSI2C timing."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER_DIR = ROOT / 'xnu-overlay/iokit/Kernel/arm'


def uefi_fast_timing(clock):
    """Independent port of HsI2cDxe SetTiming for FAST_SPD."""
    div = (clock // (400000 * 15)) & 0xFF
    base = ((9 * clock) // 1000000) // ((div + 1) * 10)
    mask2 = lambda x: ((0xFFFFFFFF >> x) << x) & 0xFF
    return div, mask2(base), mask2(base - 1)


cases = '\n'.join(
    f' {{uint32_t d,t,t1;b2l_hsi2c_fast_timing({c}u,&d,&t,&t1);assert(d=={e[0]}u&&t=={e[1]}u&&t1=={e[2]}u);}}'
    for c, e in ((c, uefi_fast_timing(c)) for c in (200000000, 100000000, 400000000, 26000000)))

PROGRAM = r'''
#include <assert.h>
#include <stdio.h>
#include "b2l_battery_model.h"
static struct b2l_battery_state run(struct b2l_battery_raw r){struct b2l_battery_state s;b2l_battery_decode(&r,&s);return s;}
int main(void){
''' + cases + r'''
 { uint32_t d,t,t1; b2l_hsi2c_fast_timing(200000000u,&d,&t,&t1); assert(d==33&&t==0xE0&&t1==0xF0); }

 /* Units: 3.85 V, 51% (0x32 + 0x80/256 rounds up), 312 mA, 4000 mAh. */
 struct b2l_battery_raw r={0};
 r.vcell=49280;r.repsoc=0x3280;r.current=1000;r.avgcurrent=-1000;r.repcap=2000;r.fullcaprep=4000;r.designcap=4100;
 r.cycles=12345;r.temp=6528;r.tte=640;r.ttf=0xFFFF;
 struct b2l_battery_state s=run(r);
 assert(s.valid&&s.millivolts==3850&&s.percent==51&&s.milliamps==312&&s.avg_milliamps==-312);
 assert(s.remaining_mah==2000&&s.full_mah==4000&&s.design_mah==4100&&s.cycles==123);
 assert(s.temp_valid&&s.temp_centi_c==2550);
 /* No charger reading, discharging: unplugged, time to empty from TTE. */
 assert(!s.external&&!s.charging&&!s.full&&s.minutes_to_empty==60&&s.minutes_to_full==-1);
 /* No charger reading, charging current: plugged and charging. */
 r.avgcurrent=200;r.ttf=1280;s=run(r);assert(s.external&&s.charging&&s.minutes_to_full==120&&s.minutes_to_empty==-1);

 /* Charger: CHGIN_OK with fast CC; WCIN; DETAILS_00 only; done; off. */
 r.avgcurrent=-100;r.charger_ok=true;r.int_ok=0x40;r.details01=0x01;s=run(r);assert(s.external&&s.charging&&!s.full);
 r.int_ok=0x20;s=run(r);assert(s.external&&s.charging);
 r.int_ok=0;r.details00=0x60;s=run(r);assert(s.external&&s.charging);
 r.details00=0x18;s=run(r);assert(s.external);
 r.details00=0;r.int_ok=0x40;r.details01=0x04;s=run(r);assert(s.external&&s.full&&!s.charging&&s.minutes_to_full==-1&&s.minutes_to_empty==-1);
 r.details01=0x08;s=run(r);assert(s.external&&!s.charging&&!s.full);
 r.details01=0x02;r.repsoc=100u<<8;s=run(r);assert(s.percent==100&&s.full&&!s.charging);
 r.int_ok=0;r.details01=0x01;r.repsoc=0x3200;s=run(r);assert(!s.external&&!s.charging&&!s.full&&s.minutes_to_empty==60);

 /* Clamps and plausibility. */
 r.repsoc=0x64FF;s=run(r);assert(s.percent==100&&s.valid);
 r.repsoc=0x6600;s=run(r);assert(!s.valid);
 r.repsoc=0x3200;r.vcell=0;s=run(r);assert(!s.valid);
 r.vcell=0xFFFF;s=run(r);assert(!s.valid);
 r.vcell=49280;r.temp=(int16_t)0x8000;s=run(r);assert(!s.temp_valid);
 r.temp=-2560;s=run(r);assert(s.temp_valid&&s.temp_centi_c==-1000);
 r.current=(int16_t)0x8000;s=run(r);assert(s.milliamps==-10240);
 assert(b2l_fg_minutes(0)==-1&&b2l_fg_minutes(0xFFFF)==-1&&b2l_fg_minutes(0xFFFE)==6143);
 puts("Battery model: UEFI fast-mode timing, MAX77705 units/rounding, charger states, fallback and plausibility PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as td:
    d = pathlib.Path(td)
    (d / 't.c').write_text(PROGRAM)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + str(HEADER_DIR), str(d / 't.c'), '-o', str(d / 't')], check=True)
    subprocess.run([str(d / 't')], check=True)
