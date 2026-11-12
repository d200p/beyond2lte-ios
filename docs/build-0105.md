# Build 0105: reproducible procedure for the second unit

This note records the inputs, pinned revisions and scripts for the
images currently prepared for the second SM-G975F (beyond2lteeea, firmware
G975FXXSGHWC1). The design changes are described in [build 0102](build-0102.md)
(precompiled root, kernel mode, stub islands, 0103 trap-path trimming, 0104
keyless PACGA routines, 0105 touch firmware) and
[offline precompilation](arm64e-precompile.md).

Private inputs (IPSW, KDK, extracted images, root and SYSTEM images, backups,
WiFi address, logs) stay under ignored `out/` directories and are never
committed.

## Inputs

| Input | Source |
| --- | --- |
| iPhone17,2 26.6.2 (23G90) IPSW | Apple |
| OS, SystemOS and AppOS images | from the IPSW, d94ap Customer Erase Install identity |
| Kernel Debug Kit 26.5 25F71 | developer.apple.com (Apple ID) |
| TWRP 3.7.0_9-2 beyond2lte | dl.twrp.me, checked against its published checksum |

## Pinned source revisions

| Component | Revision |
| --- | --- |
| XNU | `ac9718fb1af618d5ce8678d0dc6e8a58f252216f` (`scripts/fetch-xnu.sh`) |
| AvailabilityVersions | `149b1777f3e8c2133042d8e188f72e3adf6a7110` |
| libdispatch | `701f4d1a24ae9c6863901bbbb22624b7d1b87321` |
| libpthread, BearSSL, HFS | pinned inside the stage scripts |
| Mu-Silicium | `2147dec93fc293ec85b1814a828816130d6f93b7`, submodules at superproject pins (Mu_Basecore `762a65dad79e2934cddbefa467b7e2c7b5ee4bc0`) |
| beyond2lte-uefi overlay | master `f122c7024c5eeef2c8c78dfd08c62708c9ef823d` |
| beyond2lte-uefi touch pieces | `codex/touch-from-8549bfa` at `c7846ec9257c322eca6eae4cac7d1801786c95f5` |
| Heimdall (download-mode flasher) | `3997d5cc607e6c603c6e7c0d07e42e9868c62af2`, built with `-DDISABLE_FRONTEND=ON -Dlibusb_USE_STATIC_LIBS=NO` |

The first unit's touch commit `48add5bb…` no longer exists on the server.
`scripts/import-touch-firmware.py` imports only the touch driver, its protocol
header (checked against the loader's copy), `beyond2lte.dec`, the HsI2c library,
and removes the driver from APRIORI.

## Procedure

Host: Apple Silicon Mac, Xcode with the iPhoneOS SDK, Homebrew `llvm`, `lld`,
`cmake`, `ninja`, `libusb`, Python 3 with `cryptography`.

Root image, SYSTEM image, flashing and log decoding are manual steps and are
not scripted; see [preparing the root](ios-userspace-root.md),
[native cache](native-cache.md), [persistent console](persistent-console.md)
and [packaging](packaging.md).

```sh
# Kernel sources and build environment
scripts/fetch-xnu.sh
# place the prepared root at out/ipsw/ios26-userdata.hfs.img first:
# prepare-xnu reads launchd.plist from it
scripts/prepare-xnu.sh
scripts/setup-build-env.sh out/build-env-0102 /path/to/KDK_26.5_25F71   # root with KDK and KDK_SDK payloads
source out/build-env-0102/env.sh && scripts/build-kernel.sh CWARNFLAGS=-w CXXWARNFLAGS=-w

# Firmware
scripts/setup-firmware-tree.sh out/firmware
WORKSPACE=$PWD/out/firmware/Mu-Silicium PYTHON_COMMAND=$PWD/out/firmware/venv/bin/python \
  CLANG_BIN=$PWD/out/firmware/llvm-tools/ B2L_ENABLE_TOUCH=1 PATH=$PWD/out/firmware/venv/bin:$PATH \
  scripts/build-boot-firmware.sh

# Root precompilation (mounted root, ownership enabled)
scripts/build-precompile.sh
sudo out/b2l-precompile --write --pacga-trampolines /path/to/mounted/root
sudo out/b2l-precompile --verify --pacga-trampolines /path/to/mounted/root

# Recovery for a newly unlocked phone
python3 scripts/make-vbmeta-disabled.py out/recovery/vbmeta_disabled.img
heimdall flash --RECOVERY twrp.img --VBMETA out/recovery/vbmeta_disabled.img --no-reboot

# BOOT image
python3 scripts/package-boot.py --mu out/firmware/Mu-Silicium \
  --firmware out/firmware/Mu-Silicium/Build/beyond2ltePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd \
  --kernel out/xnu/obj/DEVELOPMENT_ARM64_B2L/mach.development.b2l \
  --loader out/firmware/Mu-Silicium/Build/B2LXnu/RELEASE_CLANGPDB/AARCH64/XnuLoader.efi \
  --output-dir out/0105 --max-image-bytes 67108864 --boot-partition-bytes 57671680 \
  --wifi-mac-file PRIVATE_WIFI_MAC_FILE
```

A root precompiled with `--pacga-trampolines` must be flashed together with a
BOOT image from 0104 or later.

The BOOT image embeds a fresh 256-byte random seed per packaging run and the
phone's private WiFi address, so two packaged images never match byte for byte;
kernel, loader and firmware are the comparable parts.

## Fresh-tree verification

The kernel, loader, firmware and BOOT image were rebuilt from scratch in new
directories with only `scripts/fetch-xnu.sh`, `scripts/prepare-xnu.sh`,
`scripts/setup-build-env.sh`, `scripts/build-kernel.sh`,
`scripts/setup-firmware-tree.sh`, `scripts/build-boot-firmware.sh` and
`scripts/package-boot.py`, then compared with the images built earlier on the
same host:

| Artifact | Result |
| --- | --- |
| `XnuLoader.efi` | byte-identical |
| `B2LTouchDxe.efi` | byte-identical |
| firmware FD | byte-identical |
| kernel | same size and same size for every section except `__os_log`; differs in build date, source paths and UUID |
| BOOT image | packager verification passed |

The kernel embeds its source directory in assertion and log strings and the
build date in its version string; those shift string addresses throughout the
code, so a kernel rebuilt in another directory or at another time never matches
an earlier build byte for byte. Building in the same directory does not make it
identical either, because of the date.

## Known limits of reproducibility

- **Root image.** The installed root was precompiled in four passes while the
  tool evolved (image sections, stub islands, PACGA routines twice).
  Running the current tool once converts the same instructions but can place
  PACGA routines in different empty pages, so a rebuilt root is functionally
  equivalent but not byte-identical to the installed one.
  Copy timestamps and HFS allocation order also differ between runs.
- **SYSTEM image.** Built by hand; HFS creation dates make each build's bytes
  unique. The precompiled kernel mode
  skips the cache, so its contents do not affect booting.
- **Touch firmware.** Taken from a newer tip of the touch branch than the one
  verified on the first unit; device verification on this unit is pending.
