# Build 0145: memory clock test, big-cluster curve, masks live

Status: booted once. The memory clock was already high. Contains everything in
[build 0144](build-0144.md).

## Where the frame time goes

0144's `B2L FB PERF` put the display copy at 7.8 ms per full swap: about 33 MB
read and 6.5 MB written in that time, or 5 GB/s, far under what LPDDR4X gives.
Live draws spend most of their time in cache maintenance, which is memory-bound
too. So before anything else this build measures the memory clock's effect: a
kernel copy before and after asking ACPM for 2093 MHz, Android's boot rate.
`b2l_mif_khz` sets the request and 0 skips it. A rate read returns 0, so only the
two bandwidth figures can say whether the request took.

The `W` line gains `busy_us`, the render thread's CPU time in the window. Draws
against that, not wall time, is the share worth moving.

## Big cluster

The two-rate hold becomes a curve: 2080 MHz below 76 C, then 1820, 1560 and the
1378 MHz boot rate at 76, 80 and 84 C. Each step back up waits for 3 C below the
mark that took it down. The 88 C full hold stays.

## Masks live

Image format 9, text and shape masks through the tinted shader, may draw live:
65 of 66 compared within tolerance in 0144, with 2.2 ms against 11.9 ms on the CPU
for the large ones. Tinted colour draws may too; both still need one clean
comparison per key and no wrong pixel in the window.

## Not in this build

- Batching draws into one pass needs hooks where Core Animation changes or reads
  the destination; the next build studies those.
- Interrupt-driven job waits: this port polls everywhere and has no interrupt
  plumbing for the GPU yet.
- Deeper CPU idle states and the root's `sys_icache_invalidate` wait for the
  owner's say: one can hang the phone, the other changes the root image.

| Sign | Meaning |
| --- | --- |
| `copy N MB/s after` well above `before` | The memory clock was low; the copy and flush costs should fall. |
| Both figures alike, `reply=1` | Memory already ran at that rate, or ACPM ignored it. |
| `mif reply=1`, then the log ends | The retiming stalled: take the rollback, 0144. |
| Text looks wrong or missing | Masks go back to shadow. |

## Device result

```
B2L ACPM: copy 20681 MB/s before, asking mif 2093000 kHz
B2L ACPM: mif reply=1, copy 19798 MB/s after
```

- **Memory clock.** Already at or near Android's rate: the request changed nothing.
  So the 7.8 ms display copy is not short of bandwidth; it is the uncached panel
  writes and the row compare. The fix for it is scanout from the surface, not a
  faster copy.
- **Render thread.** Draws were 50 to 83% of its CPU time in busy windows, so the
  rest of what it does is small; the display copy runs in the kernel, outside it.
- **Big cluster.** Stepped 2080, 1820, 1560 MHz under 80 to 98% load at 78 to 82 C,
  and sat at 2080 MHz at 16% load and 72 C. No full hold.
- 411 live draws sampled, 0 failures, no GPU fault, no new crash report. About
  710 s.
