#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause
# Compile the actual offline precompiler under ASan/UBSan and run its fixtures.
# Optional argument: a real dyld_shared_cache_arm64e main file for a report-only pass.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
cc="${CC:-clang}"
# The converter must agree with the kernel's shared core before any file rewrite.
tmp="$(mktemp -d /private/tmp/b2l-precompile.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT
"$cc" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$repo/common/cache" \
  "$repo/tests/native_cache_test.c" "$repo/common/cache/b2l_cache_core.c" "$repo/common/cache/CacheSha.c" -o "$tmp/core"
"$tmp/core"
CC="$cc" python3 "$repo/tests/test_precompile.py" "$@"
python3 "$repo/tests/test_pacga_keyless.py"
echo 'ARM64e precompiler fixtures passed'
