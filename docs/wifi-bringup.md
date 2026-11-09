# Exynos 9820 / BCM4375 Wi-Fi bring-up

Recovery enumerates a Broadcom endpoint at `0000:01:00.0`: vendor/device
`14e4:4475`, subsystem `14e4:4375`, PCI revision `05`. Its current kernel log
records a successful Gen1 link and a Broadcom chip ID of `0x4375`, then an
intentional power-off during DHD initialization. Cached sysfs PCI IDs alone do
not prove that the link remains up. This evidence establishes the transport
and device family, not functioning Wi-Fi under XNU.

## First XNU checkpoint

`b2l_wifi_diag=1` takes one snapshot before launchd of shared PMU, GPIO and
CMU registers. Missing, zero and unsupported values disable the probe before
mapping any register. The diagnostic profile does not select it. Lines have
the `B2L WIFI:` prefix and are retained in the existing persistent console.

This read-only mode completed on the phone in epoch 48, followed by launchd,
frame presentation and user-confirmed menu interaction. PHY isolation bypass
was zero; WLAN_REG_ON, PERST and CLKREQ were inputs with low sampled levels.
All six sampled gate words were `0x200000`, and all five QCH words were
`0x3f0002`. The preceding epoch ended during cache verification before reaching
this checkpoint. Allow at least five minutes before testing menu interaction
and returning to recovery; seeing an image alone is insufficient evidence.

The snapshot maps these four shared blocks with I/O memory attributes and
releases all mappings, including after a partial mapping failure. Mapping
lengths are 16 KiB and bases are aligned, accommodating the current 4 KiB
kernel and 16 KiB page granularity. Reads are aligned 32-bit accesses.

| Block | Base | Sampled offsets / purpose |
| --- | --- | --- |
| PMU | `0x15860000` | `0x71c`, channel-zero PHY isolation bypass bit 0 |
| PERIC0 GPIO | `0x10430000` | GPG0 CON/DAT `0x80/0x84`, WLAN_REG_ON pin 5 |
| FSYS1 GPIO | `0x13c40000` | GPF2 CON/DAT `0x20/0x24`, PERST pin 2 and CLKREQ pin 0 |
| CMU_FSYS1 | `0x13c00000` | PCIe gate words `0x2030..0x2044` and QCH words `0x3034..0x3044`, stride 4 |

Six gate words correspond to DBI, PHY SCL APB, master AXI, controller APB,
PCS APB and slave AXI. Five QCH words correspond to APB, DBI, master, PCS and
PHY. They are printed raw: automatic clock gating makes a single bit
insufficient to prove safe controller access. An all-ones PMU/GPIO register
stops decoding and releases mappings. Mapping success does not itself prove
that a register is readable; physical execution is still required.

No controller ELBI, PHY, PCS, DBI, configuration window or BAR is mapped or
read. Mode 1 writes no GPIO, clock, reset, isolation, ATU, interrupt, SysMMU or
PCI command register. No firmware is loaded and no network interface is
registered. A completed snapshot means only that this limited checkpoint ran.

## Temporary power-path checkpoint

The next userspace loader selects `b2l_wifi_diag=2`. It first takes the same
snapshot, then accepts only the measured idle pin/isolation state and exact
clock words above. Any mismatch skips the active part without writes.

It preloads a low PERST data latch and makes that pin an output, preloads a
low WLAN_REG_ON latch and makes that pin an output, then raises WLAN_REG_ON.
After Samsung's 200 ms turn-on delay it briefly enables channel-zero PHY
isolation bypass and checks the shared-register readback. PERST stays low.
The chip is held in reset throughout the power pulse.

On success or a failed readback it attempts restoration of every owned bit:
PHY isolation, power/reset data, then the two pin function fields. Each
restoration is verified and retried at most three times. A failed restoration
is explicitly logged. Read-modify-write operations preserve all other bits;
an all-ones read is rejected before modification and as a readback result.
The complete active part normally takes about 201 ms and has no polling loop.

This mode changes only the two GPIO mux fields, their data bits and PMU PHY
isolation bit zero. It changes no clocks, regulators, PHY calibration or
controller state, reads no ELBI/DBI/config/BAR, and enables no link, DMA,
firmware or network service. Successful GPIO/register readback is not proof
that the chip booted firmware or established a PCIe link. Physical validation
of this active mode is pending.

