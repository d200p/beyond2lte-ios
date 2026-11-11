#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise the actual offline ARM64e precompiler on synthetic Mach-O and dyld-cache fixtures.

The tool is compiled from tools/b2l-precompile.c with the shared converter under
ASan/UBSan. Instruction words are assembled by the host toolchain, never typed by
hand. An independent Python model of the kernel converter supplies expectations.
Optional argument: a real dyld_shared_cache_arm64e main file for a report-only run.
"""
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid

repo = pathlib.Path(__file__).resolve().parents[1]

MH_MAGIC_64 = 0xFEEDFACF
CPU_ARM64 = 0x0100000C
CPU_X86_64 = 0x01000007
LC_SEGMENT_64 = 0x19
LC_UUID = 0x1B
PURE = 0x80000000
SOME = 0x400
STUBS = 0x8

ASM = [
    ('pacibsp', 'HINT'), ('autibsp', 'HINT'), ('paciasp', 'HINT'), ('xpaclri', 'HINT'),
    ('retab', 'RET'), ('retaa', 'RET'),
    ('braa x0, x1', 'BRANCH'), ('brab x2, x3', 'BRANCH'), ('blraa x4, x5', 'BRANCH'), ('blrab x30, x30', 'BRANCH'),
    ('blrabz x6', 'BRANCH'), ('braaz x7', 'BRANCH'), ('brabz x30', 'BRANCH'), ('blraaz x8', 'BRANCH'),
    ('ldapr x0, [x1]', 'LDAPR'), ('ldapr w2, [x3]', 'LDAPR'), ('ldaprb w3, [x4]', 'LDAPR'), ('ldaprh w5, [x6]', 'LDAPR'),
    ('ldapr x9, [sp]', 'LDAPR'),
    ('pacia x0, x1', 'PAC'), ('autda x2, x3', 'PAC'), ('pacdb x4, sp', 'PAC'), ('autib x30, sp', 'PAC'),
    ('paciza x4', 'PAC'), ('autdzb x5', 'PAC'), ('xpaci x5', 'PAC'), ('xpacd x6', 'PAC'),
    ('pacga x0, x1, x2', 'PACGA'), ('pacga x8, x8, x10', 'PACGA'),
    ('pacga x3, x4, sp', 'PACGA'), ('pacga xzr, x1, x2', 'PACGA'), ('pacga x9, xzr, x9', 'PACGA'),
    ('ldraa x0, [x1, #8]!', 'LDRA'), ('ldrab x2, [x3, #-16]', 'LDRA'),
    ('stp x29, x30, [sp, #-16]!', 'NONE'), ('mov x0, #1', 'NONE'), ('add x1, x2, x3', 'NONE'), ('ret', 'NONE'),
    ('nop', 'NONE'), ('ldr x0, [x1]', 'NONE'), ('ldar x4, [x5]', 'NONE'), ('br x6', 'NONE'), ('blr x7', 'NONE'),
    ('bl .', 'NONE'), ('b .', 'NONE'), ('cbz x0, .', 'NONE'), ('mrs x0, tpidrro_el0', 'NONE'),
]


def model_native(i):
    """Independent restatement of b2l_nc_native(); returns None when excluded."""
    if (i & 0xFFFFE000) == 0xDAC10000 or (i & 0xFFFFE3E0) == 0xDAC123E0 or \
       (i & 0xFFFFFFE0) == 0xDAC143E0 or (i & 0xFFFFFFE0) == 0xDAC147E0:
        return 0xD503201F
    if i in (0xD50320FF, 0xD503211F, 0xD503215F, 0xD503219F, 0xD50321DF, 0xD503231F, 0xD503233F,
             0xD503235F, 0xD503237F, 0xD503239F, 0xD50323BF, 0xD50323DF, 0xD50323FF):
        return 0xD503201F
    if i in (0xD65F0BFF, 0xD65F0FFF):
        return 0xD65F03C0
    if (i & 0xFFFFF800) in (0xD71F0800, 0xD73F0800) or (i & 0xFFFFF81F) in (0xD61F081F, 0xD63F081F):
        rn = (i >> 5) & 31
        if rn == 31:
            return None
        return (0xD63F0000 if i & 0x200000 else 0xD61F0000) | (rn << 5)
    if (i & 0x3FFFFC00) == 0x38BFC000:
        return (i & 0xC00003FF) | 0x08DFFC00
    return None


def assemble(lines):
    """Return the little-endian instruction words for the given assembly lines."""
    text = '\n'.join(lines) + '\n'
    llvm_mc = os.environ.get('LLVM_MC', '/opt/homebrew/opt/llvm/bin/llvm-mc')
    if sys.platform == 'darwin' and shutil.which('clang'):
        with tempfile.TemporaryDirectory() as tmp:
            src = pathlib.Path(tmp) / 'f.s'
            obj = pathlib.Path(tmp) / 'f.o'
            src.write_text('.text\n' + text)
            subprocess.run(['clang', '-arch', 'arm64e', '-c', str(src), '-o', str(obj)], check=True)
            data = obj.read_bytes()
        offset, size = macho_text_section(data)
        words = list(struct.unpack('<%dI' % (size // 4), data[offset:offset + size]))
    elif os.path.exists(llvm_mc):
        r = subprocess.run([llvm_mc, '-triple=aarch64', '-mattr=+pauth,+rcpc', '-show-encoding'],
                           input=text, capture_output=True, text=True, check=True)
        words = []
        for line in r.stdout.splitlines():
            m = re.search(r'encoding: \[([^]]+)\]', line)
            if m:
                words.append(int.from_bytes(bytes(int(v.strip(), 16) for v in m[1].split(',')), 'little'))
    else:
        raise SystemExit('Need Apple clang (Darwin) or LLVM_MC to assemble fixtures')
    assert len(words) == len(lines), (len(words), len(lines))
    return words


def macho_text_section(data):
    ncmds, sizeofcmds = struct.unpack_from('<II', data, 16)
    at = 32
    for _ in range(ncmds):
        cmd, n = struct.unpack_from('<II', data, at)
        if cmd == LC_SEGMENT_64:
            nsects = struct.unpack_from('<I', data, at + 64)[0]
            for j in range(nsects):
                s = at + 72 + j * 80
                name = data[s:s + 16].rstrip(b'\0')
                if name == b'__text':
                    size, offset = struct.unpack_from('<QI', data, s + 40)
                    return offset, size
        at += n
    raise AssertionError('no __text')


def words_bytes(words):
    return struct.pack('<%dI' % len(words), *words)


STUB_ASM = ['adrp x17, 0', 'add x17, x17, #0x10', 'ldr x16, [x17]', 'braa x16, x17',
            'adrp x1, 0', 'add x1, x1, #0x20', 'b .', 'brk #1',
            'adrp x16, 0', 'ldr x16, [x16, #8]', 'braaz x16', 'nop']
STUB_REGION = b''


def section(sectname, segname, addr, size, offset, flags):
    return struct.pack('<16s16sQQIIIIIIII', sectname, segname, addr, size, offset, 2, 0, 0, flags, 0, 0, 0)


def segment(segname, vmaddr, vmsize, fileoff, filesize, initprot, sections):
    body = b''.join(sections)
    return struct.pack('<II16sQQQQiiII', LC_SEGMENT_64, 72 + len(body), segname, vmaddr, vmsize,
                       fileoff, filesize, 7, initprot, len(sections), 0) + body


def header(cputype, subtype, commands):
    body = b''.join(commands)
    return struct.pack('<IIIIIIII', MH_MAGIC_64, cputype, subtype, 2, len(commands), len(body), 0, 0) + body


def uuid_command(value=None):
    return struct.pack('<II', LC_UUID, 24) + (value or uuid.uuid4().bytes)


class Fixture:
    """A thin ARM64e Mach-O with code, stub, data-in-text and non-executable PURE sections."""

    def __init__(self, code, stubs, base=0x100000000, subtype=2, cputype=CPU_ARM64):
        self.code, self.stubs = code, stubs
        text_off, stub_off, const_off = 0x400, 0x800, 0xC00
        data_off = 0x1000
        commands = [
            segment(b'__TEXT', base, 0x1000, 0, 0x1000, 5, [
                section(b'__text', b'__TEXT', base + text_off, len(code), text_off, PURE | SOME),
                section(b'__auth_stubs', b'__TEXT', base + stub_off, len(stubs), stub_off, PURE | SOME | STUBS),
                section(b'__const', b'__TEXT', base + const_off, len(code), const_off, 0),
            ]),
            segment(b'__DATA', base + 0x1000, 0x1000, data_off, 0x1000, 3, [
                section(b'__data', b'__DATA', base + 0x1000, len(code), data_off, PURE | SOME),
            ]),
            uuid_command(),
        ]
        image = bytearray(0x2000)
        h = header(cputype, subtype, commands)
        image[:len(h)] = h
        image[text_off:text_off + len(code)] = code
        image[stub_off:stub_off + len(stubs)] = stubs
        image[const_off:const_off + len(code)] = code
        image[data_off:data_off + len(code)] = code
        self.image = bytes(image)
        self.ranges = {'text': (text_off, len(code)), 'stubs': (stub_off, len(stubs)),
                       'const': (const_off, len(code)), 'data': (data_off, len(code))}

    def expected(self, base=0, convert_text=True):
        out = bytearray(self.image)
        for key in ('text', 'stubs'):
            if key == 'text' and not convert_text:
                continue
            off, n = self.ranges[key]
            for at in range(off, off + n, 4):
                w = struct.unpack_from('<I', out, at)[0]
                native = model_native(w)
                if native is not None:
                    struct.pack_into('<I', out, at, native)
        return bytes(out)


def fat(slices):
    """Big-endian FAT_MAGIC container; slices is a list of (cputype, subtype, bytes)."""
    out = bytearray(struct.pack('>II', 0xCAFEBABE, len(slices)))
    at = 0x4000
    archs = []
    for cputype, subtype, data in slices:
        archs.append((cputype, subtype, at, len(data)))
        at += (len(data) + 0x3FFF) & ~0x3FFF
    for cputype, subtype, off, size in archs:
        out += struct.pack('>IIIII', cputype, subtype, off, size, 14)
    out += bytes(0x4000 - len(out))
    for (cputype, subtype, off, size), (_, _, data) in zip(archs, slices):
        out += data + bytes(((len(data) + 0x3FFF) & ~0x3FFF) - len(data))
    return bytes(out)


def cache_header(mapping_count, images_text_offset, images_text_count, subcache_offset, subcache_count,
                 cache_uuid, dyld_mh=0):
    h = bytearray(0x238)
    h[:16] = b'dyld_v1  arm64e\0'
    struct.pack_into('<II', h, 0x10, 0x238, mapping_count)
    h[0x58:0x68] = cache_uuid
    struct.pack_into('<Q', h, 0x78, dyld_mh)
    struct.pack_into('<QQ', h, 0x88, images_text_offset, images_text_count)
    struct.pack_into('<II', h, 0x188, subcache_offset, subcache_count)
    return h


def mapping(address, size, offset, initprot):
    return struct.pack('<QQQII', address, size, offset, 7, initprot)


class CacheGroup:
    """Main cache with an executable and a data mapping, one subcache, four images."""

    MAIN_TEXT, MAIN_DATA, SUB_TEXT = 0x180000000, 0x190000000, 0x1A0000000

    def __init__(self, code, stubs, broken_section=False, flagged=False):
        """flagged: give the stub island its own mapping marked DYLD_CACHE_MAPPING_TEXT_STUBS
        in the mapping-and-slide table, followed by one all-zero page usable for routines."""
        self.code, self.stubs, self.flagged = code, stubs, flagged
        self.sub_uuid = uuid.uuid4().bytes
        images = [  # (address, path, convert __text)
            (self.MAIN_TEXT, b'/usr/lib/libA.dylib'),
            (self.SUB_TEXT, b'/usr/lib/libB.dylib'),
            (self.MAIN_TEXT + 0x1000, b'/usr/lib/system/libcorecrypto.dylib'),
        ]
        dyld_addr = self.MAIN_TEXT + 0x2000
        main = bytearray(0x8000 if flagged else 0x6000)
        if flagged:
            # text mapping ends with one all-zero padding page; the stub island has one too
            main[:0x238] = cache_header(3, 0x300, len(images), 0x400, 1, uuid.uuid4().bytes, dyld_addr)
            maps = [(self.MAIN_TEXT, 0x4000, 0x1000, 5, 0), (self.MAIN_TEXT + 0x4000, 0x2000, 0x5000, 5, 8),
                    (self.MAIN_DATA, 0x1000, 0x7000, 3, 0)]
            struct.pack_into('<II', main, 0x138, 0x600, len(maps))
            for i, (a, n, off, prot, flags) in enumerate(maps):
                main[0x238 + 32 * i:0x258 + 32 * i] = mapping(a, n, off, prot)
                struct.pack_into('<QQQQQQII', main, 0x600 + 56 * i, a, n, off, 0, 0, flags, 5, prot)
            self.data_off, self.stub_off = 0x7000, 0x5000
        else:
            main[:0x238] = cache_header(2, 0x300, len(images), 0x400, 1, uuid.uuid4().bytes, dyld_addr)
            main[0x238:0x258] = mapping(self.MAIN_TEXT, 0x4000, 0x1000, 5)
            main[0x258:0x278] = mapping(self.MAIN_DATA, 0x1000, 0x5000, 3)
            self.data_off, self.stub_off = 0x5000, 0x4000
        path_at = 0x500
        for i, (address, path) in enumerate(images):
            main[0x300 + i * 32:0x300 + i * 32 + 32] = uuid.uuid4().bytes + struct.pack('<QII', address, 0x1000, path_at)
            main[path_at:path_at + len(path) + 1] = path + b'\0'
            path_at += len(path) + 1
        main[0x400:0x438] = self.sub_uuid + struct.pack('<Q', 0x20000000) + b'.01'.ljust(32, b'\0')
        self.image_offsets = {}
        for address in (self.MAIN_TEXT, self.MAIN_TEXT + 0x1000, dyld_addr):
            off = 0x1000 + (address - self.MAIN_TEXT)
            main[off:off + 0x1000] = self.image(address, broken=(broken_section and address == self.MAIN_TEXT))
            self.image_offsets[address] = ('main', off)
        # shared stub region outside every image: auth stubs then objc stubs, zero padded
        self.stub_region = STUB_REGION
        main[self.stub_off:self.stub_off + len(STUB_REGION)] = STUB_REGION
        main[self.data_off:self.data_off + len(code)] = code  # data mapping must never change
        sub = bytearray(0x4000)
        sub[:0x238] = cache_header(1, 0, 0, 0, 0, self.sub_uuid)
        sub[0x238:0x258] = mapping(self.SUB_TEXT - 0x2000, 0x3000, 0x1000, 5)
        # first mapped page is header-like data (mixed words): must be skipped, not converted
        sub[0x1000:0x1000 + 0x300] = b''.join(struct.pack('<I', 0xD71F0A11 if i % 7 == 0 else i + 1) for i in range(0xC0))
        sub[0x2000:0x2000 + len(STUB_REGION)] = STUB_REGION
        sub[0x3000:0x4000] = self.image(self.SUB_TEXT)
        self.image_offsets[self.SUB_TEXT] = ('sub', 0x3000)
        self.main, self.sub = bytes(main), bytes(sub)

    def image(self, base, broken=False):
        text_addr = CacheGroup.MAIN_DATA + 0x100 if broken else base + 0x400
        commands = [
            segment(b'__TEXT', base, 0x1000, 0, 0x1000, 5, [
                section(b'__text', b'__TEXT', text_addr, len(self.code), 0x400, PURE | SOME),
                section(b'__auth_stubs', b'__TEXT', base + 0x800, len(self.stubs), 0x800, PURE | SOME | STUBS),
                section(b'__const', b'__TEXT', base + 0xC00, len(self.code), 0xC00, 0),
            ]),
            uuid_command(),
        ]
        image = bytearray(0x1000)
        h = header(CPU_ARM64, 2, commands)
        image[:len(h)] = h
        image[0x400:0x400 + len(self.code)] = self.code
        image[0x800:0x800 + len(self.stubs)] = self.stubs
        image[0xC00:0xC00 + len(self.code)] = self.code
        return bytes(image)

    def expected(self, exclude_corecrypto=True):
        main, sub = bytearray(self.main), bytearray(self.sub)
        for target, off in ((main, self.stub_off), (sub, 0x2000)):
            for at in range(off, off + len(self.stub_region), 4):
                native = model_native(struct.unpack_from('<I', target, at)[0])
                if native is not None:
                    struct.pack_into('<I', target, at, native)
        for address, (where, off) in self.image_offsets.items():
            target = main if where == 'main' else sub
            is_corecrypto = address == self.MAIN_TEXT + 0x1000
            for sect_off, n in ((0x400, len(self.code)), (0x800, len(self.stubs))):
                if sect_off == 0x400 and is_corecrypto and exclude_corecrypto:
                    continue
                for at in range(off + sect_off, off + sect_off + n, 4):
                    native = model_native(struct.unpack_from('<I', target, at)[0])
                    if native is not None:
                        struct.pack_into('<I', target, at, native)
        return bytes(main), bytes(sub)


def keyless_asm(d, n, m):
    reg = lambda r: 'xzr' if r == 31 else f'x{r}'
    D = reg(d)
    return [f'eor {D}, {reg(n)}, {reg(m)}, ror #29', f'eor {D}, {D}, {D}, lsr #31',
            f'add {D}, {D}, {D}, lsl #21', f'eor {D}, {D}, {D}, lsr #29',
            f'add {D}, {D}, {D}, lsl #13', f'eor {D}, {D}, {D}, lsr #33',
            f'and {D}, {D}, #0xffffffff00000000']


def check_trampolines(exe, tmp, code, stubs, asm_words, kinds):
    """Route PACGA to keyless routines in a flagged stub island; check placement, branches and limits."""
    root = tmp / 'pacga'
    root.mkdir()
    group = CacheGroup(code, stubs, flagged=True)
    main_path, sub_path = root / 'dyld_shared_cache_arm64e', root / 'dyld_shared_cache_arm64e.01'
    main_path.write_bytes(group.main)
    sub_path.write_bytes(group.sub)
    # Without the option nothing about PACGA changes.
    run(exe, '--write', '--report', tmp / 'p0.json', main_path)
    exp_main, exp_sub = group.expected()
    assert main_path.read_bytes() == exp_main and sub_path.read_bytes() == exp_sub
    main_path.write_bytes(group.main)
    sub_path.write_bytes(group.sub)
    run(exe, '--write', '--pacga-trampolines', '--report', tmp / 'p1.json', main_path)
    s = summary(tmp / 'p1.json')['summary']
    main, sub = main_path.read_bytes(), sub_path.read_bytes()
    pacga_offsets = [i * 4 for i, k in enumerate(kinds) if k == 'PACGA']
    # first cave in address order: the text mapping's zero padding page
    cave_file, cave_va = 0x4000, CacheGroup.MAIN_TEXT + 0x3000
    routed = nop = kept = 0
    used = set()
    exp = {'main': bytearray(exp_main), 'sub': bytearray(exp_sub)}
    exp_main = exp['main']
    for address, (where, off) in group.image_offsets.items():
        data = main if where == 'main' else sub
        corecrypto = address == CacheGroup.MAIN_TEXT + 0x1000
        for rel in pacga_offsets:
            file_at, va = off + 0x400 + rel, address + 0x400 + rel
            original = asm_words[rel // 4]
            word = struct.unpack_from('<I', data, file_at)[0]
            d, n, m = original & 31, (original >> 5) & 31, (original >> 16) & 31
            if corecrypto or (d != 31 and (where == 'sub' or m == 31)):
                assert word == original, (hex(va), hex(word))  # excluded, out of range, or SP operand
                kept += 1
                continue
            if d == 31:  # result discarded: a NOP needs no routine, so range does not matter
                assert word == 0xD503201F
                nop += 1
            else:
                assert word >> 26 == 0b000101, hex(word)
                delta = ((word & 0x03FFFFFF) ^ 0x02000000) - 0x02000000
                slot = va + 4 * delta
                assert cave_va <= slot < cave_va + 0x1000 and not (slot - cave_va) % 32 and slot not in used
                used.add(slot)
                sf = cave_file + slot - cave_va
                body = list(struct.unpack_from('<8I', main, sf))
                assert body[:7] == assemble(keyless_asm(d, n, m)), (hex(va), [hex(w) for w in body])
                back = ((body[7] & 0x03FFFFFF) ^ 0x02000000) - 0x02000000
                assert body[7] >> 26 == 0b000101 and slot + 28 + 4 * back == va + 4
                routed += 1
                exp_main[sf:sf + 32] = main[sf:sf + 32]
            exp[where][file_at:file_at + 4] = data[file_at:file_at + 4]
    # libA and dyld route 3 each; kept = corecrypto 5 + sub-cache 4 (out of range) + 2 SP operands
    assert routed == 2 * 3 and nop == 3 and kept == 5 + 4 + 2, (routed, nop, kept)
    assert s['pacga_routed'] == routed and s['pacga_nop'] == nop and s['pacga_kept'] == kept and s['caves'] == 2, s
    assert main == bytes(exp['main']) and sub == bytes(exp['sub'])  # nothing else in either file moved
    assert not any(main[cave_file + 32 * routed:cave_file + 0x1000]) and not any(main[0x6000:0x7000])
    # Idempotent: a second pass finds nothing to route and changes nothing.
    run(exe, '--write', '--pacga-trampolines', '--verify', '--report', tmp / 'p2.json', main_path)
    s2 = summary(tmp / 'p2.json')['summary']
    assert main_path.read_bytes() == main and sub_path.read_bytes() == sub and s2['pacga_routed'] == 0, s2
    print(f'pacga routines: {routed} routed, {nop} as NOP, {kept} left trapping (SP/excluded/out of range)')


def build_tool(tmp):
    cc = os.environ.get('CC', 'clang')
    exe = tmp / 'b2l-precompile'
    subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I' + str(repo / 'common/cache'), '-I' + str(repo / 'xnu-overlay/osfmk/arm64'),
                    str(repo / 'tools/b2l-precompile.c'), str(repo / 'common/cache/b2l_cache_core.c'),
                    str(repo / 'common/cache/CacheSha.c'), '-o', str(exe)], check=True)
    return exe


def run(exe, *args, expect=0):
    r = subprocess.run([str(exe)] + [str(a) for a in args], capture_output=True, text=True)
    assert r.returncode == expect, (args, r.returncode, r.stdout, r.stderr)
    return r


def summary(report_path):
    with open(report_path) as f:
        return json.load(f)


def main():
    asm_words = assemble([a for a, _ in ASM])
    kinds = [k for _, k in ASM]
    for word, kind in zip(asm_words, kinds):
        native = model_native(word)
        assert (native is not None) == (kind in ('HINT', 'RET', 'BRANCH', 'LDAPR', 'PAC')), (hex(word), kind)
    code = words_bytes(asm_words)
    stubs = words_bytes([w for w, k in zip(asm_words, kinds) if k == 'BRANCH'] * 3)
    global STUB_REGION
    STUB_REGION = words_bytes(assemble(STUB_ASM) * 20) + bytes(64)
    convertible = sum(1 for w in asm_words if model_native(w) is not None)
    stub_region_sites = sum(1 for w in assemble(STUB_ASM) if model_native(w) is not None) * 20
    pacga = sum(1 for k in kinds if k == 'PACGA')
    ldra = sum(1 for k in kinds if k == 'LDRA')

    with tempfile.TemporaryDirectory(prefix='b2l-precompile.') as tmpname:
        tmp = pathlib.Path(tmpname)
        exe = build_tool(tmp)
        root = tmp / 'root'
        (root / 'usr/lib/system').mkdir(parents=True)
        (root / 'bin').mkdir()
        outside = tmp / 'outside'
        outside.mkdir()

        thin = Fixture(code, stubs)
        (root / 'bin/thin').write_bytes(thin.image)
        other = Fixture(code, stubs, subtype=0)  # plain arm64 slice must stay untouched
        x86 = header(CPU_X86_64, 3, [uuid_command()]) + bytes(0x100)
        fat_blob = fat([(CPU_X86_64, 3, x86), (CPU_ARM64, 2, thin.image), (CPU_ARM64, 0, other.image)])
        (root / 'bin/fat').write_bytes(fat_blob)
        (root / 'usr/lib/system/libcorecrypto.dylib').write_bytes(thin.image)
        os.link(root / 'bin/thin', root / 'bin/thin-link')
        (outside / 'victim').write_bytes(thin.image)
        os.symlink(outside / 'victim', root / 'bin/victim-link')
        (root / 'bin/plain.txt').write_bytes(code)  # same words, no Mach-O magic
        os.utime(root / 'bin/thin', (946684800, 946684800))
        group = CacheGroup(code, stubs)
        (root / 'dyld_shared_cache_arm64e').write_bytes(group.main)
        (root / 'dyld_shared_cache_arm64e.01').write_bytes(group.sub)
        before = {p: p.read_bytes() for p in root.rglob('*') if p.is_file() and not p.is_symlink()}

        # 1. report mode changes nothing and --verify reports residual work
        r = run(exe, '--verify', '--report', tmp / 'r1.json', root, expect=2)
        s = summary(tmp / 'r1.json')
        assert s['mode'] == 'report' and s['summary']['errors'] == 0
        assert s['summary']['files_seen'] == 6, s['summary']  # hard link counted once, symlink skipped
        assert s['summary']['caches'] == 1 and s['summary']['cache_files'] == 2 and s['summary']['images'] == 4
        assert s['summary']['arm64e_slices'] == 3 and s['summary']['other_slices'] == 2
        for p, data in before.items():
            assert p.read_bytes() == data, p
        # thin, fat(arm64e slice), corecrypto file (text excluded), 4 cache images
        text_sites = convertible
        stub_sites = len(stubs) // 4
        expected_converted = (text_sites + stub_sites) * 2 + stub_sites + (text_sites + stub_sites) * 3 + stub_sites + 2 * stub_region_sites
        assert s['summary']['counts']['converted'] == expected_converted, (s['summary']['counts'], expected_converted)
        assert s['summary']['counts']['excluded'] == text_sites * 2
        assert s['summary']['counts']['pacga'] == pacga * 7 and s['summary']['counts']['ldra'] == ldra * 7

        # 2. write mode converts exactly the modelled words and preserves metadata
        r = run(exe, '--write', '--report', tmp / 'r2.json', root)
        s = summary(tmp / 'r2.json')
        assert s['mode'] == 'write' and s['summary']['errors'] == 0
        assert s['summary']['counts']['converted'] == expected_converted
        assert (root / 'bin/thin').read_bytes() == thin.expected()
        assert (root / 'bin/thin-link').read_bytes() == thin.expected()
        assert (root / 'usr/lib/system/libcorecrypto.dylib').read_bytes() == thin.expected(convert_text=False)
        expected_fat = fat([(CPU_X86_64, 3, x86), (CPU_ARM64, 2, thin.expected()), (CPU_ARM64, 0, other.image)])
        assert (root / 'bin/fat').read_bytes() == expected_fat
        main_exp, sub_exp = group.expected()
        assert (root / 'dyld_shared_cache_arm64e').read_bytes() == main_exp
        assert (root / 'dyld_shared_cache_arm64e.01').read_bytes() == sub_exp
        assert (outside / 'victim').read_bytes() == thin.image
        assert (root / 'bin/plain.txt').read_bytes() == code
        assert int(os.stat(root / 'bin/thin').st_mtime) == 946684800
        assert os.stat(root / 'bin/thin').st_nlink == 2
        entries = {e['path']: e for e in s['files']}
        assert entries[str(root / 'dyld_shared_cache_arm64e')]['kind'] == 'dyld_cache'
        assert entries[str(root / 'bin/fat')]['kind'] == 'fat'

        # 3. idempotent: a second pass finds nothing and --verify passes
        r = run(exe, '--write', '--verify', '--report', tmp / 'r3.json', root)
        assert summary(tmp / 'r3.json')['summary']['counts']['converted'] == 0
        run(exe, '--verify', root)
        after = {p: p.read_bytes() for p in root.rglob('*') if p.is_file() and not p.is_symlink()}
        assert set(after) == set(before)
        for p in after:
            if p.name in ('plain.txt',):
                assert after[p] == before[p]

        # 4. exclusion options
        alt = tmp / 'alt'
        shutil.copytree(root, alt, symlinks=True)
        for p in ('bin/thin', 'usr/lib/system/libcorecrypto.dylib'):
            (alt / p).write_bytes(thin.image)
        (alt / 'dyld_shared_cache_arm64e').write_bytes(group.main)
        (alt / 'dyld_shared_cache_arm64e.01').write_bytes(group.sub)
        run(exe, '--write', '--no-default-exclusions', alt)
        assert (alt / 'usr/lib/system/libcorecrypto.dylib').read_bytes() == thin.expected()
        assert (alt / 'dyld_shared_cache_arm64e').read_bytes() == group.expected(exclude_corecrypto=False)[0]
        (alt / 'bin/thin').write_bytes(thin.image)
        run(exe, '--write', '--exclude-text', '/bin/thin', alt)
        assert (alt / 'bin/thin').read_bytes() == thin.expected(convert_text=False)

        # 5. malformed inputs fail closed without writing
        bad = tmp / 'bad'
        bad.mkdir()
        truncated = bytearray(thin.image)
        struct.pack_into('<I', truncated, 20, 0x1F00)  # sizeofcmds beyond the file image
        (bad / 'truncated').write_bytes(truncated)
        oversize = bytearray(thin.image)
        struct.pack_into('<Q', oversize, 32 + 72 + 40, 0x10000)  # __text size beyond the file
        (bad / 'oversize').write_bytes(oversize)
        for name in ('truncated', 'oversize'):
            run(exe, '--write', bad / name, expect=1)
            assert (bad / name).read_bytes() in (bytes(truncated), bytes(oversize))
        broken = CacheGroup(code, stubs, broken_section=True)
        (bad / 'dyld_shared_cache_arm64e').write_bytes(broken.main)
        (bad / 'dyld_shared_cache_arm64e.01').write_bytes(broken.sub)
        run(exe, '--write', bad / 'dyld_shared_cache_arm64e', expect=1)
        assert (bad / 'dyld_shared_cache_arm64e').read_bytes() == broken.main
        assert (bad / 'dyld_shared_cache_arm64e.01').read_bytes() == broken.sub
        (bad / 'dyld_shared_cache_arm64e.01').unlink()
        run(exe, '--write', bad / 'dyld_shared_cache_arm64e', expect=1)
        assert (bad / 'dyld_shared_cache_arm64e').read_bytes() == broken.main
        (bad / 'dyld_shared_cache_arm64e').unlink()
        (bad / 'dyld_shared_cache_arm64e.01').write_bytes(group.sub)
        r = run(exe, '--write', bad / 'dyld_shared_cache_arm64e.01')
        assert 'without their main cache' in r.stderr and (bad / 'dyld_shared_cache_arm64e.01').read_bytes() == group.sub
        check_trampolines(exe, tmp, code, stubs, asm_words, kinds)
        print(f'precompile fixtures passed: {len(asm_words)} assembled words, {expected_converted} conversions modelled')

        if len(sys.argv) > 1:
            real = pathlib.Path(sys.argv[1])
            r = run(exe, '--quiet', '--report', tmp / 'real.json', real)
            s = summary(tmp / 'real.json')['summary']
            assert s['errors'] == 0 and s['caches'] == 1 and s['images'] > 0 and s['counts']['converted'] > 0, s
            print(f"real cache report-only: files={s['cache_files']} images={s['images']} "
                  f"convertible={s['counts']['converted']} excluded={s['counts']['excluded']} pacga={s['counts']['pacga']}")


if __name__ == '__main__':
    main()
