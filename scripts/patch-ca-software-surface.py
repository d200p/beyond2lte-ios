#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Register IOSurface-backed surfaces in Core Animation's software context.

Core Animation's OGL layer keeps every live surface of a context on a list
(context + 0x58, or + 0x60 for surfaces flagged 0x400). Wrapping an IOSurface is
context vtable slot 92. The Metal context's wrapper links the new surface onto
that list; the software context's wrapper (IOSurfaceLock + base address) does
not. CA::OGL::AsynchronousNode::retain_surface wraps an IOSurface through slot
92 and immediately calls CA::OGL::Context::push_surface, which asserts
`found != slist->end ()` (ogl-context.cpp:1186) when the surface is not listed.
Devices always run the Metal context; this port has no GPU, so backboardd runs
the software context and aborts (taking SpringBoard with it) whenever an
asynchronous node renders, typically when Home leaves an app.

The patch makes the software wrapper behave like the Metal one. Its entry
instruction (a NOP once precompiled) becomes a branch to a routine placed in an
all-zero 4 KiB page outside every image in a text mapping, the rule b2l-precompile uses for its
PACGA routines, so a later precompiler run neither reuses nor rejects it. The
routine calls the unchanged wrapper body, and for a non-NULL result calls
QuartzCore's own list-insert function unless the surface is already listed.
Surface removal already unlinks listed surfaces, so nothing else changes.

