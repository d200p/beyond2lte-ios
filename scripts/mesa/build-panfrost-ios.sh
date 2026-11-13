#!/usr/bin/env bash
# S10 port: first step towards GPU composition through Core Animation's OpenGL
# ES renderer (CA::OGL::GLESContext, which iOS 26 still tries after Metal).
# Cross-compiles Mesa's Panfrost (Mali-G76 is Bifrost v7) for iOS arm64e with
# the same pointer-authentication choices as userspace/b2l-gpu.c, so that the
# errors of each step show what the port needs. Nothing here runs on the phone
# yet: no winsys for B2LMali, no EAGL frontend.
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
flags="'-target', 'arm64e-apple-ios26.0', '-isysroot', '$sdk', '-fno-ptrauth-returns', '-fno-ptrauth-calls', '-fno-ptrauth-intrinsics', '-I$repo/scripts/mesa/include'"
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
    gallium-rusticl=false android-libbacktrace=disabled; do
  name=${kv%%=*}
  if grep -q "option('$name'\|option(\"$name\"\|^  '$name'" "$options_file"; then args+=("-D$kv"); else echo "skipping -D$kv (not in this release)"; fi
done
meson setup "$build/out" "$src" --cross-file "$cross" --buildtype=release \
  --default-library=static "${args[@]}" 2>&1 | tee "$build/configure.log"
ninja -C "$build/out" -k 0 2>&1 | tee "$build/build.log"
