#!/usr/bin/env python3
"""Check real cache code against zlib, LLVM and the reference ARM64e decoder."""
import argparse
import ctypes
import os
from pathlib import Path
import random
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import zlib


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--require-llvm', action='store_true')
    a = p.parse_args()
    repo = Path(__file__).resolve().parents[1]
    cache = repo / 'common/cache'
    arm = repo / 'xnu-overlay/osfmk/arm64'
    cc = shlex.split(os.environ.get('CC', 'cc'))
    flags = ['-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
             '-Wno-misleading-indentation', '-I' + str(cache), '-I' + str(arm)]
    sources = [str(cache / 'b2l_cache_core.c'), str(cache / 'CacheSha.c')]
    with tempfile.TemporaryDirectory(prefix='b2l-cache-fastpaths-') as temp:
        root = Path(temp)
        lib = root / 'cache.so'
        mode = '-dynamiclib' if sys.platform == 'darwin' else '-shared'
        subprocess.run(cc + flags + [mode, '-fPIC'] + sources + ['-o', str(lib)], check=True)
        m = ctypes.CDLL(str(lib))
        m.b2l_nc_crc.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        m.b2l_nc_crc.restype = ctypes.c_uint32
        m.b2l_nc_native.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
        rng = random.Random(918273)
        data = bytes(rng.randrange(256) for unused in range(65560))
        buf = ctypes.create_string_buffer(data)
        for size in [0, 1, 3, 4, 60, 128, 4096, 12288, 65536] + [rng.randrange(65537) for unused in range(300)]:
            offset = rng.randrange(24)
            assert m.b2l_nc_crc(ctypes.byref(buf, offset), size) == zlib.crc32(data[offset:offset + size])
        print('Actual cache CRC matches independent zlib for boundary, unaligned and random inputs', flush=True)

        llvm = os.environ.get('LLVM_MC') or shutil.which('llvm-mc')
        if a.require_llvm and not llvm:
            raise SystemExit('LLVM_MC is required for the independent encoding audit')
        if llvm:
            pairs = []
            for key in ('ldraa', 'ldrab'):
                for wb in (False, True):
                    for offset in (-4096, -264, -256, -8, 0, 8, 248, 256, 4088):
                        for rn in (0, 1, 29, 30):
                            for rd in (0, 1, 30, 31):
                                supported = (not wb or rn != rd) and ((not wb and offset >= 0) or -256 <= offset <= 255)
                                if not supported:
                                    continue
                                dest = 'xzr' if rd == 31 else f'x{rd}'
                                original = f'{key} {dest},[x{rn},#{offset}]' + ('!' if wb else '')
                                target = ('ldr' if wb or offset >= 0 else 'ldur') + f' {dest},[x{rn},#{offset}]' + ('!' if wb else '')
                                pairs.append((original, target))
            result = subprocess.run([llvm, '-triple=aarch64', '-mattr=+pauth', '-show-encoding'],
                                    input='\n'.join(word for pair in pairs for word in pair),
                                    capture_output=True, text=True, check=True)
            words = [int.from_bytes(bytes(int(value.strip(), 16) for value in match.split(',')), 'little')
                     for match in re.findall(r'encoding: \[([^]]+)\]', result.stdout)]
            assert len(words) == len(pairs) * 2
            for original, expected in zip(words[::2], words[1::2]):
                out = ctypes.c_uint32()
                assert m.b2l_nc_native(original, ctypes.byref(out)) == 1 and out.value == expected
            print(f'Actual LDRA conversion agrees with LLVM on {len(pairs)} assembled instruction pairs', flush=True)
            # PACGA mask ABI: every register alias assembles to the same AND immediate
            # (or NOP for a discarded XZR result). Off by default, on by explicit opt-in.
            pacga_pairs = []
            for rd in (0, 8, 29, 30, 31):
                for rn in (0, 8, 21, 30, 31):
                    for rm in (0, 8, 10, 30, 31):
                        source_n = 'xzr' if rn == 31 else f'x{rn}'
                        modifier = 'sp' if rm == 31 else f'x{rm}'
                        if rd == 31:
                            pacga_pairs.append((f'pacga xzr,{source_n},{modifier}', 'nop'))
                        else:
                            pacga_pairs.append((f'pacga x{rd},{source_n},{modifier}',
                                                f'and x{rd},{source_n},#0xffffffff00000000'))
            result = subprocess.run([llvm, '-triple=aarch64', '-mattr=+pauth', '-show-encoding'],
                                    input='\n'.join(word for pair in pacga_pairs for word in pair),
                                    capture_output=True, text=True, check=True)
            words = [int.from_bytes(bytes(int(value.strip(), 16) for value in match.split(',')), 'little')
                     for match in re.findall(r'encoding: \[([^]]+)\]', result.stdout)]
            assert len(words) == len(pacga_pairs) * 2
            gate = ctypes.c_int.in_dll(m, 'b2l_nc_pacga_mask')
            for original, expected in zip(words[::2], words[1::2]):
                out = ctypes.c_uint32()
                gate.value = 0
                assert m.b2l_nc_native(original, ctypes.byref(out)) == 0
                gate.value = 1
                assert m.b2l_nc_native(original, ctypes.byref(out)) == 1 and out.value == expected
            gate.value = 0
            print(f'Actual PACGA mask conversion agrees with LLVM on {len(pacga_pairs)} assembled instruction pairs', flush=True)
        else:
            print('LLVM encoding audit skipped locally; required in firmware CI', flush=True)

        for name, extra in [('native_cache_ldra_test', [str(arm / 'b2l_compat.c')]),
                            ('native_cache_pacga_test', [str(arm / 'b2l_compat.c')]),
                            ('native_cache_activity_test', ['-pthread'])]:
            exe = root / name
            subprocess.run(cc + flags + ['-fsanitize=address,undefined', str(repo / 'tests' / (name + '.c'))] +
                           sources + extra + ['-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
