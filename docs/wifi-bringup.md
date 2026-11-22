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

`b2l_wifi_diag=2` first takes the same
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
that the chip booted firmware or established a PCIe link. This mode completed
on the phone in epoch 49: `power probe readback passed` and
`power probe restore verified`, followed by user-confirmed menu interaction.
The subsequent short epoch 50 contains only early boot; decode all retained
epochs to find the actual long test session.

## Controller status checkpoint

`b2l_wifi_diag=3` repeats the guarded
power probe and, only after successful power/reset/isolation readback and the
201 ms delay, reads six channel-zero ELBI registers. ELBI is mapped at
`0x13ed0000`, length 16 KiB, but remains unread if shared-state validation or
the power probe fails. Failure to map it stops before any register sampling.

| ELBI offset | Samsung driver name / purpose |
| --- | --- |
| `0x02c` | APP_LTSSM_ENABLE |
| `0x074` | ELBI_RDLH_LINKUP |
| `0x1d0` | SOFT_CORE_RESET |
| `0x288` | PCS global reset |
| `0x28c` | PHY PCS/PMA reset |
| `0x290` | MAC reset |

An all-ones value stops further controller reads. Power/isolation/pin
restoration runs after either success or a failed ELBI readback. This mode
writes no controller register and accesses no PHY, PCS, DBI, configuration
window, BAR or endpoint. No link training, IRQ, DMA, firmware or network
interface is enabled. Completion establishes only ELBI register accessibility
under the tested conditions. Mode 3 completed on the phone in epoch 51:
LTSSM enable was zero, link status `0xf15400`, and all four sampled reset
registers were one. The power restoration and interactive Home were verified.

## One Gen1 link attempt

`b2l_wifi_diag=4` follows the verified power and ELBI checkpoints. It accepts
only the observed inactive controller/reset
state and an unowned CLKREQ pad. It then uses the Exynos 9820 channel-zero
26 MHz PHY profile and DesignWare root-port link controls for one Gen1 x1
attempt. The first physical trial stopped during `phy-calibration`, before
root DBI setup or link training. PHY power-down, shared-state restoration and
interactive Home were verified. The diagnostic follow-up identified SYSREG
`0x13c11050`: baseline `0x1d02`, vendor write `0x1d0c`, readback `0x1d04`.
It also stopped before training, with verified restoration and interactive
Home. The stop line reports the number of link-status polls: zero means no
training status was sampled, so the initial `last_ELBI=0` is not a measured
link state.

Samsung's channel-zero calibration writes the same profile without verifying
its readback. The corrected trial preserves that write and accepts the exact
observed baseline/write/readback triple above as a board-specific exception.
Normal masked readback verification still applies otherwise; a different
baseline with the same discrepancy, any other missing bit or all-ones value
stops the probe. The exception is logged and does not apply to restoration
or any other register. That trial established a Gen1 x1 link on the phone,
followed by verified PHY shutdown, shared-state restoration and interactive
Home. The root DBI ID was `0xa598144d`; this is not the endpoint identity.
The physical meaning of bit 3 remains unconfirmed.

Additional I/O mappings, each 16 KiB, come from this board's device tree:

| Block | Base | Scope |
| --- | --- | --- |
| PHY | `0x13ef0000` | 48 common and 80 TRSV register words; power controls |
| PCS | `0x13ee0000` | Channel-zero reset, reference-clock and RX-idle controls |
| SYSREG_FSYS1 | `0x13c10000` | PCIe fields at `0x1044` and `0x1050` only |
| Root DBI | `0x13f00000` | Root link width/speed, auxiliary clock and PCI command controls |

The PHY data header records its pinned Samsung source hash. Independent
golden hashes in the host test check both arrays. Recovery's exposed kernel
configuration has no EXYNOS_OTP definition; this trial makes no OTP access.
CLKREQ uses the board's function 2 and pull-up 3, while the caller owns the
PERST output. Shared SYSREG and pin fields are restored with masked writes.
No CMU, ACPM DVFS or regulator setting is changed.

The sequence enables the PHY, pulses the documented resets, programs the
PHY/PCS profile, and configures the root port for one lane and target Gen1.
RX electrical-idle checking is bypassed while accessing DBI, as in Samsung's
accessor. Root bus mastering, SERR and parity responses are disabled; all
PCIe interrupts remain masked. PCI command/control writes put zero in W1C
status fields. Root vendor/device IDs are read for diagnostics, not rewritten;
an uninitialized zero root identity is allowed if control readback works.

Training has a 500 ms monotonic deadline and a second limit of 500 polls,
with 1 ms spacing. Preemption or an individual I/O access can extend observed
elapsed time. Only LTSSM states `0x0d..0x14` with negotiated speed 1 and width
1 count as success. There is one attempt, with no retry or panic on timeout.
No iATU/configuration window, endpoint BAR, SysMMU/DMA map or endpoint command
is touched; no firmware or network service is started.

On every path after initialization begins, the probe attempts to assert PERST,
disable LTSSM, mask interrupts, clear root bus mastering, request and check the
PHY power-down controls, and restore the shared pin/SYSREG and selected ELBI
fields. Then the caller restores isolation and WLAN power. Root DBI and PHY
configuration bytes are retained rather than restored to their S-Boot image;
the intended final condition is reset, isolated and powered down. Failed
quiescence is explicit and bounded, and does not suppress common restoration.
Look for all three results: `Gen1 link up`, `Gen1 quiesce verified`, and
`power probe restore verified`. Interactive Home alone does not prove a link.

## Read-only endpoint identity

`b2l_wifi_diag=5` repeats the verified
Gen1 sequence, then reads only CFG0 words at offsets `0x00`, `0x04`, `0x08`
and `0x2c` for bus 1/device 0/function 0. These contain vendor/device IDs,
command/status, PCI class/revision and subsystem IDs. This checkpoint completed
on the phone: `14e4:4475`, subsystem `14e4:4375`, PCI revision 5, class
`0x028000`, command zero, followed by verified CFG0/PHY/power cleanup and
interactive Home.

