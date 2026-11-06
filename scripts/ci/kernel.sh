#!/usr/bin/env bash
# Build only the open kernel. This script never connects to a phone.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root"
base="$repo_root/out/ci-env"
export KDKROOT="$base/kdk"
export B2L_DEPSROOT="$base/deps"
export B2L_BUILD_ENV="$base"
mkdir -p "$base" "$B2L_DEPSROOT" out/ci-logs
uname -sm
xcodebuild -version
xcodebuild -showsdks
df -h .
[[ "$(uname -s)" == Darwin && "$(uname -m)" == arm64 ]]
scripts/test-loader.sh

# Exact Apple-signed kit, retrieved from Dortania's public archival mirror.
kit="$base/kdk.dmg"
curl --fail --location --retry 3 --output "$kit" \
  https://github.com/dortania/KdkSupportPkg/releases/download/25F71/Kernel_Debug_Kit_26.5_build_25F71.dmg
printf '90ed319cd1ba6e23d1eefcee89fa1b10743f4cf60b85208ae52aa9f45543c7aa  %s\n' "$kit" | shasum -a 256 -c -
mount_dir="$base/kdk-mount"
mkdir -p "$mount_dir"
hdiutil attach -readonly -nobrowse -mountpoint "$mount_dir" "$kit"
trap 'hdiutil detach "$mount_dir" || true' EXIT
package="$(find "$mount_dir" -maxdepth 2 -name '*.pkg' -print -quit)"
[[ -n "$package" ]]
pkgutil --check-signature "$package"
pkgutil --expand-full "$package" "$base/kdk-expanded"
hdiutil detach "$mount_dir"
trap - EXIT
rm "$kit"
python3 scripts/ci/merge-kdk.py "$base/kdk-expanded" "$KDKROOT"

fetch_source() {
  local name="$1" url="$2" revision="$3"
  mkdir -p "$base/src/$name"
  git -C "$base/src/$name" init -q
  git -C "$base/src/$name" fetch --depth 1 "$url" "$revision"
  git -C "$base/src/$name" checkout --detach FETCH_HEAD
}
fetch_source availability https://github.com/apple-oss-distributions/AvailabilityVersions.git 149b1777f3e8c2133042d8e188f72e3adf6a7110
fetch_source libdispatch https://github.com/apple-oss-distributions/libdispatch.git 701f4d1a24ae9c6863901bbbb22624b7d1b87321
cmake -S "$base/src/availability" -B "$base/availability-obj" -G Ninja \
  -DSRCROOT="$base/src/availability" -DOBJROOT="$base/availability-obj" \
  -DDSTROOT="$B2L_DEPSROOT" -DSYMROOT="$base/availability-sym" \
  -DDRIVERKIT=0 -DCONFIG_EXCLAVEKIT=0 -DCONFIG_EXCLAVECORE=0 \
  -DCONFIG_KERNELKIT=0 -DINSTALL_KERNEL_HEADERS=1 -DAV_VERSION=Local
cmake --build "$base/availability-obj" --target install
scripts/fetch-xnu.sh
B2L_KERNEL_ONLY=1 scripts/prepare-xnu.sh

# Export the real public/private headers before building libfirehose_kernel.
make -C xnu SDKROOT=macosx KDKROOT="$KDKROOT" \
  TARGET_CONFIGS='DEVELOPMENT ARM64 B2L' PLATFORM=iPhoneOS PLATFORM_FOR_CONFIG=iPhoneOS \
  SDKVERSION=26.5 BTI_BUILD=0 BUILD_LTO=0 \
  MEMORY_SIZE=6442450944 SYSCTL_HW_PHYSICALCPU=2 SYSCTL_HW_LOGICALCPU=2 \
  OBJROOT="$repo_root/out/xnu/obj" SYMROOT="$repo_root/out/xnu/sym" \
  DSTROOT="$B2L_DEPSROOT" installhdrs
# libfirehose builds with -nostdinc; supply the public SDK's platform header.
cp "$(xcrun --sdk macosx --show-sdk-path)/usr/include/TargetConditionals.h" \
  "$B2L_DEPSROOT/usr/local/include/kernel/TargetConditionals.h"
cat > "$base/env.sh" <<'ENV'
export KDKROOT B2L_DEPSROOT
ENV
bash scripts/build-firehose.sh
scripts/build-kernel.sh CWARNFLAGS=-w CXXWARNFLAGS=-w
mkdir -p out/ci-kernel
kernel="$(find out/xnu -name mach.development.b2l -type f -print -quit)"
[[ -n "$kernel" ]]
cp "$kernel" out/ci-kernel/mach.development.b2l
scripts/inspect-kernel.sh out/ci-kernel/mach.development.b2l > out/ci-kernel/inspection.txt
shasum -a 256 out/ci-kernel/mach.development.b2l > out/ci-kernel/SHA256SUMS
git rev-parse HEAD > out/ci-kernel/source-revision.txt
