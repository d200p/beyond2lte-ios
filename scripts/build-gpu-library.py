#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Build the backboardd translation library and its kernel payload header.

The kernel publishes the library into the root at boot, so a BOOT image carries
it and the previous BOOT image removes it. Its file name includes its hash:
publication refuses to change an existing file, so each build is a new file.
The header is generated into the XNU tree only; like the other payloads it is
not repository content.
"""
import argparse
import hashlib
import pathlib
import plistlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
LIMIT = 4 * 1024 * 1024


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--xnu', type=pathlib.Path, default=ROOT / 'xnu')
    ap.add_argument('--output', type=pathlib.Path, default=ROOT / 'out/b2l-gpu.dylib')
    a = ap.parse_args()
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    a.output.parent.mkdir(parents=True, exist_ok=True)
    # Pointer authentication would trap into the kernel emulator on every call.
    # S10 port: the Mesa library's path (scripts/build-mesa-payload.py runs first).
    defines = []
    mesa = a.xnu / 'bsd/kern/b2l_mesa_payload.h'
    if mesa.is_file():
        for line in mesa.read_text().splitlines()[:8]:
            if line.startswith('#define B2L_MESA_LIBRARY '):
                defines.append('-DB2L_MESA_LIBRARY=' + line.split(' ', 2)[2])
    subprocess.run(['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64e-apple-ios26.0', '-isysroot', sdk,
                    *defines,
                    '-O2', '-Wall', '-Wextra', '-Werror', '-dynamiclib', '-fno-ptrauth-returns',
                    '-fno-ptrauth-calls', '-fno-ptrauth-intrinsics',
                    '-I', str(ROOT / 'xnu-overlay/iokit/Kernel/arm'),
                    '-framework', 'CoreFoundation', '-framework', 'IOKit',
                    # S10 port: Metal M2a's test (userspace/b2l-mtltest.c) is a Metal client
                    '-framework', 'IOSurface', '-lobjc',
                    '-install_name', '/usr/local/lib/b2l-gpu.dylib',
                    str(ROOT / 'userspace/b2l-gpu.c'), str(ROOT / 'userspace/b2l-mtltest.c'),
                    '-o', str(a.output)], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(a.output)], check=True)
    data = a.output.read_bytes()
    if len(data) > LIMIT:
        raise SystemExit('library exceeds the publication limit')
    tag = hashlib.sha256(data).hexdigest()[:12]
    rows = [','.join('0x%02x' % b for b in data[i:i + 20]) for i in range(0, len(data), 20)]
    # S10 port: the keyboard relief library for every app (userspace/b2l-kbd.c),
    # in /usr/lib, which app sandboxes may read and map.
    kbd_out = a.output.parent / 'b2l-kbd.dylib'
    subprocess.run(['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64e-apple-ios26.0', '-isysroot', sdk,
                    '-O2', '-Wall', '-Wextra', '-Werror', '-dynamiclib', '-fno-ptrauth-returns',
                    '-fno-ptrauth-calls', '-fno-ptrauth-intrinsics', '-fblocks',
                    '-framework', 'CoreFoundation', '-lobjc',
                    '-install_name', '/usr/lib/b2l-kbd.dylib',
                    str(ROOT / 'userspace/b2l-kbd.c'), '-o', str(kbd_out)], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(kbd_out)], check=True)
    kbd = kbd_out.read_bytes()
    if len(kbd) > LIMIT:
        raise SystemExit('keyboard library exceeds the publication limit')
    kbd_tag = hashlib.sha256(kbd).hexdigest()[:12]
    kbd_rows = [','.join('0x%02x' % b for b in kbd[i:i + 20]) for i in range(0, len(kbd), 20)]
    # S10 port: Metal M1 (docs/metal-on-panfrost.md), the plug-in Metal loads when
    # B2LMali's IOAcceleratorES names it (userspace/b2l-metal.c): the executable and
    # Info.plist of /System/Library/Extensions/B2LMetal-<hash>.bundle, the hash over both.
    # M2: it opens libb2l-mesa (B2L_MESA_LIBRARY) and takes its b2l_mtl_* table
    # (scripts/mesa/b2l-target/b2l_mtl.h); blocks for the completion handlers.
    metal_out = a.output.parent / 'B2LMetal'
    subprocess.run(['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64e-apple-ios26.0', '-isysroot', sdk,
                    *defines,
                    '-O2', '-Wall', '-Wextra', '-Werror', '-dynamiclib', '-fno-ptrauth-returns',
                    '-fno-ptrauth-calls', '-fno-ptrauth-intrinsics', '-fblocks',
                    '-I', str(ROOT / 'xnu-overlay/iokit/Kernel/arm'),
                    '-I', str(ROOT / 'scripts/mesa/b2l-target'),
                    '-framework', 'CoreFoundation', '-framework', 'IOKit', '-framework', 'IOSurface', '-lobjc',
                    '-install_name', '@rpath/B2LMetal',
                    str(ROOT / 'userspace/b2l-metal.c'), '-o', str(metal_out)], check=True)
    subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(metal_out)], check=True)
    metal = metal_out.read_bytes()
    if len(metal) > LIMIT:
        raise SystemExit('Metal plug-in exceeds the publication limit')
    metal_plist = plistlib.dumps({
        'CFBundleExecutable': 'B2LMetal',
        'CFBundleIdentifier': 'dev.b2l.metal',
        'CFBundleInfoDictionaryVersion': '6.0',
        'CFBundleName': 'B2LMetal',
        'CFBundlePackageType': 'BNDL',
        'CFBundleSupportedPlatforms': ['iPhoneOS'],
        'CFBundleVersion': '1',
        'NSPrincipalClass': 'B2LMetalDevice',
    }, fmt=plistlib.FMT_XML)
    metal_tag = hashlib.sha256(metal + metal_plist).hexdigest()[:12]
    metal_rows = [','.join('0x%02x' % b for b in metal[i:i + 20]) for i in range(0, len(metal), 20)]
    plist_rows = [','.join('0x%02x' % b for b in metal_plist[i:i + 20]) for i in range(0, len(metal_plist), 20)]
    header = a.xnu / 'bsd/kern/b2l_gpu_payload.h'
    header.write_text(
        '/* Generated by scripts/build-gpu-library.py; not repository content. */\n'
        '#define B2L_HAS_GPU_UI_PAYLOAD 1\n'
        f'#define B2L_GPU_UI_LIBRARY "/usr/local/lib/b2l-gpu-{tag}.dylib"\n'
        f'#define B2L_GPU_UI_STAGING "/usr/local/lib/.b2l-gpu-{tag}"\n'
        'static const unsigned char b2l_gpu_library[] = {\n' + ',\n'.join(rows) + '\n};\n'
        '#define B2L_HAS_KBD_PAYLOAD 1\n'
        f'#define B2L_KBD_LIBRARY "/usr/lib/b2l-kbd-{kbd_tag}.dylib"\n'
        f'#define B2L_KBD_STAGING "/usr/lib/.b2l-kbd-{kbd_tag}"\n'
        'static const unsigned char b2l_kbd_library[] = {\n' + ',\n'.join(kbd_rows) + '\n};\n'
        '#define B2L_HAS_METAL_PAYLOAD 1\n'
        f'#define B2L_METAL_PLUGIN_NAME "B2LMetal-{metal_tag}"\n'
        f'#define B2L_METAL_BUNDLE_DIR "/System/Library/Extensions/B2LMetal-{metal_tag}.bundle"\n'
        'static const unsigned char b2l_metal_executable[] = {\n' + ',\n'.join(metal_rows) + '\n};\n'
        'static const unsigned char b2l_metal_plist[] = {\n' + ',\n'.join(plist_rows) + '\n};\n')
    print(f'{len(data)} bytes, library {tag}, sha256 {hashlib.sha256(data).hexdigest()}')
    print(f'{len(kbd)} bytes, keyboard library {kbd_tag}, sha256 {hashlib.sha256(kbd).hexdigest()}')
    print(f'{len(metal)} bytes, Metal plug-in B2LMetal-{metal_tag}, sha256 {hashlib.sha256(metal).hexdigest()}')


if __name__ == '__main__':
    main()