Only the iOS 26.6.2 (23G90) arm64e cache precompiled by b2l-precompile is
accepted: the cache UUID and both functions' bytes must match exactly.
Run after b2l-precompile --write. --dry-run first, --verify afterwards,
--undo restores the original bytes.
"""
import argparse
import hashlib
import os
import pathlib
import struct
import sys

CACHE = 'private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e'
UUID = 'cc670b308fdb34daafab7aee1251384b'
WRAPPER = 0x18410D2C8           # software context vtable slot 92
WRAPPER_LEN = 0xC0
WRAPPER_SHA = '74f85af044fe8c43010c152c8b73759c4b4dd5adb2020b8c2b99e210c5c9aac8'
INSERT = 0x1840D8C58            # context surface-list insert (Metal wrapper uses it)
INSERT_LEN = 0x94
INSERT_SHA = 'a0daa8dcd7e1e352693e58f4117bbcf1aaba78f486f9771cd9b6e90a09bc428c'
NOP = 0xD503201F
PAGE = 0x1000                   # b2l-precompile judges cave pages at 4 KiB
TEXT_STUBS = 0x8                # its stub pages must hold only stub words
BRANCH_RANGE = 1 << 27          # B/BL: +-128 MiB
ROUTINE_SIZE = 23 * 4


def branch(pc, target, link=False):
    delta = target - pc
    if delta % 4 or not -BRANCH_RANGE <= delta < BRANCH_RANGE:
        raise ValueError(f'branch {pc:#x} -> {target:#x} out of range')
    return (0x94000000 if link else 0x14000000) | ((delta >> 2) & 0x3FFFFFF)


def cond(pc, target, base):
    """CBZ (base = 0xB4000000 | Rt) or B.cond (base = 0x54000000 | cond)."""
    delta = target - pc
    if delta % 4 or not -(1 << 20) <= delta < (1 << 20):
        raise ValueError('conditional branch out of range')
    return base | (((delta >> 2) & 0x7FFFF) << 5)


def routine(cave):
    """Call the wrapper body; insert a non-NULL result unless already listed."""
    body = WRAPPER + 4  # the original function after its entry NOP
    words = [
        0xA9BE7BFD,                 # 0  stp  x29, x30, [sp, #-32]!
        0x910003FD,                 # 1  mov  x29, sp
        0xF9000BE0,                 # 2  str  x0, [sp, #16]        context
        None,                       # 3  bl   body
        None,                       # 4  cbz  x0, done
        0xF9000FE0,                 # 5  str  x0, [sp, #24]        surface
        0xAA0003E1,                 # 6  mov  x1, x0
        0xF9400BE0,                 # 7  ldr  x0, [sp, #16]
        0xB9405C29,                 # 8  ldr  w9, [x1, #92]
        0xD2800B0A,                 # 9  mov  x10, #0x58
        0x7216013F,                 # 10 tst  w9, #0x400
        0xD2800C0B,                 # 11 mov  x11, #0x60
        0x9A8B014A,                 # 12 csel x10, x10, x11, eq
        0xF86A6809,                 # 13 ldr  x9, [x0, x10]        list head
        None,                       # 14 loop: cbz x9, insert
        0xEB01013F,                 # 15 cmp  x9, x1
        None,                       # 16 b.eq listed
        0xF9400929,                 # 17 ldr  x9, [x9, #16]        next
        None,                       # 18 b    loop
        None,                       # 19 insert: bl INSERT
        0xF9400FE0,                 # 20 listed: ldr x0, [sp, #24]
        0xA8C27BFD,                 # 21 done: ldp x29, x30, [sp], #32
        0xD65F03C0,                 # 22 ret
    ]
    at = lambda i: cave + 4 * i
    words[3] = branch(at(3), body, link=True)
    words[4] = cond(at(4), at(21), 0xB4000000 | 0)
    words[14] = cond(at(14), at(19), 0xB4000000 | 9)
    words[16] = cond(at(16), at(20), 0x54000000 | 0)
    words[18] = branch(at(18), at(14))
    words[19] = branch(at(19), INSERT, link=True)
    return struct.pack('<%dI' % len(words), *words)


class Cache:
    def __init__(self, path):
        self.path = path
        main = open(path, 'rb').read(0x1000)
        if main[:16].rstrip(b'\0') != b'dyld_v1  arm64e':
            sys.exit(f'not an arm64e dyld cache: {path}')
        self.uuid = main[0x58:0x68].hex()
        off, count = struct.unpack_from('<II', main, 0x188)
        header = open(path, 'rb').read(off + count * 56)
        suffixes = [''] + [header[off + i * 56 + 24:off + (i + 1) * 56].split(b'\0')[0].decode() for i in range(count)]
        self.maps = []  # (address, size, file offset, file, flags, initProt)
        for suffix in suffixes:
            h = open(path + suffix, 'rb').read(0x1000)
            moff, mcount = struct.unpack_from('<II', h, 0x138)
            for j in range(mcount):
                a, size, fo, _, _, flags, _, init = struct.unpack_from('<QQQQQQII', h, moff + j * 56)
                self.maps.append((a, size, fo, path + suffix, flags, init))

    def locate(self, va, n):
        for a, size, fo, f, flags, init in self.maps:
            if a <= va and va + n <= a + size:
                return f, fo + va - a, flags, init
        raise KeyError(f'{va:#x} not mapped')

    def read(self, va, n):
        f, off, _, _ = self.locate(va, n)
        with open(f, 'rb') as fh:
            fh.seek(off)
            return fh.read(n)

    def write(self, va, data):
        f, off, _, init = self.locate(va, len(data))
        if not init & 4:
            raise ValueError(f'{va:#x} is not executable')
        with open(f, 'r+b') as fh:
            fh.seek(off)
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())

    def image_ranges(self):
        """Executable segment VM ranges of every cached image."""
        main = open(self.path, 'rb').read(0x1000)
        at, count = struct.unpack_from('<II', main, 0x1C0)
        table = open(self.path, 'rb').read(at + count * 32)[at:]
        ranges = []
        for i in range(count):
            base = struct.unpack_from('<Q', table, i * 32)[0]
            head = self.read(base, 32)
            if struct.unpack_from('<I', head)[0] != 0xFEEDFACF:
                sys.exit(f'image {i} at {base:#x} has no Mach-O header')
            ncmds, sizeofcmds = struct.unpack_from('<II', head, 16)
            cmds = self.read(base + 32, sizeofcmds)
            pos = 0
            for _ in range(ncmds):
                cmd, n = struct.unpack_from('<II', cmds, pos)
                if cmd == 0x19:  # LC_SEGMENT_64
                    va, vm = struct.unpack_from('<QQ', cmds, pos + 24)
                    if struct.unpack_from('<I', cmds, pos + 60)[0] & 4 and vm:
                        ranges.append((va, va + vm))
                pos += n
        return ranges

    def find_cave(self, size):
        """Nearest all-zero 4 KiB page outside every image, in branch range.

        This is the page rule b2l-precompile uses for PACGA routines: executable
        cache mappings, outside all executable image segments, every byte zero.
        TEXT_STUBS mappings are left out: the precompiler rejects a stub page
        holding anything but stub words.
        """
        ranges = self.image_ranges()
        best = None
        for a, msize, fo, f, flags, init in self.maps:
            if not init & 4 or flags & TEXT_STUBS:
                continue
            lo = max(a, min(WRAPPER, INSERT) - BRANCH_RANGE + PAGE)
            hi = min(a + msize, max(WRAPPER, INSERT) + BRANCH_RANGE - PAGE)
            if lo >= hi:
                continue
            with open(f, 'rb') as fh:
                fh.seek(fo + (lo - a))
                data = fh.read(hi - lo)
            for p in range((-(lo - a)) % PAGE, len(data) - PAGE + 1, PAGE):
                va = lo + p
                if data.count(0, p, p + PAGE) != PAGE:
                    continue
                if any(s < va + PAGE and va < e for s, e in ranges):
                    continue
                if all(abs(t - va) < BRANCH_RANGE - PAGE for t in (WRAPPER, INSERT)) and \
                        (best is None or abs(va - WRAPPER) < abs(best - WRAPPER)):
                    best = va
        if best is None:
            sys.exit('no all-zero page outside images within branch range')
        assert size <= PAGE
        return best


def current_cave(cache):
    """Cave address if the wrapper entry already branches to our routine."""
    word = struct.unpack('<I', cache.read(WRAPPER, 4))[0]
    if word == NOP:
        return None
    if word >> 26 != 0x05:
        sys.exit(f'unexpected wrapper entry {word:#010x}')
    imm = word & 0x3FFFFFF
    target = WRAPPER + 4 * (imm - (1 << 26) if imm & (1 << 25) else imm)
    expected = routine(target)
    if cache.read(target, len(expected)) != expected:
        sys.exit(f'wrapper entry branches to {target:#x}, which is not this routine')
    return target


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('root', type=pathlib.Path, help='mounted iOS root, writable for patch/undo')
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument('--dry-run', action='store_true')
    mode.add_argument('--verify', action='store_true')
    mode.add_argument('--undo', action='store_true')
    a = ap.parse_args()
    path = a.root / CACHE
    if not path.is_file():
        sys.exit(f'no arm64e shared cache under {a.root}')
    cache = Cache(str(path))
    if cache.uuid != UUID:
        sys.exit(f'cache UUID {cache.uuid} is not iOS 26.6.2 (23G90) {UUID}')
    if hashlib.sha256(cache.read(INSERT, INSERT_LEN)).hexdigest() != INSERT_SHA:
        sys.exit('QuartzCore list-insert bytes differ (run b2l-precompile --write first)')
    cave = current_cave(cache)
    body = cache.read(WRAPPER + 4, WRAPPER_LEN - 4)
    if hashlib.sha256(struct.pack('<I', NOP) + body).hexdigest() != WRAPPER_SHA:
        sys.exit('software surface wrapper bytes differ (run b2l-precompile --write first)')

    if a.verify:
        if cave is None:
            sys.exit('not patched')
        print(f'verified: software wrapper {WRAPPER:#x} -> routine {cave:#x} -> insert {INSERT:#x}')
        return
    if a.undo:
        if cave is None:
            print('not patched; nothing to undo')
            return
        cache.write(WRAPPER, struct.pack('<I', NOP))
        cache.write(cave, bytes(len(routine(cave))))
        print(f'restored wrapper entry and zeroed {cave:#x}')
        return
    if cave is not None:
        print(f'already patched: wrapper {WRAPPER:#x} -> routine {cave:#x}')
        return
    cave = cache.find_cave(ROUTINE_SIZE)
    code = routine(cave)
    entry = struct.pack('<I', branch(WRAPPER, cave))
    print(f'wrapper {WRAPPER:#x}: nop -> b {cave:#x}; routine {len(code)} bytes at {cave:#x} '
          f'({os.path.basename(cache.locate(cave, 4)[0])})')
    if a.dry_run:
        print('dry run: nothing written')
        return
    cache.write(cave, code)     # routine first: the entry branch never points at zeros
    cache.write(WRAPPER, entry)
    print('patched')


if __name__ == '__main__':
    main()
