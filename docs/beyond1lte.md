# Galaxy S10 (beyond1lte) board support

Status: **diagnostic profile boots on a real S10 (2026-11-09).** The firmware and
kernel came from CI run 42695735637 on commit `9a3071b`, and the BOOT image sha256 is `d931b528…fe4ee`. The
handset is an SM-G973F/DS with bootloader `G973FXXUHHXJ1` (binary 17). The iOS
userspace profile has not been tried.

## First hardware boot (diagnostic profile)

Observed on the panel (photos kept privately by the owner):

1. Binary-17 S-Boot accepted the BOOT image. BootShim, the generated `beyond1ltePkg` UEFI and XnuLoader handed off to XNU.
2. SMP: CPU1–7 reached secondary stages 17/18; `timer and remote xcall verified`
   on CPU0–7; Edge scheduler psets for clusters 0/1/2 (M4 > A75 > A55);
   `B2L CPU: 8/8 CPUs initialized, per-CPU timer and bidirectional SGI1 verified`.
3. BSD/IOKit init: Skywalk self-tests passed; HFS `hfs-715.120.4` registered.
4. Native UFS on the S10: contiguous DMA verified, LUN 0 `capacity=31236096`
   blocks of 4096 bytes (128 GB), `/dev/b2lufs0` ready **read-only**, primary+backup GPT CRC verified (31
   partitions), `system` p25 recognised as ext4-family read-only.
5. As the diagnostic profile requires (`rd=b2l-diagnostic`, `b2l_root_rw=0`,
   `b2l_system_log=0`), no root device of that name exists. IOKit waits for it, then panics with
   `Failed to mount root device @IOKitBSDInit.cpp:963` and the panic path ends
   in `spinning forever`. This is the expected end of a diagnostic boot, not a fault.

No partition other than BOOT was written. Touch and buttons were not exercised:
the diagnostic profile starts no userspace.

## Why the S10 can reuse the S10+ port

The S10 (SM-G973F, `beyond1lte`) and S10+ (SM-G975F, `beyond2lte`) are the same
Exynos 9820 board family. For everything this port drives, the stock Samsung
device trees (LineageOS `android_kernel_samsung_exynos9820`, `arch/arm64/boot/dts/samsung`) are identical:

| Block | Both boards |
| --- | --- |
| Interrupts | GICv2, GICD `0x10101000`, GICC `0x10102000` |
| Touch | `sec,sec_ts` (Y771) on HSI2C10 at `0x48`, IRQ `gpa1-0`, `tsp_io` LDO19 / `tsp_avdd` LDO27 (shared `exynos9820-beyondlte_input_common.dtsi`) |
| Keys | power `gpa2-4`, volume up `gpa0-3`, volume down `gpa0-4`, Bixby `gpa0-6` (same shared file) |
| IF-PMIC | MAX77705 on HSI2C45 at `0x66`, IRQ `gpa1-6` |
| Wi-Fi wiring | `WLAN_EN` `gpg0-5`, host wake `gpa0-7`, PCIe0 |
| Panel | S6E3HA9, 1440x3040; the firmware framebuffer is `0xCA000000`, 20 MiB |

The common DTSI files differ only in cameras, battery thermistor tables, grip
sensor tuning, sensor-hub type and the touch firmware *file name*. The touch firmware is stored on the controller and is not loaded by this port.

## Memory: one extra reservation on the S10

A live `/sys/firmware/fdt` from an SM-G973F/DS in TWRP 3.7.0_9-1 (bootloader
`G973FXXUHHXJ1`, 2026-11-09) has the **same 6 DRAM banks and the same 28
reservations** as the S10+ capture (`common/memory/beyond2lte-8g-dt.json`, taken on bootloader `G975FXXSGHWC1`), **plus one**:

```
/reserved-memory/mm_debug  compatible = "exynos,mm_debug"  reg = 0x91200000 + 0x1000
```

Mu-Silicium's reference `beyond1lte.dtb` lacks it too, so it most likely comes
from the newer bootloader. Under the original allowlist that page lay inside
`{0x87200000, 0xee00000}` and could have been handed to XNU. Handling:

* `common/memory/beyond1lte-8g-dt.json` is the **live S10 capture**, with a `_source` note.
  `prepare-boot-firmware.py --device beyond1lte` turns every fixed reservation of it,
  `mm_debug` included, into a reserved firmware memory-map entry.
* `common/memory/b2l_ram_ranges.h` and its identical kernel copy split that range
  into `{0x87200000, 0xa000000}` and `{0x91204000, 0x4dfc000}`, excluding the
  16 KiB page for both boards. The S10+ loses 16 KiB that it never reserved.
  This is the only change shared with the S10+, and one kernel still serves both boards.
* `scripts/ci/firmware.sh` stops an S10 build if any shared bank or reservation
  differs from the S10+ basis, or if any fixed reservation of either board overlaps the allowlist.

