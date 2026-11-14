# Build 0135: ACPM on the clock controller's channel

Status: booted once. Sensors work; no rate came back. Contains everything in
[build 0134](build-0134.md).

- The rate requests move to channel 0, which [0134](build-0134.md) identified.
- The sensors are read first and no longer depend on the DVFS channel. If the
  first read fails, the driver sends the TMU init request Android sends first,
  once, and reads again.
- The rate read stops at the first domain with no reply, because each costs a
  50 ms timeout.
- The first rate change is logged before it is sent and timed after, followed by
  a cycle-counter measurement.

## Reading the log

```
B2L ACPM: temp big=.. mid=.. little=.. g3d=.. isp=.. guard=1
B2L ACPM: dvfs channel len=.. elem=..
B2L ACPM: mif ... kHz (and the other domains)
B2L ACPM: first change, 156000 to 433000 kHz
B2L ACPM: change took ... ns
B2L GPU: clock ... kHz (...)
```

| Last ACPM line | Meaning |
| --- | --- |
| `no reply for g3d` | Channel 0 does not answer; the GPU stays at the boot clock. |
| `first change`, then the log ends | The rate change itself stalled: take the rollback. |
| `set ... got no reply` | ACPM is left alone for the rest of the boot. |
| `guard=0` | No temperature; the target is capped at 325 MHz. |

## Device result

```
B2L ACPM CH 0: id0 field1 owner4 type1; len f elem 10 credit1 ap_poll1
B2L ACPM: temp big=55 mid=52 little=52 g3d=50 isp=51 guard=1
B2L ACPM: dvfs channel len=15 elem=16
B2L ACPM: no reply for g3d
```

- Channel 0 is a real queue of 15 messages.
- The first TMU read answered with no init request, so the thermal guard works.
- `no reply for g3d` also fired on a reply of rate 0, so this build cannot tell
  the two apart. Android never reads a rate from ACPM: its `cal_dfs_get_rate`
  stays on the AP side.
- No clock changed. 240 s, 123 live draws, 0 failures, no GPU fault.