The live DT's configuration resource is `0x14ffe000`, length `0x2000`.
Samsung splits it into 4 KiB CFG0 and CFG1 halves and uses outbound viewport
1 for CFG0. Mode 5 maps a 16 KiB envelope at `0x14ffc000`, but accesses only
the four words at envelope offset `0x2000` plus the offsets above. It touches
neither the preceding memory aperture nor CFG1. Other modes do not map it.

After confirmed Gen1 x1 and RX-idle bypass, it verifies a type-1 root bridge
header and disabled root bus mastering, saves the viewport and root bus
numbers, selects outbound ATU1, and accepts only an inactive zero control
word. Unsupported/all-ones or occupied ATU state skips endpoint reads.
It temporarily sets primary/secondary/subordinate buses to 0/1/1, retaining
the latency byte, and programs only ATU1's CFG0 descriptor:

| Field | Value |
| --- | --- |
| CPU base / limit | `0x14ffe000` / `0x14ffefff` |
| PCI target | `0x01000000` (01:00.0) |
| Type / enable | `4` (CFG0) / `0x80000000` |

Descriptor words are written in full while disabled and read back exactly;
old all-ones address/limit contents are data rather than an RMW prerequisite.
All descriptor fields are checked before one enable write, followed by at
most five 9 ms enable checks. After a fixed 100 ms readiness interval, the
four reads share a 100 ms deadline, with no endpoint request retries. Before
each word, the probe checks the link, negotiated Gen1 x1, root command and
ATU enable. A single MMIO access can still fault or exceed the software
deadline; host tests do not prove hardware accessibility.

An invalid/unexpected vendor/device ID, enabled endpoint I/O/memory/master
command bits, invalid word or mismatched subsystem stops further reads.
Success requires `14e4:4475` and subsystem `14e4:4375`; the PCI revision is
logged separately and is not a backplane chip revision or firmware selection.
The probe writes no endpoint register, BAR or command, enables no interrupt,
programs no memory/inbound ATU region, and allocates no DMA map or firmware.

On exit it disables its owned ATU1 region with checked bounded retries and
restores root bus numbers and viewport. The disabled descriptor's programming
bytes are retained. Cleanup failure is explicit, contributes to the Gen1
quiescence failure result, and does not suppress the common PHY/power cleanup.
Require `endpoint identity verified`, `endpoint probe end: stage=complete
cleanup=verified`, `Gen1 quiesce verified` and `power probe restore verified`.

Boot 28 on the S10 (beyond1lte, 2026-11-12) verified mode 5: endpoint
`14e4:4475`, subsystem `14e4:4375`, PCI revision 5, class `0x28000`, with
`endpoint probe end: stage=complete cleanup=verified`, `Gen1 quiesce
verified` and `power probe restore verified`. The S10 then ran its own mode 6
(BAR0 sized and placed at `0x14000000` through outbound ATU2, window moved to
ChipCommon) on every boot up to build 61: chip ID `0x10054375`, BCM4375
revision 5, capabilities `0x58400009`, as on the S10+. On 2026-11-16 the
S10 branch took the S10+ branch's modes 6-10 below in its place (same SoC and
chip; the S10's unassigned BAR0 `0x4` is one of the two states they accept).

## Internal chip identity

`b2l_wifi_diag=6` follows the verified endpoint identity. It temporarily assigns
the first BAR0 aperture and Broadcom's
`PCI_BAR0_WIN` to read three chipcommon words: chip ID at `0x00`, capabilities
at `0x04` and the EROM pointer at `0xfc`. This completed on the phone:
chip ID `0x10054375` (BCM4375, revision 5, package 0, AI interconnect 1),
capabilities `0x58400009`, EROM pointer `0x18111000`, all cleanup verified and
interactive Home. The EROM pointer was not followed in this mode.
There are no chip register writes, RAM mappings, core resets, firmware loads,
interrupts or DMA clients in this checkpoint.

Recovery's cached BAR0 is a 64 KiB, non-prefetchable 64-bit aperture at
`0x14400000`; the separate 4 MiB RAM aperture is not used. Mode 6 maps a
16 KiB envelope at `0x14400000`, but programs outbound ATU0 for only the first
4 KiB, with CPU and PCI base `0x14400000` and limit `0x14400fff`. ATU0 must
initially have zero control. Other active or invalid ATU0 state skips the
probe. Mode 5 still programs no memory translation or endpoint register.

The probe saves endpoint command, BAR0 low/high and vendor window. It accepts
only I/O/memory/master command bits zero, high BAR0 zero, and low BAR0 either
`4` (unassigned 64-bit memory) or `0x14400004` (the measured recovery address).
It does not write a sizing pattern or access BAR2. With memory decoding still
off it programs/checks BAR0 and PCI configuration offset `0x80` to
`0x18000000`, the pinned Broadcom enumeration base. It then enables endpoint
memory decoding only, preserving other command bits and writing zero to
status W1C bits. Endpoint and root bus mastering remain disabled.

The three reads share a 100 ms deadline. Before each, the probe verifies
the memory route, CFG0 prerequisites, Gen1 x1, root command, endpoint command,
BAR0 and vendor window. A bad chip ID or all-ones word stops further reads.
Success requires chip ID `0x4375`; revision/package/interconnect fields are
decoded using Broadcom's `CID_*` masks. The EROM pointer is logged but never
followed. Neither the PCI revision nor the internal chip revision alone
selects the board's NVRAM variant.

Cleanup first disables endpoint memory decoding with checked bounded retries.
Only after that succeeds does it restore the saved window and BAR0 words.
It disables its owned ATU0 and returns viewport selection to CFG0, then mode 5
performs its normal CFG0/bus/viewport cleanup. If link prerequisites fail, no
further endpoint request is issued. Inaccessible or failed restoration is
explicit, contributes to the overall quiescence failure result, and still
permits PERST assertion and common PHY/power shutdown. Descriptor contents
remain programmed while disabled. A successful chip read and successful
cleanup are separate results.

## Bounded EROM capture

