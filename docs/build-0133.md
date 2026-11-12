# Build 0133: ACPM table dump, more live draws

Status: booted once; froze before drawing anything. See the result below. Contains everything
in [build 0132](build-0132.md).

## 0132 result

116 live draws, no failures, no wrong pixels on screen, no GPU fault, no
backboardd crash. ACPM never ran: the DVFS channel read queue length 1, which
makes a send impossible, so the client refused and nothing reached the firmware.
No clock changed.

`kind` 1 Sover draws took 30.6 ms on the CPU against 6.1 ms on the GPU and none
went live. Each 5 s window held so few that the rule "compare the first 4" used
them all.

## Changes

- The driver dumps ACPM's framework, the first 12 channels with their queue
  indices, and the plugin table, as raw words. Android can load ACPM plugins
  from files, so the DVFS plugin may not be running here at all.
- One draw per kind is compared in each 5 s instead of four.
- `X` lines gain `self=`: whether a draw samples its own destination, for `kind`
  52.

## Device result

The kernel stopped at about 72 s, right after `reading channel table`, with no
panic. Walking all 12 channels and following their offsets read somewhere the
bus does not answer. 0132 read channel 5 and its queue indices without trouble,
so part of this SRAM simply must not be touched. See [0134](build-0134.md).