## Recovery evidence

With one rooted beyond2lte in TWRP, collect cached PCI IDs, resources, driver
bindings, platform status and the exported WLAN_REG_ON GPIO:

```sh
rtk proxy python3 scripts/probe-b2l-wifi.py --output /existing/private/parent/new-capture
```

Use `--serial` to select a particular recovery if multiple devices are
connected. Output directories are exclusive and mode 0700; files are 0600.
The collector does not mount filesystems, read MAC addresses or saved networks,
read live PCI configuration space, change GPIOs, or invoke `pcie_sysfs` tests.
An absent PCI endpoint may mean the link is off. Recovery's `devmem` executable
cannot provide a baseline here because this recovery has no `/dev/mem`; no
device node was created to bypass that limitation.

Original Wi-Fi station firmware, CLM and nine candidate NVRAM files have been
extracted privately from the verified original vendor backup. They are not
repository or CI inputs. Selecting the correct module/board NVRAM remains
open; no random variant is used.

## Following checkpoints

1. Validate the temporary power-path checkpoint and verified restoration on
   the phone, preserving the validated read-only BOOT as rollback.
2. Port channel-zero pin control, clock, PHY calibration and reset
   sequencing from the Exynos 9820 sources. Confirm a bounded Gen1 link before
   configuration transactions. Keep DMA/bus mastering disabled during identity
   probing and do not replace UFS/shared clock configuration wholesale.
3. Read the endpoint and chip revision, select verified board firmware/NVRAM,
   and bring up firmware with an appropriate DMA/SysMMU design.
4. Establish scanning, authentication, packet transport and DHCP, then adapt
   the HakiOS network and Apple80211 interfaces to this iOS release.

Linux can initialize this hardware, but its driver is not a loadable XNU
driver. The S20 Ultra reference uses Exynos 990; its MMIO/HVC/PHY/DMA settings
cannot be copied to Exynos 9820. HakiOS's chip driver is Qualcomm/ath11k;
only its network and iOS integration are candidates for reuse here.

## Validation

`rtk proxy python3 tests/test_wifi_diagnostics.py` runs the actual diagnostic
control flow under ASan/UBSan, with an MMIO allowlist, unsupported boot modes,
mapping failures at every position, reverse cleanup, pin decoding, an invalid
register and unchanged simulated memory in mode 1. Mode 2 tests preserve
unrelated pin/PMU bits, drop every write in turn, reject all-ones readback,
exercise unexpected baseline states and bound persistent restoration failure.
Recovery tests exercise wrong-device
and ambiguous-device rejection, PCI parsing, private output and no overwrite.
These host checks cannot prove hardware register accessibility or Wi-Fi support.

## Source references

Samsung/LineageOS is pinned to
`a412cfaf86b980ce35cae96a35f678c8ea551575`:

- [beyond2lte configuration](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/arch/arm64/configs/exynos9820-beyond2lte_defconfig)
- [PCIe host driver and sequencing](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/pci/dwc/pcie-exynos-host-v0.c)
- [PCIe register offsets](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/pci/dwc/pcie-exynos-host-v0.h)
- [Exynos 9820 PHY calibration](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/pci/dwc/pci-exynos9820_cal.c)
- [GPIO bank offsets](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/pinctrl/samsung/pinctrl-exynos-arm64.c)
- [Clock register bases, offsets and fields](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/soc/samsung/cal-if/exynos9820/cmucal-sfr.c)
- [WLAN power and PCIe channel setup](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/net/wireless/broadcom/bcmdhd_101_16/dhd_custom_exynos.c)

Reusable integration references:

- [HakiOS XNU packet interface](https://github.com/oosexgang/hakios-xnu-kernel/blob/63fc2b4633bac3af8e57dadab9f5a8d4cb940438/drivers/net/hakios.net.c)
- [HakiOS Wi-Fi service](https://github.com/oosexgang/hakios-xnu-kernel/blob/63fc2b4633bac3af8e57dadab9f5a8d4cb940438/drivers/wifi/hakios.wlan.cpp)
- [BCM4375 on Exynos 990 bring-up report](https://github.com/Bentlybro/z3s-mainline-linux/blob/main/docs/WIFI_BRINGUP.md)
