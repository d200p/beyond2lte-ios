#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise the shared IOSurface purgeable-state helper and its provider wiring."""
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
ARM = ROOT / 'xnu-overlay/iokit/Kernel/arm'
source = (ARM / 'B2LSurface.cpp').read_text()
assert 'kB2LSurfaceSetPurgeable       = 20' in source, 'selector 20 must be dispatched'
case = source[source.index('case kB2LSurfaceSetPurgeable:'):source.index('case kB2LSurfaceCreatePort:')]
assert 'scalarInputCount != 2' in case and 'scalarOutputCount < 1' in case and 's->release()' in case
assert 'purgeState' in (ARM / 'B2LSurface.h').read_text()

PROGRAM = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "b2l_surface_purgeable.h"
static uint32_t shared;
static void *worker(void *arg) {
    uint64_t old; for (unsigned i = 0; i < 100000; i++) {
        assert(!b2l_surface_purgeable(&shared, (i + (unsigned)(uintptr_t)arg) & 1, &old)); assert(old <= 1); }
    return 0;
}
int main(void) {
    uint32_t state = 0; uint64_t old = 99;
    /* Fresh surface is non-volatile; CA's volatile/non-volatile cycle never reports Empty. */
    assert(!b2l_surface_purgeable(&state, kB2LSurfacePurgeVolatile, &old) && old == kB2LSurfacePurgeNonVolatile);
    assert(!b2l_surface_purgeable(&state, kB2LSurfacePurgeKeepCurrent, &old) && old == kB2LSurfacePurgeVolatile && state == 1);
    assert(!b2l_surface_purgeable(&state, kB2LSurfacePurgeNonVolatile, &old) && old == kB2LSurfacePurgeVolatile && state == 0);
    /* An explicit Empty request is honoured and reported back. */
    assert(!b2l_surface_purgeable(&state, kB2LSurfacePurgeEmpty, &old) && old == 0);
    assert(!b2l_surface_purgeable(&state, kB2LSurfacePurgeNonVolatile, &old) && old == kB2LSurfacePurgeEmpty);
    /* Unknown requests and null pointers are rejected without changing state. */
    assert(b2l_surface_purgeable(&state, 4, &old) == -1 && state == 0);
    assert(b2l_surface_purgeable(0, 0, &old) == -1 && b2l_surface_purgeable(&state, 0, 0) == -1);
    pthread_t t[8]; for (uintptr_t i = 0; i < 8; i++) pthread_create(&t[i], 0, worker, (void *)i);
    for (int i = 0; i < 8; i++) pthread_join(t[i], 0);
    assert(shared <= 1);
    puts("Surface purgeable state: CA volatile cycle, KeepCurrent, explicit Empty, rejection and 8-thread updates PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as td:
    d = pathlib.Path(td)
    (d / 't.c').write_text(PROGRAM)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=thread', '-I' + str(ARM),
                    str(d / 't.c'), '-o', str(d / 't')], check=True)
    subprocess.run([str(d / 't')], check=True)
