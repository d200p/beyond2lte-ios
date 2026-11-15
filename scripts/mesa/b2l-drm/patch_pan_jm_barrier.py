#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""S10 port: tiler jobs with the job barrier on this GPU; B2L_NO_TILER_BARRIER=1
takes it off.

Usage: patch_pan_jm_barrier.py MESA/src/gallium/drivers/panfrost/pan_jm.c

Boot 49 (Core Animation on Panfrost, IDVS off): the UI was drawn, but some
quads lost a triangle or had one stretched to a wrong point, differently from
frame to frame, while every job completed without a fault. That is the IDVS
failure of boots 41-43 again (the tiler using positions that were not yet in
memory), now intermittent: the tiler job depends on its vertex job, but on the
Mali-G76 r0p0 the vertex shader's position stores are not always visible to it
by then. Panfrost already sets the job barrier on jobs whose memory writes
later jobs consume (compute jobs, vertex jobs with side effects); with it the
job manager waits for every earlier job in the chain to complete before the
tiler job starts. Read per draw. Idempotent.
"""
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text()
if 'B2L_NO_TILER_BARRIER' in text:
    sys.exit(0)
old = ('   pan_jc_add_job(&batch->jm.jobs.vtc_jc, MALI_JOB_TYPE_TILER, false, false,\n'
       '                  vertex, 0, tiler_job, false);')
new = ('   pan_jc_add_job(&batch->jm.jobs.vtc_jc, MALI_JOB_TYPE_TILER,\n'
       '                  getenv("B2L_NO_TILER_BARRIER") == NULL, /* S10 port */ false,\n'
       '                  vertex, 0, tiler_job, false);')
if text.count(old) != 1:
    sys.exit('pan_jm.c: vertex/tiler job pair not found')
text = text.replace(old, new)
first = text.index('#include')
text = text[:first] + '#include <stdlib.h> /* S10 port: getenv */\n' + text[first:]
path.write_text(text)
print('pan_jm.c: tiler jobs with the job barrier unless B2L_NO_TILER_BARRIER is set')
