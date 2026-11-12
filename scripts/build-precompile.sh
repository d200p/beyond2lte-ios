#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause
# Build the host ARM64e -> ARM64 precompiler into the ignored out/ directory.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
cc="${CC:-clang}"
out="${1:-$repo/out/b2l-precompile}"
mkdir -p "$(dirname "$out")"
"$cc" -O2 -std=c11 -Wall -Wextra -Werror -I"$repo/common/cache" -I"$repo/xnu-overlay/osfmk/arm64" \
  "$repo/tools/b2l-precompile.c" "$repo/common/cache/b2l_cache_core.c" "$repo/common/cache/CacheSha.c" -o "$out"
echo "built $out"
