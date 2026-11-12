# Build 0140: clocks follow load, cheaper cache maintenance

Status: booted once. Clocks follow load; Settings still hangs early. Contains
everything in [build 0139](build-0139.md).

## CPU clocks

[0139](build-0139.md) proved the rates and showed an idle phone reaching 80 C with
the clocks left high. Now a thread samples the scheduler's load every 250 ms:

- At 15% of all CPUs, about one core busy, the clusters go to the targets at once.
- Under 8% for 3 s, they return to the boot rates.
- The 80 C hold, 70 C release and lost-sensor rule stay, read every 2 s.

This starts as soon as ACPM opens rather than at 125 s, so late boot gains too.
The busy and task dumps stay at 60 and 120 s. `b2l_cpu_boost=0` keeps the boot
rates.

## Cache maintenance

Live draws spent 54% of their time cleaning caches, one line at a time, over
every row of the target and all of the texture.

- The target is cleaned over the tile bounds of the draw, not whole rows.
- The texture is cleaned over the span its quads sample, plus two texels.
- Work of 512 KiB or more is split into four stripes through `dispatch_apply`.

## Reading the log

```
B2L ACPM: cpu big .. mid .. little .. g3d .. C load ..% high=.. changes=.. holding=..
B2L GPU UI: P draws=.. map_us=.. flush_us=.. submit_us=.. drop_us=..
```

| Sign | Meaning |
| --- | --- |
| `flush_us` per draw near 0139's 819 | The stripes are not helping; check that workers run. |
| `exact` falls for kinds that were clean | The span misses sampled texels. |
| `load 0%` always | The scheduler's figures do not work here; the clocks stay low. |
| `changes` rising fast | The marks are too close, and the clocks flap. |

## Device result

About 235 s, 158 live draws, 0 failures, no GPU fault.

- **Settings.** Launches at 139 s and 197 s were killed in the same place, in
  `__wait_for_IOKit_to_quiesce`. So the About that opened in 0139, at about
  1015 s, opened because it was late. The cause is not fixed. IOKit is quiet at
  60 s and configd's threads are parked in their run loops by 120 s, which leaves
  InterfaceNamer waiting on its own timer. [Build 0141](build-0141.md) has the cause.
- **Heat.** Load was 73 to 100% for the whole run, so the load rule kept the clocks
  up, and the hold fired 14 times. The big cluster goes from 68 to 80 C in seconds
  at 2080 MHz, while mid and little stay near 73 C.
- **Cache maintenance.** Per draw: flush 879 us and drop 344 us, against 819 and
  296 in 0139, for draws 18% larger. Four stripes gave about 10% at best, so
  `dc civac` looks limited by the interconnect, not by the CPU issuing it.
- A 1.45 Mpix blended draw took 2985 us on the GPU against 7611 us on the CPU.
