# Build 0139: CPU clocks, Settings evidence

Status: booted once. All three CPU clusters reached their targets, and About
opened. Contains everything in [build 0138](build-0138.md).

## Evidence for the Settings hang

At 60 s and 120 s the driver logs IOKit's busy state, every busy service, and the
threads of `configd` and `timed`: state, wait event, `x16` (the trap number) and
user stack.

## CPU clocks

The boot chain leaves the CPUs at about half their rate, and most drawing and all
apps run on them. From 125 s, after the dumps, ACPM sets:

| Cluster | Boot | Target | Maximum |
| --- | --- | --- | --- |
| Cortex-A55 | 1053 MHz | 1586 MHz | 1950 MHz |
| Cortex-A75 | 1222 MHz | 2002 MHz | 2400 MHz |
| Exynos M4 | 1378 MHz | 2080 MHz | 2912 MHz |

The targets ask nothing of the memory clock in the device tree's tables. 2080 MHz
is the big cluster's boot rate on Android and the highest allowed without the
overcurrent driver, which this port does not have. Android sends the big cluster
through HIU, so ACPM may not honour that request; the add-chain measurement that
follows shows what really happened.

Requests go one cluster at a time and reach the log first, so a stall names the
cluster. Every 2 s the hottest of the five sensors is read: at 80 C the CPUs go
back to the boot clocks, and at 70 C they rise again. A lost sensor ends the boost
for the boot. `b2l_cpu_boost=0` skips it.

## GPU time

`P` lines split live draw time into mapping, cache flush, the submit call, cache
drop, and the tiler and fragment jobs. 433 MHz gave 1.3 to 1.5 times the
throughput of 156 MHz, and this says where the rest went.

## Reading the log

```
B2L BUSY: sec=60 root=..
B2L TASK configd: tid=.. state=.. wait=.. x16=.. pc=.. lr=..
B2L ACPM: cl0 to 1586000 kHz
B2L ACPM: cl0 reply=1 in ... ns
B2L CPU CLOCK: cpu=0 ran_on=0 mhz=...
B2L ACPM: cpu big .. mid .. little .. g3d .. C holding=0
B2L GPU UI: P draws=.. map_us=.. flush_us=.. submit_us=.. drop_us=.. tiler_us=.. fragment_us=..
```

| Sign | Meaning |
| --- | --- |
| `clN to`, then the log ends | That cluster's change stalled: take the rollback, 0138. |
| `CPU CLOCK` unchanged for a cluster | ACPM ignored the request. |
| `held at boot clocks` often | The targets are too high for sustained use. |

## Device result

```
B2L BUSY: sec=60 root=0
B2L ACPM: cl0 reply=1 in 155846 ns
B2L ACPM: cl1 reply=1 in 169731 ns
B2L ACPM: cl2 reply=1 in 190539 ns
B2L CPU CLOCK: cpu=0 ran_on=0 mhz=1585
B2L CPU CLOCK: cpu=4 ran_on=4 mhz=2001
B2L CPU CLOCK: cpu=6 ran_on=6 mhz=2079
```

- **CPU clocks.** Measured at 1585, 2001 and 2079 MHz. ACPM drives the big cluster
  directly, so HIU was not needed. A 4.2 Mpix blended draw fell from about 70 ms
  on the CPU in 0136 to about 42 ms.
- **Heat.** The clocks stay up when the phone is idle, and the big cluster reached
  80 C with almost nothing on screen. The hold fired 11 times in about 900 s, at 80
  to 82 C, releasing at 67 to 70 C. The guard held, but boosting an idle phone is
  wasteful: the clocks should follow load.
- **Settings.** About opened with no `0x8BADF00D`, and configd's threads were
  parked normally at 120 s. IOKit was already quiet at 60 s. This is not yet proof
  of a fix: Settings was launched about 1015 s after boot, while the 0138 kills
  came from launches at about 220 and 320 s. It has to be repeated early in a boot.
- **Live draw time.** 225 sampled draws averaged 2019 us: map 199, flush 819,
  submit 705 (tiler 182, fragment 481) and drop 296. Cache maintenance on the CPU
  is 54% of the time, and the GPU's own work is 33%.
- 225 live draws, 0 failures, no GPU fault. About 1040 s without a stall.
