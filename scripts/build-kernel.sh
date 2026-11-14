#!/usr/bin/env bash
# Build the standalone kernel target. Apple kext KPI bundles are not a B2L target.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
xnu_path="${XNU_PATH:-$repo_root/xnu}"
build_root="${BUILD_DIR:-$repo_root/out/xnu}"
: "${KDKROOT:?Set KDKROOT to the merged KDK and KDK_SDK payload root}"
: "${B2L_DEPSROOT:?Set B2L_DEPSROOT to the isolated dependency installation root}"
export B2L_DEPSROOT
[[ "$(git -C "$xnu_path" rev-parse HEAD)" == ac9718fb1af618d5ce8678d0dc6e8a58f252216f ]] || {
  echo 'Unexpected XNU source revision.' >&2; exit 1;
}
[[ -f "$xnu_path/pexpert/pexpert/arm64/B2L.h" ]] || {
  echo 'Apply the B2L patch and overlay before building.' >&2; exit 1;
}
python3 "$repo_root/scripts/stage-libpthread.py" "$xnu_path"
python3 "$repo_root/scripts/stage-bearssl.py" "$xnu_path"
python3 "$repo_root/scripts/stage-hfs.py" "$xnu_path"
cd "$xnu_path"
# S10 port: B2L_CCACHE=1 compiles through ccache (CI keeps its directory between
# runs); without it, or without ccache, the build is exactly as before.
compiler_args=()
if [[ "${B2L_CCACHE:-0}" == 1 ]] && command -v ccache >/dev/null; then
  # MIG runs $(CC) as a single path, so CC must not contain a space: small
  # wrapper scripts call ccache with the real compilers.
  wrap_dir="$build_root/ccache-bin"
  mkdir -p "$wrap_dir"
  for tool in clang clang++; do
    printf '#!/bin/sh\nexec ccache %s "$@"\n' "$(xcrun -sdk macosx -find "$tool")" > "$wrap_dir/$tool"
    chmod +x "$wrap_dir/$tool"
  done
  compiler_args=(CC="$wrap_dir/clang" CXX="$wrap_dir/clang++")
  ccache --zero-stats >/dev/null || true
  echo "Compiling through ccache ($(ccache --version | head -1))"
fi
exec make SDKROOT=macosx KDKROOT="$KDKROOT" ${compiler_args[@]+"${compiler_args[@]}"} \
  TARGET_CONFIGS='DEVELOPMENT ARM64 B2L' \
  PLATFORM=iPhoneOS PLATFORM_FOR_CONFIG=iPhoneOS SDKVERSION=26.5 BTI_BUILD=0 \
  RC_DARWIN_KERNEL_VERSION=25.5.0 BUILD_LTO=0 BUILD_WERROR=0 \
  MEMORY_SIZE="${BUILD_MEMORY_BYTES:-17179869184}" \
  SYSCTL_HW_PHYSICALCPU="${BUILD_JOBS:-4}" SYSCTL_HW_LOGICALCPU="${BUILD_JOBS:-4}" \
  KERNEL_BUILDS_IN_PARALLEL=1 MAKEJOBS="-j${BUILD_JOBS:-4}" \
  OBJROOT="$build_root/obj" SYMROOT="$build_root/sym" DSTROOT="$build_root/dst" \
  build "$@"
