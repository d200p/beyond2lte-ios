# Build 0132: GPU clock through ACPM

Status: BOOT image built; see the result below once booted. Contains everything
in [build 0131](build-0131.md).

## ACPM

`b2l_acpm.h` is a client for the firmware that owns clocks, voltages and sensors.
It finds its channels in the table ACPM leaves in SRAM. A request names a
frequency; ACPM picks the voltage, so nothing here touches one. A reply that
does not arrive turns ACPM off for the boot and changes nothing else; Android's
watchdog reset on that path is not copied.

On the first GPU use the driver logs every domain's rate and the temperatures.
This is the first look at the memory (`mif`) and CPU clocks as ACPM sees them.
CPU and memory clocks are only read.

The GPU runs at 433 MHz while jobs run, `gpu_sustainable_info` in the stock tree,
and returns to the boot level before the cores power down. At 85 C it holds the
boot level until 75 C. If the sensor cannot be read the target is 325 MHz. Boot
argument `b2l_gpu_khz` sets the target and 0 turns ACPM off. The cycle counter
checks the first change: `B2L GPU: clock ... kHz` should now read about 433000.

## Live draws

`kind` 1 Sover and filtered `kind` 1 Copy go live, and the size floor drops to
100,000 pixels. Across [0130](build-0130.md) and 0131, `kind` 3 Sover had 97 of 97
within tolerance. `kind` 1 had 1 of 62 and 1 of 104 just over it, in filtered
pixels off by 3 levels of 255.

```
B2L ACPM: mif ... kHz (and the other domains)
B2L ACPM: temp big=.. mid=.. little=.. g3d=.. isp=.. guard=1
B2L GPU: clock ... kHz
B2L ACPM: g3d .. C big .. C rate .. kHz clean_ms ..
```
