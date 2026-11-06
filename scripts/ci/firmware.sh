#!/usr/bin/env bash
# Prepare a disposable UEFI tree and build the diagnostic loader on Linux.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo_root"
export WORKSPACE="$repo_root/out/mu-build"
export CLANG_BIN=/usr/lib/llvm-18/bin
export PYTHON_COMMAND="$repo_root/out/firmware-venv/bin/python"
mkdir -p out
git clone --no-checkout --filter=blob:none https://github.com/Project-Silicium/Mu-Silicium.git "$WORKSPACE"
git -C "$WORKSPACE" fetch --depth 1 origin fa64a442a9c98fdad6bd773b2d7e661afef3bb35
git -C "$WORKSPACE" checkout --detach FETCH_HEAD
git -C "$WORKSPACE" submodule update --init --depth 1 Mu_Basecore Common/Mu Common/Mu_OEM_Sample Silicium-ACPI
# Use the exact Basecore revision documented by the device port.
git -C "$WORKSPACE/Mu_Basecore" fetch --depth 1 origin 762a65dad79e2934cddbefa467b7e2c7b5ee4bc0
git -C "$WORKSPACE/Mu_Basecore" checkout --detach FETCH_HEAD
git -C "$WORKSPACE/Mu_Basecore" submodule update --init --recursive --depth 1
git clone --no-checkout --filter=blob:none https://github.com/orhunnx/beyond2lte-uefi.git out/uefi-reference
git -C out/uefi-reference fetch --depth 1 origin c7846ec9257c322eca6eae4cac7d1801786c95f5
git -C out/uefi-reference checkout --detach FETCH_HEAD
rsync -a out/uefi-reference/mu-silicium-overlay/ "$WORKSPACE/"
rsync -a out/uefi-reference/silicium-acpi-overlay/SoCs/ "$WORKSPACE/Silicium-ACPI/SoCs/"
python3 -m venv out/firmware-venv
"$PYTHON_COMMAND" -m pip install 'edk2-pytool-library~=0.23.17' 'edk2-pytool-extensions~=0.31.2' 'antlr4-python3-runtime==4.13.2' regex pefile coloredlogs
export PYTHONPATH="$WORKSPACE/Mu_Basecore/BaseTools/Source/Python${PYTHONPATH:+:$PYTHONPATH}"
make -C "$WORKSPACE/Mu_Basecore/BaseTools" -j2
mkdir -p "$WORKSPACE/Conf"
for name in target tools_def build_rule; do
  cp "$WORKSPACE/Mu_Basecore/BaseTools/Conf/${name}.template" "$WORKSPACE/Conf/${name}.txt"
done
scripts/build-boot-firmware.sh
mkdir -p out/ci-firmware
firmware="$(find "$WORKSPACE/Build" -name '*.fd' -type f -print -quit)"
[[ -n "$firmware" ]]
cp "$firmware" out/ci-firmware/firmware.fd
cp "$WORKSPACE/Build/B2LXnu/RELEASE_CLANGPDB/AARCH64/XnuLoader.efi" out/ci-firmware/XnuLoader.efi
mkdir -p out/ci-firmware/mu/BootShim out/ci-firmware/mu/Resources/{DTBs,Scripts}
cp "$WORKSPACE/BootShim/BootShim.S" out/ci-firmware/mu/BootShim/
cp "$WORKSPACE/Resources/DTBs/beyond2lte.dtb" out/ci-firmware/mu/Resources/DTBs/
cp "$WORKSPACE/Resources/Scripts/mkbootimg.py" out/ci-firmware/mu/Resources/Scripts/
git rev-parse HEAD > out/ci-firmware/source-revision.txt
printf '%s\n' "${B2L_BOOT_PROFILE:-diagnostic}" > out/ci-firmware/boot-profile.txt
