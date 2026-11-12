# Build 0126: GPU interface and draw measurement

Status: booted once; everything loaded and worked, chord and persistence of
power-off still to confirm. Contains everything in [build 0125](build-0125.md).
Root, firmware and loader are unchanged. The UI still draws on the CPU.

## Kernel interface

After its self test passes, `B2LMali` stays up for one user client (selectors
in `b2l_mali_uapi.h`): log, create buffer, map surface, submit, info. The first
request powers the GPU and builds its address space; closing powers it down.

- Buffers are cached kernel memory mapped into the client.
- Surfaces are mapped on demand into fixed 32 MiB slots, oldest out first, at
  most 256 MiB, and stay retained while mapped. Larger surfaces are refused, so
  the caller falls back to the CPU.
- Jobs are synchronous and polled, with a 1 s limit. A failed job logs the fault
  registers and resets the GPU.
- The CPU cleans the pass, each source whose seed moved, and the target before
  a job, and the target again after it. `COHERENCY_FEATURES` is 0.

## Library in backboardd

`userspace/b2l-gpu.c` hooks `CA::OGL::SWContext::draw_elements`. It **only
measures**: every draw still runs on the CPU. Every 5 s it logs the time spent
in draws, grouped by render state, and it samples vertices so their layout can
be read. At start it runs one pass from one real IOSurface to another through
the kernel interface and compares every pixel.

The kernel publishes the library from BOOT to
`/usr/local/lib/b2l-gpu-<hash>.dylib`, adds it to backboardd's environment, and
lets dyld honour that for `/usr/libexec/backboardd` alone. No Apple file
changes. A failed load leaves backboardd running without it. The cache UUID and
the bytes being replaced are checked before anything is patched.

The previous BOOT image removes it: the file remains but is no longer loaded.
Boot argument `b2l_gpu_ui=0` turns it off.

## Power

Until now Shut Down ended at `CPU halted` with the phone still on and no way
out but a forced reset, and Restart did not restart. `haltRestart` had no
platform implementation.

- **Shut Down** records the reason in `INFORM2`, waits up to 10 s for the power
  key to be released (a held key turns the phone back on), and clears `PS_HOLD`.
  If power stays on, which a connected charger may cause, it restarts.
- **Restart** clears `RST_STAT` and triggers `SWRESET`.
- **Both volume buttons held for 3 s** syncs the disks and restarts into
  recovery, through `INFORM3` as Android's `reboot recovery` does.

Before cutting power the kernel waits 6 s so the persistent log reaches disk.
Panics are untouched and keep their halted screen. Android also tells ACPM to
stop first; this port has no ACPM client, so that step is skipped. Whether the
phone stays off with USB connected has to be seen on the device.

## Reading the log

```
B2L GPU UI: library publication error=0 bytes=...      (printf, early boot)
B2L GPU: client opened by pid ...
B2L GPU UI: test: submit=0x0 status=1 ... bad_pixels=0
B2L GPU UI: hook: installed at ..., protect=0
B2L GPU UI: W window_us=... draw_us=... draws=... keys=...
B2L GPU UI: K kind=... blend=.../... mode=... units=.../... draws=... idx=... us=...
```

| Line | Meaning |
| --- | --- |
| No `client opened` | The library did not load or the self test failed. Check publication and backboardd's crash reports. |
| `hook: vm_protect write failed` | The cache page cannot be made writable in the process. The UI is unaffected; the hook needs another route. |
| `hook: entry bytes differ`, `not cache 23G90` | The hook refused to patch. |
| `test: ... bad_pixels` above 0 | The interface works but cache maintenance does not. |
| `B2L POWER: still on after PS_HOLD, restarting` | Dropping PS_HOLD did not cut power. |

`draw_us` against `window_us` says how much of the time goes to drawing, and
the `K` lines say which draws cost it. The translator handles the costly ones.

## Device result

Library published, loaded into backboardd (pid 62) and opened the GPU once, with
no runtime failure, GPU fault or panic. A 256x256 pass between two real
IOSurfaces took 104 us tiler and 175 us fragment, `bad_pixels=0`. The hook
installed: patching shared-cache text in the process works, and 61,601 draws
ran through it. New reports since 0125: three Preferences hangs and one
SiriSearchFeedback report, no backboardd crash.

Over 288 s, draws took 58.3 s, and 59 to 76% of the wall clock in the eight
busiest 5 s windows. The table holds 48 state combinations and filled up, so
49.0 s (84%) is attributed:

| Share of attributed time | Draws |
| ---: | --- |
| 33.5% | `kind` 1 (one texture unit) |
| 27.4% | `kind` 3 (one texture unit, pixel coordinates) |
| 12.0% | `kind` 0 (solid colour) |
| 10.8% | `kind` 52 |
| 16.2% | everything else |

Blend bytes index `SW::Blend`: 1 Copy, 3 Sover, 12 Add, 50 DarkenSover. All but
DarkenSover (2.5%) in kinds 0, 1 and 3 are fixed-function blends. `kind` is not
yet decoded beyond its texture-unit count; 0xb0 of the state holds each unit's
binding, 0x11 its image function. Unit type 1 and 2 are images, 3 and 4 not yet
identified.

`CA::OGL::Vertex` is 48 bytes: position vec4 (window pixels), texture coordinate
vec4, colour as four half floats.

A full-screen black fill took 13.8 ms on the CPU. Peak presentation was 375
swaps per 10 s with little changing on screen; that is not a speed-up, the UI
path is unchanged.

Shut Down reached `reboot(2)` with RB_HALT and logged `powering off`, with no
`still on after PS_HOLD`. The bootloader's reset reason does not separate this
from a forced reset, so power-off rests on that absence. The chord logged
nothing and was not exercised. `log show` cannot open these exported archives,
so backboardd's unified log was not reviewed; crash reports and the kernel log
were.

Open: the GPU stays powered for as long as backboardd runs, even with nothing
to draw. Idle power-down is needed before this can stay on.

## Tests

`tests/test_exec_gpu_ui.py` runs the exec hook, `tests/test-lwcr.py` the dyld
policy (backboardd only, once published, never set-uid), and
`tests/test_mali_core.py` page unmapping, and `tests/test_power.py` the power
sequences. The user client, surface slots and
the hook itself have no host test yet; 0126 is their first run.