`b2l_wifi_diag=7` repeats the verified chipcommon reads and accepts only the
measured ID `0x10054375` and EROM pointer
`0x18111000`. With memory decoding off it switches `PCI_BAR0_WIN` to that
page, verifies it, and re-enables memory decoding only. It uses the same first
4 KiB BAR0 translation, with no additional CPU mapping or descriptor target.
This checkpoint completed on the phone: 133 contiguous words, exact completion
count and terminator, followed by all cleanup checks and interactive Home.
The full console had dropped records elsewhere; the EROM words themselves
were complete and passed host structural validation.

The raw capture has a 100 ms deadline and a limit of 896 aligned words, ending
before EROM control offset `0xe00`. Before every read it checks the memory
route, CFG0/link/root command, endpoint command, BAR0 and current window.
All-ones data or a failed guard stops the capture. The exact `0x0000000f`
terminator is recognized only at an entry boundary: untagged high address,
size and high-size extension words are consumed as data, even if they equal
the terminator. The capture never follows any address descriptor or reads
the EROM control registers.

Each line records the ROM offset and raw word. `EROM capture complete` means
that a terminator was captured within the bounds, not that the component
table has been validated. Host analysis must verify every offset/count and
decode the captured table before using core IDs, revisions or addresses.
Missing data, a deadline, a bound stop or malformed entries must not become
an invented core map. Normal BAR0/window/ATU/PHY/power cleanup still runs, and
its result is independent of capture completion. No core is reset, no chip
register is written, and no RAM, DMA or firmware is used.

Host decoding identified 20 components. Selected validated backplane targets:

| Component | Revision | Register base | First master wrapper |
| --- | --- | --- | --- |
| Chipcommon | 64 | `0x18000000` | `0x18100000` |
| ARM Cortex-R4 | 15 | `0x18002000` | `0x18102000` |
| PCIe2 | 64 | `0x18003000` | `0x18103000` |
| D11 MAC, instance 0 | 82 | `0x18001000` | `0x18101000` |
| D11 MAC, instance 1 | 82 | `0x18004000` | `0x18104000` |

These are chip backplane addresses, not host physical MMIO addresses. R4 slave
port 1 describes apertures at `0x0`/`0x150000` and `0x170000`/`0x190000`
(base/size). Aperture descriptors alone do not establish enabled RAM capacity.

The host-only decoder checks raw offsets/count, identity pairs, master-port
order, slave-port coverage, wrapper count/order/size, 64-bit extensions and
range overflow. It makes no device access:

```sh
rtk proxy python3 scripts/decode-b2l-erom.py /private/path/epoch-console.txt
```

## Read-only core state

`b2l_wifi_diag=8` repeats the capture and
requires 133 words plus eight exact measured CIA/CIB/register-base/wrapper
witnesses for the two fixed targets. This is an additional guard for the
already validated targets, not a replacement for the host table decoder.
Any changed witness/count skips all following component access. This checkpoint
completed on the phone with interactive Home and all cleanup checks verified.

The probe switches the existing BAR0 window, with memory decoding off during
each switch, to PCIe2 wrapper `0x18103000` and R4 wrapper `0x18102000`. It reads
only IOCTRL `0x408`, IOSTATUS `0x500` and RESETCTRL `0x800` in each. A 100 ms
deadline and the existing BAR/link/configuration guards apply to every word.
It never writes a wrapper register, resets a core or changes its clock.

Broadcom's `ai_iscoreup` requires IOCTRL CLOCK_EN set and FGC clear (low two
bits equal 1), with RESETCTRL bit zero clear. Only when R4 satisfies those
checks does the probe select R4 register page `0x18002000` and read capabilities
at `0x04`. Otherwise it logs a skipped capability read. CPUHALT is reported
from IOCTRL bit 5 and is never modified. A successful state snapshot does not
mean the core is ready for firmware or that RAM banks have been sized.
All existing BAR/window/ATU/PHY/power cleanup still runs on success or error.

The measured PCIe2 wrapper has IOCTRL `0x1`, IOSTATUS `0x0`, RESETCTRL `0x0`.
R4 has IOCTRL `0x21`, IOSTATUS `0x0`, RESETCTRL `0x0`: its clock is enabled,
reset is released, and CPUHALT is set. R4 capabilities read `0x351585`.
The low two nibbles declare 5 TCM-A and 8 TCM-B banks, sized in the following
checkpoint. Bank count alone does not establish memory capacity or power state.

## TCM bank inventory

`b2l_wifi_diag=9` repeats all preceding
checks and additionally requires the exact measured capabilities `0x351585`
and an active R4 with CPUHALT set. It never forces that state. Changed EROM,
capabilities, clock/reset or CPUHALT prerequisites skip all selector access.
This checkpoint completed on the phone: all 13 bank records, verified selector
restoration, all parent cleanup checks and user-confirmed interactive Home.
The retained trial log reaches 238 seconds; a five-minute stability interval
was not established by this capture.

Following Broadcom's `si_tcm_size`, the inventory selects each of the 13 banks
through R4 BANKIDX `0x40` and reads BANKINFO `0x44`. The original selector must
be within those 13 banks. Each write is guarded and read back; every access
uses the existing link/BAR/window guards and a 100 ms inventory deadline.
Size is `(BANKINFO[6:0] + 1)` times 1 KiB when bit 9 is set, otherwise 8 KiB.
The result measures the capacity reported by BANKINFO, not bank power state.

BANKIDX is the only chip register written. The probe saves its original value
before modification and restores it on success or failure, with at most three
attempts within a separate 100 ms cleanup budget. If access disappears it
reports failed restoration and still attempts normal parent power cleanup.
It never writes BANKPDA, core clocks/resets, RAM, firmware, DMA or IRQ registers.
All existing BAR/window/ATU/PHY/power cleanup remains active.

The original selector was zero. Banks 0 through 3 and 5 through 12 each report
128 KiB; bank 4 reports 64 KiB. Total reported capacity is **1,638,400 bytes**
(`0x190000`), matching the original Android DHD report at base `0x170000`.
This establishes reported capacity, without testing RAM contents or power state.

## Firmware copy with R4 halted

