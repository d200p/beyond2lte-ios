#!/usr/bin/env python3
"""Run the real exec hook that loads the GPU library into backboardd.

Only the exact executable gets the setting, only once the library is published,
a caller's own value wins, the boot argument turns it off, and running out of
string space leaves the exec untouched rather than failing it.
"""
from pathlib import Path
import subprocess, tempfile
ROOT = Path(__file__).resolve().parents[1]
TEST = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EXECUTABLE_KEY "executable_path="
#define UIO_SYSSPACE 1
#define TRUE 1
#undef CAST_USER_ADDR_T
#define CAST_USER_ADDR_T(a) ((const char *)(a))
struct image_params {
 char *ip_strings, *ip_endargv, *ip_strendp;
 int ip_argspace, ip_strspace, ip_envc;
};
static int boot_arg = -1, ready, add_error, added;
static int PE_parse_boot_argn(const char *name, void *value, int size) {
 (void)size; assert(!strcmp(name, "b2l_gpu_ui"));
 if (boot_arg < 0) return 0;
 *(int *)value = boot_arg; return 1;
}
static void b2l_early_trace(const char *format, ...) { (void)format; }
static int exec_add_user_string(struct image_params *i, const char *s, int seg, int nul) {
 (void)seg; (void)nul; if (add_error) return add_error;
 size_t n = strlen(s) + 1; memcpy(i->ip_strendp, s, n); i->ip_strendp += n;
 i->ip_argspace -= (int)n; i->ip_strspace -= (int)n; added++; return 0;
}
int b2l_gpu_ui_is_ready(void) { return ready; }
const char b2l_gpu_ui_setting[] = "DYLD_INSERT_LIBRARIES=/usr/local/lib/b2l-gpu-test.dylib";
#include "b2l_exec_gpu_ui.h"
static char area[4096];
static struct image_params setup(const char *path, const char *env) {
 struct image_params i; memset(area, 0, sizeof area); added = 0;
 int n = snprintf(area, sizeof area, "%s%s", EXECUTABLE_KEY, path);
 i.ip_strings = area; i.ip_endargv = area + n + 1; i.ip_strendp = i.ip_endargv;
 if (env) { strcpy(i.ip_endargv, env); i.ip_strendp += strlen(env) + 1; }
 i.ip_argspace = 2048; i.ip_strspace = 2048; i.ip_envc = env ? 1 : 0;
 return i;
}
int main(void) {
 const char *bbd = "/usr/libexec/backboardd";
 struct image_params i;
 ready = 1;
 i = setup(bbd, "HOME=/"); assert(!b2l_exec_gpu_ui(&i, 8) && added == 1 && i.ip_envc == 2);
 assert(i.ip_argspace == 2048 - (int)sizeof(b2l_gpu_ui_setting) - 8);
 assert(!strcmp(i.ip_strendp - sizeof(b2l_gpu_ui_setting), b2l_gpu_ui_setting));
 /* Other executables, including near matches. */
 const char *others[] = { "/usr/libexec/backboardd2", "/usr/libexec/backboard", "/System/backboardd",
  "/System/Library/CoreServices/SpringBoard.app/SpringBoard" };
 for (unsigned k = 0; k < 4; k++) { i = setup(others[k], NULL); assert(!b2l_exec_gpu_ui(&i, 8) && !added); }
 /* A caller's value stands. */
 i = setup(bbd, "DYLD_INSERT_LIBRARIES=/x"); assert(!b2l_exec_gpu_ui(&i, 8) && !added && i.ip_envc == 1);
 /* Not published, or switched off. */
 ready = 0; i = setup(bbd, NULL); assert(!b2l_exec_gpu_ui(&i, 8) && !added); ready = 1;
 boot_arg = 0; i = setup(bbd, NULL); assert(!b2l_exec_gpu_ui(&i, 8) && !added); boot_arg = 1;
 i = setup(bbd, NULL); assert(!b2l_exec_gpu_ui(&i, 8) && added == 1); boot_arg = -1;
 /* 32-bit exec, no space, and copy failure. */
 i = setup(bbd, NULL); assert(!b2l_exec_gpu_ui(&i, 4) && !added);
 i = setup(bbd, NULL); i.ip_argspace = 10; assert(!b2l_exec_gpu_ui(&i, 8) && !added && i.ip_argspace == 10);
 i = setup(bbd, NULL); i.ip_strspace = 10; assert(!b2l_exec_gpu_ui(&i, 8) && !added);
 add_error = 7; i = setup(bbd, NULL); assert(b2l_exec_gpu_ui(&i, 8) == 7 && i.ip_envc == 0); add_error = 0;
 /* An unterminated environment is left alone. */
 i = setup(bbd, NULL); memset(i.ip_endargv, 'x', 16); i.ip_strendp = i.ip_endargv + 16;
 assert(!b2l_exec_gpu_ui(&i, 8) && !added);
 puts("PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'exec_test.c'
    src.write_text(TEST)
    exe = Path(tmp) / 'exec_test'
    subprocess.run(['clang', '-std=gnu11', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(ROOT / 'xnu-overlay/bsd/kern'), str(src), '-o', str(exe)], check=True)
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert run.returncode == 0 and run.stdout.strip() == 'PASS', run.stdout + run.stderr[-1200:]
print('gpu ui exec tests passed')