The loader still publishes the internal names `samsung,beyond2lte`,
`samsung,beyond2lte-display` and `board-config=beyond2lte`. These are a contract
between XnuLoader and the kernel's IOKit matching, not hardware detection, so
they are deliberately left alone.

## What is S10-specific

| File | Purpose |
| --- | --- |
| `scripts/make-beyond1lte-overlay.py` | In the disposable Mu-Silicium tree, copies the applied S10+ `beyond2ltePkg` to `beyond1ltePkg`, renames it, keeps Mu-Silicium's S10 platform GUID and DTB, and sets the SMBIOS model to Galaxy S10. Refuses a DTB whose model is not BEYOND1LTE. |
| `scripts/fdt-memory-evidence.py` | Reads a binary DTB or evidence JSON, writes evidence, compares banks/reservations (`--allow-extra`) and checks the RAM allowlist (`--allowlist`). |
| `common/memory/beyond1lte-8g-dt.json` | Live S10 evidence (see above). |
| `B2L_DEVICE` / `--device` | Selects `beyond1lte` in `build-boot-firmware.sh`, `prepare-boot-firmware.py`, `package-boot.py`, `ci/firmware.sh` and the workflow's `device` input. The default stays `beyond2lte`. |
| Recovery readers | `pull-b2l-log.py`, `pull-b2l-unified-logs.py` and `probe-b2l-wifi.py` accept either codename and record the real one. They still never write. `pull-b2l-unified-logs.py` had the original handset's ADB serial and USERDATA node hard-coded: it now takes `--serial` and `--userdata` (`/dev/block/sdXN` or `/dev/block/by-name/userdata`), with the old values as defaults. |
| `tests/test_beyond1lte_board.py` | Overlay generation, DTB model guard, evidence parser and live-evidence invariants, the allowlist-overlap rule and the S10 log-exporter identity. |

## Observed handset (2026-11-09, read only)

| Item | Value |
| --- | --- |
| `ro.product.device` / model | `beyond1lte` / SM-G973F (label SM-G973F/DS) |
| Bootloader | `G973FXXUHHXJ1`, **binary 17 (H)** |
| Knox / boot state | `warranty_bit=1`, `verifiedbootstate=orange` (already unlocked) |
| BOOT | `/dev/block/sda14`, 57671680 bytes, equal to the CI `--max-image-bytes` |
| USERDATA | `/dev/block/sda31` (same node as the S10+) |

## Build

Run the workflow with `device = beyond1lte`. The `kernel` job is
device-independent, so either board's kernel run can be packaged with
`component = package`, provided it was built from the same source revision.

Locally, after the Mu-Silicium checkout and the S10+ overlay `rsync`:

```sh
python3 scripts/fdt-memory-evidence.py common/memory/beyond1lte-8g-dt.json --expect-model BEYOND1LTE \
  --compare common/memory/beyond2lte-8g-dt.json --allow-extra --allowlist common/memory/b2l_ram_ranges.h
python3 scripts/make-beyond1lte-overlay.py --mu "$WORKSPACE"
B2L_DEVICE=beyond1lte scripts/build-boot-firmware.sh
python3 scripts/package-boot.py --device beyond1lte --boot-partition-bytes 57671680 --mu "$WORKSPACE" ...
```

## Before each S10 boot

From TWRP, re-dump the live tree and require an exact match with the checked-in evidence:

```sh
adb pull /sys/firmware/fdt s10-live.dtb
python3 scripts/fdt-memory-evidence.py s10-live.dtb --expect-model BEYOND1LTE \
  --compare common/memory/beyond1lte-8g-dt.json --allowlist common/memory/b2l_ram_ranges.h
```

A bootloader update can change the tree. Any mismatch blocks the boot until the
evidence and allowlist are reviewed. Back up BOOT and EFS first.

## Test status (Windows 11, Python 3.14, 2026-11-09)

`test_beyond1lte_board` 10/10 and `test_prepare_boot_firmware` 8/8 (1 skipped: needs `cc`) pass.
`test_package_boot` (6 errors), `test_wifi_diagnostics` (1 failure) and
`test_pull_b2l_unified_logs` (1 failure, 3 errors) fail identically on the
unmodified upstream tree. The causes are Windows-only: reopening a
`NamedTemporaryFile`, POSIX permission bits, and deleting files that are still open. Run them on Linux or macOS for real coverage. The C/XNU-source tests
(`test-ram-ranges.py`, `test-ufs-owned-ram.py`) need clang and an XNU checkout and were not run.

## Open risks

* **Bootloader binary 17:** resolved for the diagnostic path. Mu-Silicium's S10 page
  says binary 16 is required, but binary 17 boots the UEFI image, and XNU runs on all 8 CPUs.
* The touch and MAX77705 drivers are expected to work unchanged. The panel
  coordinate range (4096x4096) and the S10's smaller 6.1-inch panel have not
  been checked on hardware.
* The generated package renames the codename inside comments too. Measurements
  quoted in comments were taken on the S10+.
* Neither upstream repository declares a license.
