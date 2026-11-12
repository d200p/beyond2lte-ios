# Build 0141: network stack service, big cluster steps down, image sources

Status: booted once. The stack service did not work; the heat and GPU changes did.
Contains everything in [build 0140](build-0140.md), without its four flush stripes.

## Settings > General > About

configd contains InterfaceNamer, which publishes nothing until it finds a service
of class `IONetworkStack`, or until `WaitStackTimeout` passes (`No network stack
object`). That class belongs to IONetworkingFamily, which this kernel does not
have. So every `SCNetworkInterfaceCopyAll` blocked until the timeout, about five
minutes after configd started: launches at 321 s hung and one at 1015 s worked.

`B2LPlatformExpert` now registers an empty `IONetworkStack`.

## CPU heat

Under load the big sensor went from 68 to 80 C in seconds while the others stayed
near 73 C, and each hold dropped all three clusters.

- At 80 C on the big sensor only the big cluster steps down, to 1560 MHz, and it
  rises again at 70 C.
- At 88 C on any sensor every cluster returns to its boot rate, until 78 C.
- A lost sensor still ends the boost.

## GPU

- The four-thread flush is gone: it gave about 10%. The smaller flush area stays.
- Images in format 1, exact in every comparison so far, may now draw live, as
  surfaces already do.
- An image whose rows are not a multiple of 64 bytes is copied to an 8 MiB aligned
  buffer and compared in shadow mode. This shows whether formats 9, 11 and 13 are
  plain BGRA. None of them is live yet.
- The `S` line now appears once for each format.

## Reading the log

| Sign | Meaning |
| --- | --- |
| About opens right after boot, no `0x8BADF00D` for Settings | The fix works. |
| Reports still stop in `__wait_for_IOKit_to_quiesce` | Something more follows the stack; read configd's dump. |
| `LIVE kind=1 bl=3/1 u=1` with `fail=0` | Image sources are drawing live. |
| `wr` above 0 for formats 9, 11 or 13 | That format is not BGRA. |

## Device result

- **Settings.** Still killed in `__wait_for_IOKit_to_quiesce`, launched at 222 s,
  and could not be used. configd's stack callback calls `IOServiceOpen` and reaches
  `*STACK*` only if that succeeds, and this stub had no user client. Build 0142
  adds one.
- **Heat.** No full hold this boot. The big cluster stepped down to 1560 MHz and
  stayed between 72 and 80 C under 84 to 98% load.
- **Image sources.** 7 live draws of format 1, 1387 us at 364 kpix, 0 failures.
- **Staged images.** Format 13: 28 of 28 wrong. Format 11: 50 of 60 wrong. Neither
  is plain BGRA, so neither goes live.
- 68 live draws, 0 failures, no GPU fault. 277 s.