The userspace loader now selects `b2l_wifi_diag=10`. A private BOOT attachment
provides the original 1,297,036-byte `bcmdhd_sta.bin_b1`. Public CI artifacts
contain no firmware. The loader copies the optional attachment into its owned
arena before the device tree and boot arguments; `top_of_kernel_data` keeps it
out of the free-page allocator. `/chosen/b2l,wifi-firmware` describes that range.
The [BOOT packaging contract](packaging.md) defines the explicit descriptor.

XNU requires the exact length, page-aligned range wholly inside reserved boot
data and pinned SHA-256 before it maps any Wi-Fi registers. It repeats mode 9
and requires every measured BANKINFO word, the exact `0x190000` total, verified
selector restoration, and the preceding EROM/core prerequisites. The physical
outcome of this firmware-copy checkpoint is pending.

The copy uses only the existing first 4 KiB BAR0 route and switches its window
over `[0x170000, 0x2aca8c)`. Before each page it checks R4 CLOCK_EN, FGC, RESET
and CPUHALT through the measured wrapper. A single five-second deadline covers
the transfer. Each bounded 64-byte burst has link/configuration/BAR/window
guards before writes and before readback; every written word must compare
equal to the authenticated source. It checks R4 halt again after the final
readback. A guard, deadline or comparison failure stops further RAM writes.

No CPUHALT release, core reset, reset vector at address zero, NVRAM, RAM-size
token, DMA or IRQ setup is performed. The copied RAM is volatile and is not
restored to its old contents; all normal BAR/window/ATU/PHY/power cleanup still
runs. Copy/readback completion and cleanup are separate results. This step
does not execute firmware or provide networking.

## S10 (beyond1lte): firmware and attachment

The S10's own vendor partition (G973FXXUHHXJ1, read from TWRP with
`mount -o ro,noload`, nothing written) holds the same station firmware the
S10+ branch pins: `bcmdhd_sta.bin_b1`, 1,297,036 bytes, SHA-256
`1676f46c...3c54ad`, so mode 10 needs no S10 variant. The files and the
phone's Wi-Fi address stay in a private folder outside the repository.

`scripts/attach-wifi-firmware.py` turns a CI BOOT image into a private one:
it writes `B2LWIFI1`, the six address octets and the `B2LFW001` descriptor
at trailer + 288, appends the firmware after the Mach-O and checks the
result with `package-boot.py`'s verifier (build 61's image grew from
27,985,800 to 29,282,836 bytes, inside the 32 MiB flash image). The S10's
loader now selects `b2l_wifi_diag=10`; a CI image without the attachment
skips all Wi-Fi MMIO.

NVRAM is not chosen yet. The S10 has two B1 candidates, Semco
`nvram.txt_CS01_semco_b1` (the S10+'s pick) and Murata
`nvram.txt_1rh_es43_b1`; the recovery driver reads the module from the
chip's OTP only when the interface comes up, and its boot log says
"Couldn't find the nvram tag". Firmware execution (the next checkpoint)
needs that answer.

Mode 10 on the S10 (boot 68, build 67): the whole image went to TCM at
`0x170000` and read back verified with the R4 halted, and every cleanup step
verified.

## S10: firmware boot (mode 11)

The S10's loader now selects `b2l_wifi_diag=11`, which boots the copied
firmware the way the Samsung kernel's `bcmdhd_101_16` does (LineageOS
`android_kernel_samsung_exynos9820` at `a412cfaf`: `dhd_pcie.c`
`dhdpcie_bus_write_vars`, `dhdpcie_wrt_rnd`, `dhdpcie_bus_download_state`,
`dhdpcie_readshared`, `dhdpcie_readconsole`; `aiutils.c` `_ai_core_reset`;
`bcmpcie.h` `pciedev_shared_t`; `hnd_cons.h`). The 4375 is not a
multi-backplane chip (`siutils.h` `MULTIBP_CAP`), so no power requests are
needed, and the R4 has one master wrapper (`0x18102000`), so its slave
wrapper (`0x18109000`, EROM `+0x6c`) is not a second reset target (`ai_scan`
fills `wrapba2` from slave wrappers only for cores without master wrappers).

The NVRAM travels in the private attachment after the firmware (`B2LFW002`,
[packaging](packaging.md)); XNU checks its layout and logs its entry count,
`boardtype` and `boardrev`. Because a wedged link could cost the rest of the
boot's results, mode 11 starts a kernel thread that sleeps 360 s, after the
Metal test window, and then runs modes 1 to 10 unchanged. After the verified
download:

1. the NVRAM at `0x300000 - 4 - size`, the token
   `(~(size/4) << 16) | (size/4)` at `0x2ffffc`, the seed record
   `{0xfeedc0de, 128}` 8 bytes below the NVRAM and 128 random bytes below
   that, every word read back, with the R4 checked halted first;
2. backplane address 0: all ones means a secured vector and stops; otherwise
   the image's first word is written there and read back;
3. a 6 s pause, so these lines reach the disk before anything runs;
4. `_ai_core_reset(0, 0)` on `0x18102000`: resetstatus (`+0x804`) idle, reset
   asserted, ioctrl `FGC|CLOCK_EN`, reset released, ioctrl `CLOCK_EN`;
5. up to 100 polls 20 ms apart of `0x2ffffc` until it holds neither the token
   nor zero: the address of `pciedev_shared_t`;
6. one second for the start-up messages, then only reads: the first eight
   shared words (flags, trap, assert expression/file/line, console, msgtrace,
   firmware ID), up to 4 KiB of console text before its write index, one log
   line per firmware line, the assert strings, and the 20 trap words;
7. `_ai_core_reset(CPUHALT, CPUHALT)` and the halted check, then the usual
   cleanup and power-off.

Bus mastering stays off on both sides, so the firmware cannot DMA; no ring,
mailbox, doorbell or IRQ is set up and nothing is sent to it. Any failure
before step 4 leaves the R4 halted; after it, the halt always runs on its own
100 ms budget, and a failed halt is reported as a cleanup failure before the
power-off. `tests/test_wifi_gen1.py` runs the sequence against a model R4 that
publishes a shared area, a console and a trap record, and against a firmware
that never answers, a lost vector write, a secured vector and a stuck
resetstatus.

The first S10 image with mode 11 attaches `nvram.txt_CS01_semco_b1`. The
console's first lines should name the firmware and may show whether that
board file suits this module.

