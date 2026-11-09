#!/usr/bin/env python3
"""Execute the patched native platform-error fallback with a panic boundary stub."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

source = (Path(sys.argv[1]) / 'osfmk/arm64/sleh.c').read_text()
start = source.index('arm64_implementation_specific_error(')
body = source[start:source.index('\n#if CONFIG_KERNEL_INTEGRITY', start)]
assert 'arm64_implementation_specific_error(state, esr, far, source);' in source
pre = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
typedef int boolean_t;
typedef uint64_t vm_offset_t;
typedef enum { PLAT_ERR_SRC_ASYNC, PLAT_ERR_SRC_SYNC } platform_error_source_t;
typedef struct { unsigned flavor; uint64_t pc, lr; } arm_saved_state_t;
static jmp_buf jump;
static unsigned captured, panics;
static uint64_t fault_pc, fault_lr, fault_esr, fault_far;
static char fault_message[100];
static int is_saved_state64(arm_saved_state_t *s) { assert(s); return s->flavor == 64; }
static uint64_t get_saved_state_pc(arm_saved_state_t *s) { assert(s->flavor == 64); return s->pc; }
static uint64_t get_saved_state_lr(arm_saved_state_t *s) { assert(s->flavor == 64); return s->lr; }
void b2l_early_fault(const char *msg, uint64_t pc, uint64_t lr, uint64_t esr, uint64_t far) {
    assert(!panics); captured++; fault_pc=pc; fault_lr=lr; fault_esr=esr; fault_far=far;
    snprintf(fault_message, sizeof(fault_message), "%s", msg);
}
static _Noreturn void panic_plain(const char *message) {
    assert(!strcmp(message, "Unhandled implementation specific error\n"));
#if defined(B2L)
    assert(captured == 1);
#else
    assert(captured == 0);
#endif
    panics++; longjmp(jump, 1);
}
static _Noreturn void
'''
post = r'''
int main(void) {
    arm_saved_state_t state = {64, 0xfffffff007123450ull, 0xfffffff007abcdefull};
    for (volatile unsigned flavor = 0; flavor < 3; flavor++) {
        for (volatile unsigned src = 0; src < 2; src++) {
            state.flavor = flavor == 2 ? 64 : 32;
            captured = panics = 0;
            if (!setjmp(jump)) arm64_implementation_specific_error(flavor ? &state : NULL,
                0xbf000002, 0x2039000, (platform_error_source_t)src);
            assert(panics == 1);
#if defined(B2L)
            assert(captured == 1 && fault_esr == 0xbf000002 && fault_far == 0x2039000);
            assert(fault_pc == (flavor == 2 ? state.pc : 0));
            assert(fault_lr == (flavor == 2 ? state.lr : 0));
            assert(strstr(fault_message, src ? "synchronous" : "asynchronous"));
            if (!src) assert(strstr(fault_message, "imprecise"));
#endif
        }
    }
    puts("Platform error: typed fields precede panic, absent/32-bit states are bounded, asynchronous PC is marked, native fallback retained");
}
'''
with tempfile.TemporaryDirectory(prefix='b2l-platform-error-') as temporary:
    root = Path(temporary)
    c = root / 'test.c'
    c.write_text(pre + body + post)
    for board in ([], ['-DB2L=1']):
        exe = root / ('b2l' if board else 'native')
        subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                       ['-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-unknown-pragmas',
                        '-Wno-unused-parameter',
                        '-fsanitize=address,undefined'] + board +
                       [str(c), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
