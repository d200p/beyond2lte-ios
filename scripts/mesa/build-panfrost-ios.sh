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
# window system, LLVM, X11, Wayland or Linux-only libraries stays off.
meson setup "$build/out" "$src" --cross-file "$cross" --buildtype=release \
  --default-library=static \
  -Dplatforms= -Degl=disabled -Dglx=disabled -Dgbm=disabled \
  -Dgallium-drivers=panfrost -Dvulkan-drivers= \
  -Dopengl=true -Dgles1=disabled -Dgles2=enabled \
  -Dllvm=disabled -Dshared-llvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
  -Dlmsensors=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled \
  -Dbuild-tests=false -Dtools= -Dgallium-va=disabled -Dgallium-vdpau=disabled \
  -Dgallium-mediafoundation=disabled -Dgallium-rusticl=false -Dandroid-libbacktrace=disabled \
  2>&1 | tee "$build/configure.log"
ninja -C "$build/out" -k 0 2>&1 | tee "$build/build.log"
