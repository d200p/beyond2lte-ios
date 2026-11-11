#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause
# Create the disposable firmware workspace used since build 0102.
#
#   scripts/setup-firmware-tree.sh FIRMWARE_DIR
#
# FIRMWARE_DIR receives Mu-Silicium (pinned, with submodules at the commits the
# superproject records), beyond2lte-uefi (reference port), a Python venv with
# Mu's requirements, built BaseTools, Conf/ from BaseTools templates and an
# llvm-tools/ directory joining Homebrew LLVM with Homebrew lld-link. The
# reference port's master overlay is applied, then the touch pieces from its
# touch branch (scripts/import-touch-firmware.py). Prints the environment for
# scripts/build-boot-firmware.sh. No device is accessed.
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
dir="${1:?FIRMWARE_DIR}"; mkdir -p "$dir"; dir="$(cd "$dir" && pwd)"
MU_SILICIUM=2147dec93fc293ec85b1814a828816130d6f93b7
MU_BASECORE=762a65dad79e2934cddbefa467b7e2c7b5ee4bc0
UEFI_MASTER=f122c7024c5eeef2c8c78dfd08c62708c9ef823d
UEFI_TOUCH=c7846ec9257c322eca6eae4cac7d1801786c95f5
mu="$dir/Mu-Silicium"; uefi="$dir/beyond2lte-uefi"

if [[ ! -d "$mu/.git" ]]; then git clone https://github.com/Project-Silicium/Mu-Silicium.git "$mu"; fi
git -C "$mu" checkout --detach "$MU_SILICIUM"
git -C "$mu" submodule update --init --recursive
[[ "$(git -C "$mu/Mu_Basecore" rev-parse HEAD)" == "$MU_BASECORE" ]] || { echo "Mu_Basecore is not $MU_BASECORE" >&2; exit 1; }

if [[ ! -d "$uefi/.git" ]]; then git clone https://github.com/orhunnx/beyond2lte-uefi.git "$uefi"; fi
git -C "$uefi" fetch origin master codex/touch-from-8549bfa
git -C "$uefi" checkout --detach "$UEFI_MASTER"
git -C "$uefi" cat-file -e "$UEFI_TOUCH^{commit}" || { echo "touch commit $UEFI_TOUCH unavailable" >&2; exit 1; }

# Same steps as the reference port's yap.sh: overlay, then ACPI SoC sources.
cp -r "$uefi/mu-silicium-overlay/." "$mu/"
cp -r "$uefi/silicium-acpi-overlay/SoCs" "$mu/Silicium-ACPI/"
python3 "$repo/scripts/import-touch-firmware.py" --uefi "$uefi" --commit "$UEFI_TOUCH" --mu "$mu" \
  --loader-header "$repo/uefi/XnuLoader/TouchDiagnostic.h"

[[ -x "$dir/venv/bin/python" ]] || python3 -m venv "$dir/venv"
"$dir/venv/bin/pip" install -q -r "$mu/pip-requirements.txt"
make -C "$mu/Mu_Basecore/BaseTools/Source/C" -j8 > "$dir/basetools.log" 2>&1
mkdir -p "$mu/Conf"
for t in build_rule target tools_def; do
  [[ -f "$mu/Conf/$t.txt" ]] || cp "$mu/Mu_Basecore/BaseTools/Conf/$t.template" "$mu/Conf/$t.txt"
done
# CLANGPDB wants clang, llvm-lib, llvm-rc and lld-link in one directory; Homebrew
# installs lld-link from the separate lld formula.
mkdir -p "$dir/llvm-tools"
for t in clang clang++ llvm-ar llvm-dlltool llvm-lib llvm-objcopy llvm-rc; do ln -sf "/opt/homebrew/opt/llvm/bin/$t" "$dir/llvm-tools/$t"; done
ln -sf "$(command -v lld-link || echo /opt/homebrew/bin/lld-link)" "$dir/llvm-tools/lld-link"
[[ -x "$dir/llvm-tools/lld-link" ]] || { echo "install lld (brew install lld)" >&2; exit 1; }
cat <<EOF
Firmware workspace ready. Build with:
  WORKSPACE=$mu PYTHON_COMMAND=$dir/venv/bin/python CLANG_BIN=$dir/llvm-tools/ \\
  B2L_ENABLE_TOUCH=1 PATH=$dir/venv/bin:\$PATH scripts/build-boot-firmware.sh
EOF
