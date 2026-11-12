#!/usr/bin/env bash
# Cross-compile the recovery Mali probe (static AArch64 Linux, no libc).
# Run it with: adb push <out> /tmp/ && adb shell /tmp/b2l-mali-probe [w h [runs]]
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-$repo_root/out/b2l-mali-probe}"
clang="${CLANG:-/opt/homebrew/opt/llvm/bin/clang}"
"$clang" --target=aarch64-linux-gnu -O2 -Wall -Wextra -ffreestanding -fno-stack-protector \
  -fno-builtin -nostdlib -static --ld-path="${LLD:-/opt/homebrew/bin/ld.lld}" -Wl,--gc-sections \
  -I"$repo_root/xnu-overlay/iokit/Kernel/arm" \
  -o "$out" "$repo_root/tools/b2l-mali-probe.c"
echo "$out"
