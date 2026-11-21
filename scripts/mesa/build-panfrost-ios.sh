#!/usr/bin/env bash
# S10 port: first step towards GPU composition through Core Animation's OpenGL
# ES renderer (CA::OGL::GLESContext, which iOS 26 still tries after Metal).
# Cross-compiles Mesa's Panfrost (Mali-G76 is Bifrost v7) for iOS arm64e with
# the same pointer-authentication choices as userspace/b2l-gpu.c, so that the
# errors of each step show what the port needs. The product is
# libb2l-mesa.dylib (scripts/mesa/b2l-target): Panfrost, the GL state tracker
# and our frontend over the B2LMali libdrm (scripts/mesa/b2l-drm), with the
# EAGL layer Core Animation's GLESContext uses (b2l-target/b2l_eagl.c).
#
# Usage: build-panfrost-ios.sh MESA_SOURCE_DIR BUILD_DIR
set -euo pipefail
src=$(cd "$1" && pwd)
build=$2
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
sdk=$(xcrun --sdk iphoneos --show-sdk-path)
cc=$(xcrun --sdk iphoneos -f clang)
cxx=$(xcrun --sdk iphoneos -f clang++)
ar=$(xcrun --sdk iphoneos -f ar)
strip=$(xcrun --sdk iphoneos -f strip)
mkdir -p "$build"
cross="$build/ios-arm64e.txt"
# -mstrict-align: B2LMali maps buffer objects as device memory on the CPU side,
# where every access must be aligned to its size. Boot 48 died on a 64-bit
# store at a 4-byte boundary in panfrost_update_shader_state (two 32-bit
# descriptor words merged by the compiler); the flag keeps every access the
# compiler emits naturally aligned. memcpy/memset come from b2l_memset.c.
# Boot 55: st_draw_quad's `stp s11, s9` (two aligned 32-bit stores merged
# into one pair) wrote across a page boundary of an upload buffer and took
# an alignment fault; the load/store pair pass is off, so every access to
# such memory stays one aligned access that cannot cross a page.
# -D_DARWIN_C_SOURCE (2026-11-16, Xcode 27.1): its SDK has <endian.h>, so Mesa's
# u_endian.h tests BYTE_ORDER, which Darwin's headers hide where a file defines
# _POSIX_C_SOURCE (flex's lexers do): program_lex.yy.c stopped the build with
# "UTIL_ARCH_LITTLE_ENDIAN and/or UTIL_ARCH_BIG_ENDIAN were unset".
flags="'-target', 'arm64e-apple-ios26.0', '-isysroot', '$sdk', '-fno-ptrauth-returns', '-fno-ptrauth-calls', '-fno-ptrauth-intrinsics', '-mstrict-align', '-mllvm', '-aarch64-enable-ldst-opt=false', '-D_DARWIN_C_SOURCE', '-I$repo/scripts/mesa/include'"
cat > "$cross" <<EOF
[binaries]
c = '$cc'
cpp = '$cxx'
objc = '$cc'
ar = '$ar'
strip = '$strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
kernel = 'xnu'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[built-in options]
c_args = [$flags]
cpp_args = [$flags]
objc_args = [$flags]
c_link_args = ['-target', 'arm64e-apple-ios26.0', '-isysroot', '$sdk']
cpp_link_args = ['-target', 'arm64e-apple-ios26.0', '-isysroot', '$sdk']
EOF
# Only Panfrost and the OpenGL ES state tracker; everything that needs a
# window system, LLVM, X11, Wayland or Linux-only libraries stays off. Options
# differ between Mesa releases (25.3 has no gallium-vdpau): only those this
# release declares are passed.
options_file="$src/meson.options"; [[ -f "$options_file" ]] || options_file="$src/meson_options.txt"
args=()
for kv in platforms= egl=disabled glx=disabled gbm=disabled gallium-drivers=panfrost vulkan-drivers= \
    opengl=true gles1=disabled gles2=enabled llvm=disabled shared-llvm=disabled valgrind=disabled \
    libunwind=disabled lmsensors=disabled zstd=disabled xmlconfig=disabled expat=disabled \
    build-tests=false tools= gallium-va=disabled gallium-vdpau=disabled gallium-mediafoundation=disabled \
    gallium-rusticl=false android-libbacktrace=disabled shader-cache=disabled; do
  name=${kv%%=*}
  if grep -q "option('$name'\|option(\"$name\"\|^  '$name'" "$options_file"; then args+=("-D$kv"); else echo "skipping -D$kv (not in this release)"; fi