Result (boot 70, build 68): the NVRAM checked (324 entries, `boardtype`
0x088f, `boardrev` 0x1300), the token `0xf6ca0935`, seed and vector read back
(address 0 held `0xb83ef000`), and 16 polls (about 0.3 s) after the release
the shared area appeared at `0x24f8bc`: flags `0x30040007` (shared version
7), no trap or assert, console at `0x2d53a4`, firmware ID `0x1b4772fd`. The
console's 19 lines:
`RTE (SDIO-PCIE-MSGBUF) 18.41.117 (B1 Network/rsdb) on BCM4375 r5 @
37.4/160.0/400.0MHz`, the PCIe message-buffer driver, both radios attached
(wl0 5 GHz, wl1 2.4 GHz), TCAM and reclaim, and `ThreadX v5.6 initialized`;
no complaint about the NVRAM. The R4 halted again and every cleanup step
verified. Next is the host side of the message-buffer protocol (rings, DMA,
interrupts), which this checkpoint never set up.

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
repository or CI inputs. The live original recovery driver resolved the module
through its OTP board-information path and selected `bcmdhd_sta.bin_b1` with
`nvram.txt_CS01_semco_b1`.

Using temporary copies of those exact verified files in recovery, the original
Linux driver successfully opened its interface, completed the ARM shared-memory
handshake and loaded CLM. Firmware reported version `18.41.117`, build date
2022-07-19, FWID `01-1b4772fd`. No scan, AP association or DHCP was requested.
This validates the firmware pairing under Linux. Firmware execution, transport
and networking under XNU remain unimplemented.

## Following checkpoints

1. Validate bounded XNU firmware staging/download and full readback with
   R4 held halted, preserving the validated TCM-inventory BOOT as rollback.
2. Bring up firmware execution and the shared command/ring protocol with an
   appropriate DMA/SysMMU and interrupt design.
3. Establish scanning, authentication, packet transport and DHCP, then adapt
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
Mode 3 additionally checks the exact ELBI read allowlist and requires asserted
reset, power/bypass readback and the delay before every controller read. It
tests all-ones controller words, rejected power/clock state and failed mapping
with cleanup, and permits no controller writes. Recovery tests exercise wrong-device
and ambiguous-device rejection, PCI parsing, private output and no overwrite.
These host checks cannot prove hardware register accessibility or Wi-Fi support.

`rtk proxy python3 tests/test_wifi_gen1.py` runs the actual mode-4 control flow
with strict MMIO, power/reset and DBI RX-idle prerequisites. It checks the
source PHY table hashes, Gen1 success, timeout, preemption/deadline behavior,
unexpected speed, invalid DBI data, inactive-state rejection, every mapping
failure and a dropped write at every position. Permanent LTSSM/PHY cleanup
failures remain explicit while common power restoration is still attempted.
The measured SYSREG readback is replayed with the exact vendor write, including
dropped writes throughout that path and rejection of a different baseline,
another missing bit and invalid readback.
The mode-4 backend rejects bus-master/IRQ enabling, W1C status acknowledgement,
CMU writes, and all ATU/endpoint/config/BAR access. Mode 5 additionally verifies
the exact ATU1 CFG0 mapping and four ordered reads, checks readiness and the
deadline, and exercises all ten mapping failures and every dropped write.
It rejects wrong/all-ones identities, endpoint mastering, link loss, root
command/speed changes, a non-bridge header, occupied/unsupported ATU state and
failed enable. Permanent ATU-disable failure remains explicit while common
power restoration still runs. Prior viewport selection and root bus numbers
are restored, including a previously selected inbound viewport. No endpoint
writes, memory/inbound region programming or BAR access is permitted.

Mode 6 tests the exact first-page memory translation, three ordered chip reads,
64-bit BAR0/window setup, W1C-safe memory-only command and restoration. It
exercises all eleven mapping failures, every dropped write, wrong/all-ones
chip words, deadline expiry, unexpected BAR/window state, occupied ATU0 and
failed enable. Permanent command/window/ATU0 cleanup failures and link loss
remain explicit while common power restoration still runs. The backend rejects
endpoint bus mastering, BAR2/RAM access and every chip-register write.

Mode 7 tests the measured EROM window, ordered ROM offsets below `0xe00`,
the 896-word and deadline bounds, and terminators inside 64-bit address/size
extension data. It covers invalid data, changed chip/pointer prerequisites,
loss of link, missing/final-bound terminators, every dropped write in the new
path, and failed final window/command cleanup with common power restoration.

`rtk proxy python3 tests/test_wifi_erom.py` independently tests host decoding
with synthetic 32/64-bit descriptors, extension words equal to terminators,
duplicate core instances, zero-based memory, malformed/missing captures,
port/wrapper mismatches and overflowing ranges.

Mode 8 additionally tests all eight target witnesses and the exact count,
fixed wrapper read scope, gated/forced-clock/reset R4 capability rejection,
invalid state/capability words, deadline/link loss, every dropped write in the
new path, and failed final cleanup. The backend permits no component writes.

Mode 9 permits only BANKIDX writes on the measured halted R4. Tests cover
all 13 banks, both size-unit encodings, changed capability/EROM witnesses,
running/gated/reset R4, invalid original selector/BANKINFO, deadlines before
and after a selector write, link loss, every lost write, and failed selector
restoration with bounded retries and continued parent power restoration.

Mode 10 tests a complete synthetic image in a backplane RAM model, exact
address bounds, halt checks per page, altered raw bank profile, staging bounds
and authentication failures, lost data/window writes at boundary positions,
readback corruption, clock/reset/halt changes, deadline/link loss, and failed
final cleanup. SHA authentication is mocked in the MMIO fixture; the actual
SHA primitive has separate known-answer cache tests. No vendor image is in CI.
The real EFI BOOT reader also runs against synthetic devices to check explicit
attachment presence, partition bounds, read/allocation failures and cleanup.

## Source references

Samsung/LineageOS is pinned to
`a412cfaf86b980ce35cae96a35f678c8ea551575`:

