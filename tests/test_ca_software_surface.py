#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Patch, verify and undo scripts/patch-ca-software-surface.py on a synthetic cache.

The synthetic cache has the real layout (header, mapping table, image table,
one image whose executable segment covers part of a text mapping, and an
all-zero TEXT_STUBS mapping) with filler bytes; the script's byte hashes are
overridden to match the filler. The cave must be the all-zero page outside the
image, never a zero page inside it or in the stub mapping.
"""
import contextlib
import hashlib
import importlib.util
import io
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('patch', ROOT / 'scripts/patch-ca-software-surface.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

TEXT, TEXT_SIZE = 0x1840D0000, 0x40000
STUBS, STUBS_SIZE = 0x186BC8000, 0x8000
LLVM_MC = shutil.which('llvm-mc') or '/opt/homebrew/opt/llvm/bin/llvm-mc'


IMAGE_SIZE = 0x30000                 # executable segment of the one image
INSIDE, OUTSIDE = TEXT + 0x20000, TEXT + 0x38000


def build(root, outside_zero=True):
    path = root / m.CACHE
    path.parent.mkdir(parents=True, exist_ok=True)
    header = bytearray(0x1000)
    header[:15] = b'dyld_v1  arm64e'
    header[0x58:0x68] = bytes.fromhex(m.UUID)
    struct.pack_into('<II', header, 0x188, 0x200, 0)        # no subcaches
    struct.pack_into('<II', header, 0x138, 0x300, 2)
    struct.pack_into('<QQQQQQII', header, 0x300, TEXT, TEXT_SIZE, 0x1000, 0, 0, 0, 5, 5)
    struct.pack_into('<QQQQQQII', header, 0x338, STUBS, STUBS_SIZE, 0x1000 + TEXT_SIZE, 0, 0, m.TEXT_STUBS, 5, 5)
    struct.pack_into('<II', header, 0x1C0, 0x400, 1)
    struct.pack_into('<Q', header, 0x400, TEXT)
    text = bytearray(os.urandom(TEXT_SIZE))
    struct.pack_into('<IiiIIII', text, 0, 0xFEEDFACF, 0x0100000C, 2, 6, 1, 72, 0)
    struct.pack_into('<II16sQQQQIIII', text, 32, 0x19, 72, b'__TEXT', TEXT, IMAGE_SIZE, 0, IMAGE_SIZE, 5, 5, 0, 0)
    text[INSIDE - TEXT:INSIDE - TEXT + m.PAGE] = bytes(m.PAGE)       # zero, but inside the image
    if outside_zero:
        text[OUTSIDE - TEXT:OUTSIDE - TEXT + m.PAGE] = bytes(m.PAGE)
    w = m.WRAPPER - TEXT
    struct.pack_into('<I', text, w, m.NOP)
    path.write_bytes(bytes(header) + bytes(text) + bytes(STUBS_SIZE))
    m.WRAPPER_SHA = hashlib.sha256(text[w:w + m.WRAPPER_LEN]).hexdigest()
    i = m.INSERT - TEXT
    m.INSERT_SHA = hashlib.sha256(text[i:i + m.INSERT_LEN]).hexdigest()
    return path


def run(*args):
    out = io.StringIO()
    sys.argv = ['patch'] + [str(a) for a in args]
    code = 0
    with contextlib.redirect_stdout(out):
        try:
            m.main()
        except SystemExit as e:
            code = e.code if isinstance(e.code, int) else 1
            out.write(str(e.code))
    return code, out.getvalue()


def disassemble(code, base):
    hexes = ', '.join('0x%02x' % b for b in code)
    r = subprocess.run([LLVM_MC, '-triple=aarch64', '-disassemble'], input=hexes,
                       capture_output=True, text=True, check=True)
    lines = [l.strip() for l in r.stdout.splitlines() if l.strip() and not l.startswith('.text')]
    out = []
    for k, l in enumerate(lines):
        mt = re.match(r'(\S+)\s+(?:(x\d+), )?#(-?\d+)$', l)
        if mt and mt.group(1) in ('b', 'bl', 'cbz', 'b.eq'):
            l = f'{mt.group(1)} {mt.group(2) + " " if mt.group(2) else ""}{base + 4 * k + int(mt.group(3)):#x}'
        out.append(re.sub(r'\s+', ' ', l.split('//')[0].strip()))
    return out


with tempfile.TemporaryDirectory() as td:
    root = pathlib.Path(td)
    path = build(root)
    original = path.read_bytes()

    assert run(root, '--verify')[0] != 0
    code, out = run(root, '--dry-run')
    assert code == 0 and 'dry run' in out and path.read_bytes() == original, out
    cave = int(re.search(r'b (0x[0-9a-f]+)', out).group(1), 16)
    assert cave == OUTSIDE, hex(cave)

    code, out = run(root)
    assert code == 0 and 'patched' in out, out
    cache = m.Cache(str(path))
    assert struct.unpack('<I', cache.read(m.WRAPPER, 4))[0] == m.branch(m.WRAPPER, cave)
    routine = cache.read(cave, m.ROUTINE_SIZE)
    assert routine == m.routine(cave) and len(routine) == m.ROUTINE_SIZE
    assert run(root, '--verify') == (0, f'verified: software wrapper {m.WRAPPER:#x} -> routine {cave:#x} -> insert {m.INSERT:#x}\n')
    code, out = run(root)
    assert code == 0 and 'already patched' in out

    d = disassemble(routine, cave)
    at = lambda i: cave + 4 * i
    assert d == [
        'stp x29, x30, [sp, #-32]!', 'mov x29, sp', 'str x0, [sp, #16]',
        f'bl {m.WRAPPER + 4:#x}', f'cbz x0 {at(21):#x}',
        'str x0, [sp, #24]', 'mov x1, x0', 'ldr x0, [sp, #16]', 'ldr w9, [x1, #92]',
        'mov x10, #88', 'tst w9, #0x400', 'mov x11, #96', 'csel x10, x10, x11, eq', 'ldr x9, [x0, x10]',
        f'cbz x9 {at(19):#x}', 'cmp x9, x1', f'b.eq {at(20):#x}', 'ldr x9, [x9, #16]', f'b {at(14):#x}',
        f'bl {m.INSERT:#x}', 'ldr x0, [sp, #24]', 'ldp x29, x30, [sp], #32', 'ret',
    ], d

    # A changed QuartzCore function body is refused and nothing is written.
    data = bytearray(path.read_bytes())
    body = 0x1000 + m.WRAPPER - TEXT + 8
    data[body] ^= 0xFF
    path.write_bytes(bytes(data))
    code, out = run(root, '--undo')
    assert code != 0 and 'differ' in out
    data[body] ^= 0xFF
    path.write_bytes(bytes(data))

    code, out = run(root, '--undo')
    assert code == 0 and 'restored' in out, out
    assert path.read_bytes() == original
    assert run(root, '--undo') == (0, 'not patched; nothing to undo\n')

    # Wrong cache build is refused.
    data = bytearray(original)
    data[0x58] ^= 1
    path.write_bytes(bytes(data))
    code, out = run(root, '--dry-run')
    assert code != 0 and 'UUID' in out

    # Zero pages only inside the image or in the stub mapping are not used.
    build(root, outside_zero=False)
    code, out = run(root, '--dry-run')
    assert code != 0 and 'no all-zero page' in out, out

print('CA software surface patch: synthetic cache patch/verify/idempotent/undo, byte and UUID refusal, routine disassembly PASS')