done
# The libdrm calls Panfrost makes: what the B2LMali-backed libdrm must provide.
grep -rhoE '\bdrm[A-Z][A-Za-z0-9]+\(' "$src/src/panfrost" "$src/src/gallium/drivers/panfrost" \
  "$src/src/gallium/winsys/panfrost" 2>/dev/null | sort | uniq -c | sort -rn > "$build/libdrm-calls.txt" || true
grep -rhoE 'DRM_IOCTL_[A-Z0-9_]+' "$src/src/panfrost" "$src/src/gallium/drivers/panfrost" \
  "$src/src/gallium/winsys/panfrost" 2>/dev/null | sort | uniq -c | sort -rn > "$build/drm-ioctls.txt" || true
echo '--- libdrm calls'; cat "$build/libdrm-calls.txt"; echo '--- DRM ioctls'; cat "$build/drm-ioctls.txt"
# Buffer objects are mapped by the B2LMali libdrm, not by mmap on a DRM fd:
# os_mmap/os_munmap (src/util/os_mman.h) go to b2l_drm_mmap/b2l_drm_munmap,
# which fall back to mmap/munmap for any other descriptor.
python3 "$repo/scripts/mesa/b2l-drm/patch_os_mman.py" "$src/src/util/os_mman.h"
# No IDVS on this GPU (B2L_IDVS=1 brings it back): see the script.
python3 "$repo/scripts/mesa/b2l-drm/patch_pan_shader_idvs.py" "$src/src/gallium/drivers/panfrost/pan_shader.c"
# Tiler jobs wait for every earlier job (B2L_NO_TILER_BARRIER=1 undoes it): see the script.
python3 "$repo/scripts/mesa/b2l-drm/patch_pan_jm_barrier.py" "$src/src/gallium/drivers/panfrost/pan_jm.c"
# A refused IOSurface import returns NULL instead of crashing (boot 58): see the script.
python3 "$repo/scripts/mesa/b2l-drm/patch_pan_bo_import.py" "$src/src/gallium/drivers/panfrost/pan_bo.c"
# The AIR (Metal shader) frontend, HakiOS's (scripts/mesa/air, MIT): it lives
# in src/compiler/air and one subdir() after spirv's reaches it, as HakiOS's
# tools/mesa/air-frontend.patch does (docs/metal-on-panfrost.md).
rm -rf "$src/src/compiler/air"
cp -R "$repo/scripts/mesa/air" "$src/src/compiler/air"
python3 - "$src/src/compiler/meson.build" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
if "subdir('air')" not in s:
    if "\nsubdir('spirv')\n" not in s:
        raise SystemExit("src/compiler/meson.build: no subdir('spirv') line to put subdir('air') after")
    s = s.replace("\nsubdir('spirv')\n", "\nsubdir('spirv')\n\nsubdir('air')\n", 1)
    open(p, 'w').write(s)
PY
# Our target, built by Mesa's own meson like the OSMesa one.
target="$src/src/gallium/targets/b2l"
mkdir -p "$target"
cp "$repo/scripts/mesa/b2l-target/meson.build" "$repo/scripts/mesa/b2l-target/b2l_mesa.c" \
  "$repo/scripts/mesa/b2l-target/b2l_eagl.c" "$repo/scripts/mesa/b2l-target/b2l_memset.c" \
  "$repo/scripts/mesa/b2l-target/b2l_mtl_sweep.c" "$repo/scripts/mesa/b2l-target/b2l_mtl.c" \
  "$repo/scripts/mesa/b2l-target/b2l_mtl.h" \
  "$repo/scripts/mesa/b2l-drm/b2l_drm.c" "$repo/scripts/mesa/b2l-drm/b2l_drm.h" \
  "$repo/xnu-overlay/iokit/Kernel/arm/b2l_mali_uapi.h" "$target/"
# The EAGL layer's GLI dispatch table, from the build machine's macOS SDK
# (Apple's header is read here, never copied into the repository or the
# artifacts); the script stops if the layout is not the one 23G90 uses.
gli_rel=System/Library/Frameworks/OpenGL.framework/Headers/gliDispatch.h
gli_header="$(xcrun --sdk macosx --show-sdk-path)/$gli_rel"
if [[ ! -f "$gli_header" ]]; then
  for sdk_dir in /Applications/Xcode*.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX*.sdk \
      /Library/Developer/CommandLineTools/SDKs/MacOSX*.sdk; do
    if [[ -f "$sdk_dir/$gli_rel" ]]; then gli_header="$sdk_dir/$gli_rel"; break; fi
  done