- [beyond2lte configuration](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/arch/arm64/configs/exynos9820-beyond2lte_defconfig)
- [PCIe host driver and sequencing](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/pci/dwc/pcie-exynos-host-v0.c)
- [PCIe register offsets](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/pci/dwc/pcie-exynos-host-v0.h)
- [Exynos 9820 PHY calibration](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/pci/dwc/pci-exynos9820_cal.c)
- [DesignWare root-port link setup](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/pci/dwc/pcie-designware.c)
- [DesignWare root configuration semantics](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/pci/dwc/pcie-designware-host.c)
- [GPIO bank offsets](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/pinctrl/samsung/pinctrl-exynos-arm64.c)
- [Broadcom BAR0 window definitions](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/net/wireless/broadcom/bcmdhd_101_16/include/pcicfg.h)
- [Broadcom enumeration base](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/net/wireless/broadcom/bcmdhd_101_16/include/hndsoc.h)
- [Chipcommon layout and identity masks](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/net/wireless/broadcom/bcmdhd_101_16/include/sbchipc.h)
- [Broadcom chip attachment and identity decoding](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/net/wireless/broadcom/bcmdhd_101_16/siutils.c)
- [Broadcom PCIe chip-ID reader](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/net/wireless/broadcom/bcmdhd_101_16/dhd_pcie.c)
- [AI EROM format and control-register boundary](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/net/wireless/broadcom/bcmdhd_101_16/include/aidmp.h)
- [Broadcom AI EROM scan](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/net/wireless/broadcom/bcmdhd_101_16/aiutils.c)
- [ARM R4 register layout](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/net/wireless/broadcom/bcmdhd_101_16/include/sbhndarm.h)
- [TCM bank inventory algorithm](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/net/wireless/broadcom/bcmdhd_101_16/siutils.c)
- [R4 bank fields and offsets](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/net/wireless/broadcom/bcmdhd_101_16/include/siutils.h)
- [Module OTP parsing and firmware/NVRAM naming](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/net/wireless/broadcom/bcmdhd_101_16/dhd_custom_cis.c)
- [Clock register bases, offsets and fields](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/soc/samsung/cal-if/exynos9820/cmucal-sfr.c)
- [WLAN power and PCIe channel setup](https://github.com/LineageOS/android_kernel_samsung_exynos9820/blob/a412cfaf86b980ce35cae96a35f678c8ea551575/drivers/net/wireless/broadcom/bcmdhd_101_16/dhd_custom_exynos.c)

Reusable integration references:

- [HakiOS XNU packet interface](https://github.com/oosexgang/hakios-xnu-kernel/blob/63fc2b4633bac3af8e57dadab9f5a8d4cb940438/drivers/net/hakios.net.c)
- [HakiOS Wi-Fi service](https://github.com/oosexgang/hakios-xnu-kernel/blob/63fc2b4633bac3af8e57dadab9f5a8d4cb940438/drivers/wifi/hakios.wlan.cpp)
- [BCM4375 on Exynos 990 bring-up report](https://github.com/Bentlybro/z3s-mainline-linux/blob/main/docs/WIFI_BRINGUP.md)

## S10: the firmware's host-side layout (mode 12)

`b2l_wifi_diag=12` repeats mode 11 and, once the shared area is up, reads
(nothing is written) what Linux DHD reads in `dhdpcie_readshared` before it
sets up a ring: all 32 words of `pciedev_shared_t` with its flags decoded
(boot 70: `0x30040007` = IPC revision 7, `d2h_sync_xorcsum`, `hostrdy`,
`no_oob_dw`; no `dma_index`, so the ring indices live in dongle memory and
need no host DMA), the mailbox data words, `ring_info_t` (ring memory, the
four index arrays, the flow-ring and completion-ring counts), the five
common rings' `ring_mem_t` (id, type, item count and size, base) and their
read/write indices. Then the firmware runs 2 s more on its own, and the
console is read again from where the first read stopped.

The next step after this one is the host side itself: rings in host
memory, bus mastering and the doorbells (mode 13, below; interrupts stay
off there too).

Result (boot 72, build 70): flags `0x30040007` (IPC revision 7,
`d2h_sync_xorcsum`, `hostrdy`, `no_oob_dw`; indices in dongle memory),
225 lfrags, 511 host rx buffers, mailbox data at `0x2f400c`/`0x2f4010`,
`ring_info` at `0x2f3b40` (ring memory `0x2f3b8c`, index arrays
`0x2f3e8c`/`0x2f3f38`/`0x2f3fe4`/`0x2f3ff8`, 40 flow rings, 43 submission
and 5 completion rings), `flags2` `0x40801001`. All five common
`ring_mem_t` are zero and every index is 0: the firmware leaves the rings
to the host and waits; it printed nothing more in the 2 s after.

What the host side (mode 13) has to respect, from Samsung's own device tree
(`pcie0@13ED0000`) and `pcie-exynos-host-v0.c` (LineageOS
`android_kernel_samsung_exynos9820` at `a412cfaf`): `use-sysmmu = "false"`
(the chip's DMA addresses are physical; SysMMU `0x13070000` is left in
bypass), `use-cache-coherency = "false"` (rings and buffers uncached, or
cleaned/invalidated around each use), `use-msi = "false"` (the legacy
interrupt, SPI 0xfd, or polling); `use-ia` only drives a PHY CDR-reset
sequencer. Bus mastering is still off on both sides, so until mode 13 the
chip cannot reach host memory at all.

Boot 73 (build 71) ran mode 12 again with the same result. The shared area
was at `0x24f8bc` again, while the console (`0x2e9768`) and `ring_info`
(`0x2e0dd4`, ring memory `0x2e0e20`) were elsewhere than in boot 72: the
firmware lays out its heap from the host's random seed. Mode 13 therefore
reads every address from the shared area of that boot.

## S10: the host side of the rings (mode 13)

`b2l_wifi_diag=13` repeats mode 12 and then does the host half of DHD's
start for this firmware, in the order `dhd_prot_init` and
`dhd_bus_hostready` use (`dhd_msgbuf.c`, `dhd_pcie.c`, `bcmmsgbuf.h`,
`bcmpcie.h`, `pcie_core.h` in `bcmdhd_101_16` at `a412cfaf`). It goes ahead
only for flags with IPC revision 7, `hostrdy` and `d2h_sync_xorcsum`, and
without `dma_index`, `2byte_indices`, `idma`, `ifrm` or `dar`; otherwise it
logs the flags and stops.

1. Host memory: 96 KiB from `kmem_alloc_contig`, below 4 GiB, zeroed,
   `KMA_PERMANENT` (never freed, so a late DMA can only land in it). It is
   used only if every page is physically contiguous and the extent lies in
   owned RAM (`b2l_ram_contains_range`), as for `b2l_ufs_transport.c`. The
   device tree says no cache coherency, so the CPU cleans the extent before
   any doorbell and cleans and invalidates it before reading what the chip
   wrote.
2. The five common rings at their IPC 7 sizes: control submission 64×40,
   rx post 512×32, control completion 64×24, tx completion 1024×24, rx
   completion 512×40. For each, `ring_mem_t` max items, item size and base
   are written in dongle RAM and read back, and every read/write index is
   set to 0 (4-byte indices in dongle memory).
3. The D2H scratch buffer (address, then length 8) and `host_cap` =
   `0x801407`: version 7, `HOSTCAP_H2D_ENABLE_HOSTRDY`,
   `HOSTCAP_DS_NO_OOB_DW` and `HOSTCAP_UR_FW_NO_TRAP`. It leaves out
   `EXTENDED_TRAP_DATA`, which would need a trap buffer.
4. Bus mastering, root port first (the bridge forwards upstream requests),
   then the endpoint. Memory decoding stays on and the status bits are
   written as zero. The guards of every later access expect it on, so it
   cannot be lost silently. If only the root port takes it, it is switched
   off again at once.
5. Hostready: `0x12345678` to DB1 (`PCIH2D_DB1`, PCIe2 core `0x18003000` +
   `0x144`). The firmware does not set `PCIE_SHARED_DAR`, so DHD uses these
   registers and not the DAR copies. Then 200 ms, the state and the console.
6. Two ioctl-response buffers (8 KiB) and four event buffers (2 KiB) posted
   on the control submission ring the way `dhd_prot_rxbufpost_ctrl` does:
   message type `0x0b`/`0x0d`, epoch from sequence number 254 modulo 253, a
   request id, length and address. Then write index 6 and DB0 (`+0x140`)
   with `0xff000006` (`DHD_WRPTR_UPDATE_H2D_DB_MAGIC`).
7. For 2 s, every 200 ms, the step logs what changed: the firmware's read
   index of the control submissions, the completion write indices,
   `PCIMailBoxInt` (`+0xc30` for core rev 64) and the D2H mailbox data.
   Then it logs every completion with its xorcsum check (all words XOR to
   zero), the scratch words and any buffer the chip wrote, and reads the
   console once more.

No ioctl is sent and no interrupt is unmasked. When the R4 is halted again,
bus mastering goes off on the endpoint and then on the root port, before
the usual power-off. A read index of 6 means the chip read the phone's
memory, and a completion means it wrote there.

`tests/test_wifi_gen1.py` models phone memory as the CPU sees it and as the
chip sees it, with only the cache calls copying between the two. Its
firmware checks everything above at each doorbell, takes the buffers and
writes one completion with a valid xorcsum plus an event. The faults cover
no memory, a page hole, memory outside owned RAM, no `hostrdy` flag, a
firmware that never takes the buffers, and an endpoint that will not
master.

Result (boot 74, build 72): mode 13 worked as written. Host memory was at
pa `0x84740000`, and the rings, indices, scratch and `host_cap` were
written and read back. Bus mastering came on, and the hostready doorbell
was rung (nothing changed in 200 ms). Then the six buffers were posted and
DB0 rung, and within 200 ms the firmware's read index of the control
submissions was 6 of 6: the chip read them from phone memory. It wrote no
completion, no mailbox status and no scratch, which is expected with
nothing asked. Its console printed nothing new. Bus mastering went off
again, and every cleanup was verified.

## S10: one ioctl (mode 14)

`b2l_wifi_diag=14` repeats mode 13. Once the firmware has taken the six
buffers, it sends one ioctl the way `dhd_fillup_ioct_reqst` does:
`MSG_TYPE_IOCTLPTR_REQ` (`0x09`) for `WLC_GET_MAGIC` (command 0), request id
`DHD_IOCTL_REQ_PKTID` (`0xfffe`), transaction id 1, 4 bytes in and 4 out,
with its input in phone memory. Then write index 7, DB0 `0xff000007` and
2 s of polling. The firmware should answer on the control completion ring
with `MSG_TYPE_IOCTLPTR_REQ_ACK` (`0x0a`), then `MSG_TYPE_IOCTL_CMPLT`
(`0x0c`, response length 4, transaction id 1), and write `0x14e46c77`
(`WLC_IOCTL_MAGIC`) in the first ioctl-response buffer. That would be the
chip writing phone memory, the second half of the message-buffer path.
Every completion is logged with its xorcsum check. The test's firmware
checks the request at the doorbell and answers as above. Its faults are
buffers never taken (no ioctl is sent) and an ioctl never answered.

Result (boot 75, build 73): mode 14 worked. The firmware took the ioctl
(read index 7 of 7) and wrote two control completions with valid xorcsums:
`MSG_TYPE_IOCTLPTR_REQ_ACK` (request `0xfffe`, status 0) and
`MSG_TYPE_IOCTL_CMPLT` (request id 2, so the second posted buffer; status
0, response length 4, transaction 1). It also wrote `0x14e46c77`, the WLC
magic, in that buffer. Both items carry the D2H phase bit (`flags 0x80`),
and `PCIMailBoxInt` read `0x1`. The message-buffer path now works both
ways: the chip reads requests from phone memory and writes answers there.

## S10: the first iovars (mode 15)

`b2l_wifi_diag=15` repeats mode 14 and, if the ioctl was answered, asks
what `dhd_sync_with_dongle` asks first. One request goes at a time, as DHD
sends them:

1. It gives the control completion read index back
   (`dhd_prot_upd_read_idx`) and posts two more 8 KiB ioctl-response
   buffers (request ids 7 and 8).
2. It sends `WLC_GET_VAR` (262) for `"ver"` (256 bytes in and out, the name
   as the input) and logs the version string from the completion's buffer.
3. Only if that one was answered, it sends `WLC_GET_VAR` for
   `"cur_etheraddr"` (32 bytes). The address is not logged. It is compared
   with the NVRAM's `macaddr=` and only the verdict (matches or differs) is
   written. For the same reason the buffer dump stops at the first six
   buffers.

The test's firmware checks both requests and answers each with an ack, a
completion and the value. Its faults are the ioctl never answered (no
iovar is sent) and `"ver"` never answered (`"cur_etheraddr"` is not sent).
The test also checks that the MAC never reaches the log.

Result (boot 76, build 74): mode 15 worked. With the completion read index
given back (2) and two more buffers posted, `"ver"` came back with status 0
and 256 bytes: `wl0: Jul 19 2022 10:52:21 version 18.41.117 (B1
Network/rsdb) FWID 01-1b4772fd`. `"cur_etheraddr"` came back with status 0
and 32 bytes, and the address differs from the NVRAM's `macaddr`; neither
address was logged. Each answer used the buffer whose request id the
completion names (2 for the magic, then 8 and 7).

## S10: the attachment's MAC (mode 16)

`b2l_wifi_diag=16` repeats mode 15. If the chip's `cur_etheraddr` is not
the address in the private attachment (`/chosen` `mac-address-wifi0`, which
the loader publishes), it does what Samsung's DHD does with its own copy.
It gives back the completion read index, posts two more 4 KiB
ioctl-response buffers, sends `WLC_SET_VAR` (263) `"cur_etheraddr"` with the
attachment's six bytes after the name (20 bytes), and reads
`"cur_etheraddr"` back. Only verdicts are logged: set (status), and read
back (now the attachment's, or still not). The iovar code is now one
generic `WLC_GET_VAR`/`WLC_SET_VAR` sender. The tests cover the set and its
read-back, a set never answered, a chip that keeps its own address, no
`/chosen` address, an address already right, and no address ever in the log.

Result (boot 77, build 75): mode 16 had nothing to do. The chip's
`cur_etheraddr` already was the attachment's address (so the chip has the
right MAC, and what differs is the NVRAM's default `macaddr`), and no set
was sent.

## S10: the radio up (mode 17)

`b2l_wifi_diag=17` repeats mode 16 and then does what `dhd_preinit_ioctls`
ends with:

1. It posts two more 2 KiB ioctl-response buffers (request ids 11 and 12;
   ids 1 and 2 are not posted again, because the firmware may still hold
   one of them).
2. It sends `WLC_UP` (ioctl 2, a zero int, the plain ioctl form without an
   iovar name) and then `WLC_GET_VAR "isup"`.
3. It polls 2 s more. The firmware's `MSG_TYPE_WL_EVENT` completions are
   logged with only the event type and status from `wl_event_msg_t`, as
   the packet starts with addresses.

No scan or join is asked for. From mode 15 on, the dump of the posted
buffers is off, since an answer can hold the MAC and an event packet starts
with addresses. The test's firmware is now generic: it checks whatever was
posted since its read index, answers in the newest posted buffer (as the
real chip did), and sends one event, whose packet carries an address, after
`WLC_UP`. The mode-17 cases are after a MAC set, with the MAC already right,
and with `WLC_UP` unanswered, and the test checks that no address reaches
the log.

Result (boot 78, build 76): `WLC_UP` answered with status 0, and the
firmware sent one event, type 54 (`WLC_E_IF`, its interface made), 79
bytes. `"isup"` is not an iovar of this firmware: status -23
(`BCME_UNSUPPORTED`), and the buffer holds the name back. The radio is up.

## S10: one passive scan (mode 18)

`b2l_wifi_diag=18` repeats mode 17 and, when `WLC_UP` was answered with
status 0, runs one scan that only listens:

1. It posts ioctl-response buffers 9 to 12 again (consumed by now, or never
   posted) and gives the firmware back the event buffers it has used (the
   interface event's), as `MSG_TYPE_EVENT_BUF_POST`.
2. It reads `"scan_ver"` (which `wl_scan_params` layout the firmware wants;
   logged for the next step, version 1 is sent here), then reads
   `"event_msgs"` and sets it again with `WLC_E_ESCAN_RESULT` (69).
3. It sets `"escan"`: version 1, `WL_SCAN_ACTION_START`, any SSID, the
   broadcast BSSID, any BSS type, the passive scan type (no probe request
   is sent, so nothing is transmitted), the firmware's own channel list,
   110 ms per channel.
4. For up to 8 s it reads every `MSG_TYPE_WL_EVENT` and posts its buffer
   again, as DHD refills event buffers, until the escan's final status.

The log has only counts, channel numbers, bands and signal levels: results,
networks (counted once by address, which is never logged), RSSI range, and
the channels heard. Never a network's name or address. The rings are never
wrapped: once the control submission ring reaches 60 items or the
completion ring 56, no more buffers are posted, and the count of those not
posted is logged. The test's firmware answers `scan_ver` as unsupported,
checks the event mask and every escan field, and sends five results (one
network twice, on 2.4 and 5 GHz) and the final event through the event
buffers it holds, newest first. The cases are with the MAC set and already
right, the escan refused, and a scan that never finishes; the test checks
that no name or address reaches the log.

Result (boot 79, build 77): everything before the scan worked, and the
escan was refused with status -37 (`BCME_VERSION`). `"scan_ver"` answered
version 1, length 6, `scan_ver_major` 2: this firmware wants
`wl_escan_params_v2_t` (`ESCAN_REQ_VERSION_V2`, then `wl_scan_params_v2_t`,
which has its own version 2 and length 76 first and a 32-bit `scan_type`).
`ESCAN_RESULT` was already on in `"event_msgs"`. Mode 18 now sends the
layout `"scan_ver"` asks for, version 2 when its major is 2 (version 1 when
it is not given). The test's firmware answers `scan_ver` as the chip did and
checks every field of both layouts. A new case, `scan_ver` unsupported,
covers version 1.
