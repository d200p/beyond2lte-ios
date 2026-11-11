#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Compile the actual cache gating bodies: a precompiled root disables learning and apply.

The extracted functions are the shipped `precompiled_root`/`enabled` helpers and
the first statement of `b2l_native_cache_before_init`, driven with mock boot
arguments. This checks the gate, not kernel scheduling or storage.
"""
import pathlib
import subprocess
import tempfile

repo = pathlib.Path(__file__).resolve().parents[1]
source = (repo / 'xnu-overlay/bsd/kern/b2l_native_cache.c').read_text()


def section(start, end):
    a = source.index(start)
    return source[a:source.index(end, a)]


gates = section('static int precompiled_root(', '\nvoid b2l_native_cache_storage(')
prologue = section('void b2l_native_cache_before_init(void){', '\n DTEntry dt;')
assert 'precompiled_root()' in prologue and 'storage_ready' in prologue, prologue
loader = (repo / 'uefi/XnuLoader/XnuLoader.c').read_text()
assert ' b2l_precompiled=1 ' in loader and ' b2l_arm64e_compat=1 ' in loader, 'loader must opt into both'

program = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
static int compat_on, precompiled_on, cache_flag = -1, storage_ready = 1;
static unsigned printed;
bool b2l_arm64e_compat_enabled(void){return compat_on;}
bool b2l_arm64e_precompiled(void){return precompiled_on;}
static bool PE_parse_boot_argn(const char *n,void *p,size_t z){
 assert(z==sizeof(int)&&!strcmp(n,"b2l_native_cache"));if(cache_flag<0)return false;memcpy(p,&cache_flag,z);return true;}
#define printf(...) (++printed)
''' + gates + prologue + ''' assert(!"the normal path must not run in precompiled mode or with a disabled cache"); }
#undef printf
int main(void){
 for(int c=0;c<2;c++)for(int p=0;p<2;p++)for(int f=-1;f<=2;f++){
  compat_on=c;precompiled_on=p;cache_flag=f;
  assert(precompiled_root()==(c&&p));
  assert(enabled()==(c&&!p&&f==1));
 }
 compat_on=1;precompiled_on=1;cache_flag=1;printed=0;b2l_native_cache_before_init();assert(printed==1);
 compat_on=0;precompiled_on=1;cache_flag=0;printed=0;b2l_native_cache_before_init();assert(printed==1);
 compat_on=1;precompiled_on=0;cache_flag=0;printed=0;b2l_native_cache_before_init();assert(printed==1);
 compat_on=1;precompiled_on=0;cache_flag=1;storage_ready=0;printed=0;b2l_native_cache_before_init();assert(printed==1);
 puts("Native cache gate: precompiled root skips apply/learning; disabled cache retains emulation PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='b2l-cache-gate-') as td:
    d = pathlib.Path(td)
    (d / 'gate.c').write_text(program)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-fsanitize=address,undefined', str(d / 'gate.c'), '-o', str(d / 'gate')], check=True)
    subprocess.run([str(d / 'gate')], check=True)
