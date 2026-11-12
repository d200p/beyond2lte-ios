# Build 0136: ACPM rate requests, blend flags

Status: booted once. The GPU runs at 433 MHz. Contains everything in
[build 0135](build-0135.md).

- The rate read logs whether each domain replied, with the raw reply and the
  queue indices before and after.
- The GPU clock request no longer depends on that read, because Android does not
  use it. The boot level falls back to 156 MHz.
- The translator reads only bit 0 of the blend flags. Inside the software
  renderer only `draw_elements` reads that byte, and only that bit; the others
  serve the Metal path. Those draws now reach the shadow comparison.

## Reading the log

```
B2L ACPM: dvfs channel len=15 elem=16 queue tx a/b rx c/d
B2L ACPM: g3d reply=1 ... kHz (...)
B2L ACPM: dvfs queue tx a/b rx c/d
B2L ACPM: first change, 156000 to 433000 kHz
B2L ACPM: change took ... ns
B2L GPU: clock ... kHz (...)
```

| Sign | Meaning |
| --- | --- |
| `reply=0` everywhere, tx rear does not move | The firmware is not reading channel 0. |
| `reply=0` everywhere, tx rear moves | It reads and does not answer this request. |
| `first change`, then the log ends | The rate change stalled: take the rollback. |
| `set ... got no reply` | ACPM is left alone for the rest of the boot. |

## Device result

```
B2L ACPM: temp big=55 mid=52 little=52 g3d=50 isp=51 guard=1
B2L ACPM: g3d reply=1 0 kHz (b0000 0 0)
B2L ACPM: dvfs queue tx 11/11 rx 11/11
B2L ACPM: first change, 156000 to 433000 kHz
B2L ACPM: change took 185693 ns
B2L GPU: clock 432249 kHz (4346699 cycles in 10056000 ns)
B2L ACPM: g3d 55 C big 67 C rate 433000 kHz clean_ms 0
```

- Channel 0 answers every domain. A rate read always returns 0, so it says
  nothing; the request itself works, as the cycle counter shows.
- Live throughput rose by 1.3 to 1.5 times for 2.8 times the clock, which points
  at memory bandwidth. The memory clock is still whatever the boot chain left.
- 101 live draws, 0 failures, no GPU fault. 30 shadow draws of one image key
  failed to run and fell back to the CPU; the cause is not known.
- Settings > General > About froze. That is not new: see
  [build 0137](build-0137.md).