fi
echo "gli: $gli_header" | tee "$build/gli.log"
python3 "$repo/scripts/mesa/b2l-target/gen-gli-dispatch.py" "$gli_header" "$target/b2l_gli_table.h" \
  2>&1 | tee -a "$build/gli.log"
grep -q "targets/b2l" "$src/src/gallium/meson.build" || printf "\nsubdir('targets/b2l')\n" >> "$src/src/gallium/meson.build"
meson setup "$build/out" "$src" --cross-file "$cross" --buildtype=release \
  --default-library=static "${args[@]}" 2>&1 | tee "$build/configure.log"
ninja -C "$build/out" -k 0 2>&1 | tee "$build/build.log" || true
# The library, stripped of local symbols and ad-hoc signed like b2l-gpu.dylib.
lib="$build/out/src/gallium/targets/b2l/libb2l-mesa.dylib"
mkdir -p "$build/b2l-mesa"
if [[ -f "$lib" ]]; then
  cp "$lib" "$build/b2l-mesa/libb2l-mesa.dylib"
  "$strip" -x "$build/b2l-mesa/libb2l-mesa.dylib"
  codesign --force --sign - "$build/b2l-mesa/libb2l-mesa.dylib"
  { ls -l "$lib" "$build/b2l-mesa/libb2l-mesa.dylib"; xcrun --sdk iphoneos nm -gU "$build/b2l-mesa/libb2l-mesa.dylib" | grep -c ' T ' ;
    xcrun --sdk iphoneos nm -gU "$build/b2l-mesa/libb2l-mesa.dylib" | grep ' _b2l_'; xcrun --sdk iphoneos otool -L "$build/b2l-mesa/libb2l-mesa.dylib";
    gzip -9 -c "$build/b2l-mesa/libb2l-mesa.dylib" | wc -c; shasum -a 256 "$build/b2l-mesa/libb2l-mesa.dylib"; } 2>&1 | tee "$build/b2l-mesa.log"
  # For reading crash backtraces from the phone: the same library before
  # strip, and its full symbol map (with local symbols), sorted by address.
  mkdir -p "$build/b2l-mesa-symbols"
  cp "$lib" "$build/b2l-mesa-symbols/libb2l-mesa-unstripped.dylib"
  xcrun --sdk iphoneos nm -n "$lib" > "$build/b2l-mesa-symbols/libb2l-mesa.nm.txt" || true
  shasum -a 256 "$lib" "$build/b2l-mesa/libb2l-mesa.dylib" > "$build/b2l-mesa-symbols/SHA256SUMS"
else
  echo "b2l-mesa: not built" | tee "$build/b2l-mesa.log"
  mesa_missing=1
fi
# The B2LMali libdrm alone, against this release's DRM uapi headers, with -Werror.
drm_out="$build/b2l-drm"
mkdir -p "$drm_out"
if "$cc" -target arm64e-apple-ios26.0 -isysroot "$sdk" -fno-ptrauth-returns -fno-ptrauth-calls \
    -fno-ptrauth-intrinsics -std=gnu11 -O2 -Wall -Wextra -Werror -I"$repo/scripts/mesa/include" \
    -I"$repo/scripts/mesa/b2l-drm" -I"$src/include" -I"$repo/xnu-overlay/iokit/Kernel/arm" \
    -c "$repo/scripts/mesa/b2l-drm/b2l_drm.c" -o "$drm_out/b2l_drm.o" 2>&1 | tee "$build/b2l-drm.log"; then
  "$ar" rcs "$drm_out/libb2l-drm.a" "$drm_out/b2l_drm.o"
  echo "b2l-drm: built $drm_out/libb2l-drm.a" | tee -a "$build/b2l-drm.log"
else
  echo "b2l-drm: FAILED" | tee -a "$build/b2l-drm.log"
fi
# A run without the library must fail: the kernel build would otherwise embed
# the last good one without saying so (2026-11-16). The logs upload anyway.
if [[ -n "${mesa_missing:-}" ]]; then
  echo "b2l-mesa: build failed, see build.log" >&2
  exit 1
fi
