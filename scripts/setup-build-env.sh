#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause
# Create the isolated kernel build environment used since build 0102.
#
#   scripts/setup-build-env.sh ENV_DIR KDKROOT
#
# ENV_DIR receives env.sh, src/ (pinned AvailabilityVersions and libdispatch)
# and deps/ (B2L_DEPSROOT). KDKROOT is an existing Kernel Debug Kit 26.5 build
# 25F71 root containing both its KDK and KDK_SDK payloads; env.sh refers to it.
# Run after scripts/prepare-xnu.sh, because the firehose library needs the
# prepared tree's exported private headers. Nothing outside ENV_DIR, xnu/ and
# out/xnu is written; no device is accessed.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
env_dir="${1:?ENV_DIR}"; kdk="${2:?KDKROOT (Kernel Debug Kit 26.5 build 25F71)}"
kdk="$(cd "$kdk" && pwd)"
[[ -d "$kdk/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders" ]] || {
  echo "$kdk lacks Kernel.framework private headers (KDK_SDK payload)" >&2; exit 1; }
mkdir -p "$env_dir"; env_dir="$(cd "$env_dir" && pwd)"
xnu="${XNU_PATH:-$repo/xnu}"
[[ -f "$xnu/pexpert/pexpert/arm64/B2L.h" ]] || { echo "Run scripts/fetch-xnu.sh and scripts/prepare-xnu.sh first" >&2; exit 1; }
mkdir -p "$env_dir/src" "$env_dir/deps" "$env_dir/logs"

fetch() { # url dir commit
  [[ -d "$2/.git" ]] || git clone --filter=blob:none --no-checkout "$1" "$2"
  git -C "$2" fetch --depth 1 origin "$3"
  git -C "$2" checkout --detach "$3"
  [[ "$(git -C "$2" rev-parse HEAD)" == "$3" ]] || { echo "unexpected revision in $2" >&2; exit 1; }
}
fetch https://github.com/apple-oss-distributions/AvailabilityVersions.git "$env_dir/src/AvailabilityVersions" 149b1777f3e8c2133042d8e188f72e3adf6a7110
fetch https://github.com/apple-oss-distributions/libdispatch.git "$env_dir/src/libdispatch" 701f4d1a24ae9c6863901bbbb22624b7d1b87321

echo "== AvailabilityVersions -> deps"
make -C "$env_dir/src/AvailabilityVersions" install \
  SRCROOT="$env_dir/src/AvailabilityVersions" OBJROOT="$env_dir/availability-obj" \
  SYMROOT="$env_dir/availability-sym" DSTROOT="$env_dir/deps" > "$env_dir/logs/availability-install.log" 2>&1
[[ -x "$env_dir/deps/usr/local/libexec/availability.pl" ]] || { echo "availability.pl not installed" >&2; exit 1; }

cat > "$env_dir/env.sh" <<EOF
# B2L build environment (local, gitignored). Sourced by scripts/build-firehose.sh.
B2L_BUILD_ENV="\$(cd "\$(dirname "\${BASH_SOURCE[0]}")" && pwd)"
export B2L_BUILD_ENV
export KDKROOT="$kdk"
export B2L_DEPSROOT="\$B2L_BUILD_ENV/deps"
export B2L_SRCROOT="\$B2L_BUILD_ENV/src"
EOF

echo "== exported XNU headers for libfirehose_kernel"
# libdispatch's firehose sources include XNU private headers (os/atomic_private.h)
# and kern/debug.h needs the SDK's public TargetConditionals.h.
# The board patch searches B2L_DEPSROOT for availability.pl; it must be exported.
export B2L_DEPSROOT="$env_dir/deps" KDKROOT="$kdk"
make -C "$xnu" SDKROOT=macosx KDKROOT="$kdk" TARGET_CONFIGS='DEVELOPMENT ARM64 B2L' \
  PLATFORM=iPhoneOS PLATFORM_FOR_CONFIG=iPhoneOS SDKVERSION=26.5 BTI_BUILD=0 RC_DARWIN_KERNEL_VERSION=25.5.0 \
  BUILD_LTO=0 BUILD_WERROR=0 MAKEJOBS=-j8 OBJROOT="${BUILD_DIR:-$repo/out/xnu}/obj" SYMROOT="${BUILD_DIR:-$repo/out/xnu}/sym" \
  DSTROOT="$env_dir/deps" installhdrs > "$env_dir/logs/installhdrs.log" 2>&1
mkdir -p "$env_dir/deps/usr/local/include/kernel"
cp "$(xcrun --sdk iphoneos --show-sdk-path)/usr/include/TargetConditionals.h" "$env_dir/deps/usr/local/include/kernel/"
[[ -f "$env_dir/deps/usr/local/include/os/atomic_private.h" ]] || { echo "XNU header export incomplete" >&2; exit 1; }

echo "== libfirehose_kernel"
KDKROOT="$kdk" B2L_BUILD_ENV="$env_dir" bash "$repo/scripts/build-firehose.sh" > "$env_dir/logs/firehose.log" 2>&1
[[ -f "$env_dir/deps/usr/local/lib/kernel/libfirehose_kernel.a" ]] || { echo "firehose build failed; see $env_dir/logs/firehose.log" >&2; exit 1; }
echo "Build environment ready: source $env_dir/env.sh && scripts/build-kernel.sh CWARNFLAGS=-w CXXWARNFLAGS=-w"
