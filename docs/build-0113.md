# Builds 0113 to 0115: isolating the 0109/0111 stall

Status: resolved. 0119 (64-byte aligned dirty spans) works; 0121 is the result with the battery driver. Root, firmware and loader unchanged.
All three have the battery driver switched off in the build tree only.

## The fault

0109 and 0111 stall the whole kernel (confirmed by the owner) about 125 s
after boot. In both, the persistent log stops together with the vsync
timer and the wait sampler, within about 5 s of the point where SpringBoard
has opened IOSurface and backboardd is about to present the first frame
with changed content. 0107 and 0108 pass the same point. That frame would
be the first time `b2l_blit_dirty` writes non-empty spans, and that code
never ran on 0107 or 0108.

An independent review (five lenses, two adversarial checks per claim) found
no bounds, alignment, lifetime, locking, memory-exhaustion or physical-layout
mechanism in the new present path, so the cause is not yet understood. It
did find one real gap: the boot console could draw on the panel during the
first copy, after which the shadow no longer matches the panel. The console
is now stopped before the first copy.

## Changes in the sources

- The dirty copy is off unless boot argument `b2l_fb_dirty=1` is given; the
  default present path is the 0108 full copy inside SwapEnd.
- `B2L FB COPY n=... begin` and `... end written=... us=... nonblack=N/64`
  bracket the first 64 copies. A begin without an end places a stall inside
  the copy; `nonblack` shows whether the frame was black.
- For the first 300 s the persistent log is written every second, with a
  `B2L TICK sec=...` line each time, so the last line on disk dates a stall
  to within about one second.

## Images

| Image | Shadow | Purpose |
| --- | --- | --- |
| 0113 | none (default) | Expected to work like 0108. |
| 0114 | 17.5 MB allocated, never used | Does the allocation alone trigger the stall? |
| 0115 | dirty copy on | Catches the stall inside or after a dirty copy. |

## Result so far

0113 shows the UI, so the restructured copy code, the per-second log and the
earlier console stop are fine, and the stall depends on the shadow. 0114 and
0115 separate the allocation from the dirty copy.

## 0116

0116 is the default sources (as 0113) with the battery driver from
[build 0110](build-0110.md) enabled.

## 0114 result

The unused 17.5 MB shadow was allocated and the boot ran normally past
193 s, so the allocation is not the trigger. The traced copies show the
first UI frames arriving as a burst at about 127 s (64 full copies of about
16 ms each between 127 and 130 s, mostly black content), which is exactly
when 0109 and 0111 stalled. The stall therefore needs the dirty copy itself
handling changed frames. 0115 shows whether it stops inside a copy.

## 0115 result

The stall is not inside a copy. Only two copies ran, both completed: the
first fill of the shadow at 103 s (17.5 MB written to the panel and the
shadow, 18 ms) and a dirty compare at 105 s (nothing written, 3 ms).
backboardd then sent no further frame; the per-second ticks continue to
125 s and stop. In 0114 the next frame came at 127 s, so the kernel stops
before the first UI frame.

What 0115 does and 0114 does not is write 17.5 MB into the shadow's pages.
In 0109, 0111 and 0115 the stall follows that first fill by 22 to 24 s.
The stock device trees' reserved regions do not explain it (only the 4 KiB
`mm_debug` page at 0x91200000 falls inside an owned range), so the shadow's
physical location has to be measured.

## 0117: memory probe

Default full-copy present path, battery driver off, with `b2l_fb_probe`
forced on. The probe allocates a panel-sized buffer that is never presented
and:

1. logs its physical runs (`B2L FB PROBE: run ... pa=... len=...`);
2. hashes every page every 5 s and logs any page that changed without the
   kernel writing it (`foreign write page=... pa=...`);
3. fills it with a pattern at 60 s uptime, as the dirty copy's first fill
   would, and keeps checking.

A stall about 20 s after the 60 s fill implicates writing those physical
pages; a stall at about 125 s instead points to a time-based trigger; a
foreign write names the page another agent is using.

## 0117 result

No stall: the log runs past 299 s and the UI frames arrive at 126 s. The
probe buffer occupied physical 0x82ca0000..0x83460000; no page changed
without the kernel writing it, before or after the 60 s fill, and the fill
itself caused nothing. Writing a large buffer is therefore not enough to
stall the kernel, at least at those addresses.

## 0118

Dirty copy on, but every SwapEnd is padded with IOSleep to at least 16 ms,
the duration of a full copy, and the shadow's physical runs are logged
(`B2L FB SHADOW: run ... pa=...`). If 0118 works, the stall depends on
SwapEnd returning faster than before (a timing race elsewhere). If it still
stalls, the shadow's addresses can be compared with 0117's probe buffer.

## 0118 result

Stalled at about 125 s, like 0115. The shadow occupied the same physical
pages (0x830a0000..0x83460000) as 0117's probe buffer, which 0117 filled
without effect, and padding SwapEnd to 16 ms changed nothing. So neither
the shadow's location nor SwapEnd timing is the cause. Input polling and
the log ran normally until 120 to 125 s.

In 0117 the first UI frame (swap 3) is copied at 126 s. 0118's last
persisted tick is at 125 s, and the log is drained once a second, so the
lines of a stall during swap 3 are lost. In every stalling build that
frame would be the first dirty copy with changed content, i.e. the first
time partial-row spans are written into the panel memory. Every working
build writes whole frames instead. The stall is silent (no panic), which
would fit an error the CPU does not report as a kernel fault, but this is
not proven.

## 0119 and 0120

Both run the dirty copy (battery driver off) and differ only in how a
changed row is written:

| Image | Written per changed row |
| --- | --- |
| 0119 | the changed span widened to 64-byte boundaries |
| 0120 | the whole row |

The 0119 expression was checked on the host with random edits at every
4-byte row alignment: the panel always matches the source and every write
starts and ends on a 64-byte boundary or a row edge.

## 0119 result and fix

0119 works: the UI appears and the boot runs past 260 s. Short writes at
arbitrary 4-byte offsets into the write-combined panel memory stopped the
machine; spans widened to 64-byte boundaries do not. The sources now always
widen dirty spans that way (`B2LBlit.h`, checked per span in
`tests/test_framebuffer_swap.py`), and the dirty copy is on by default again
(`b2l_fb_dirty=0` restores full copies). 0120 was not needed.

Measured in 0119: a typical present takes about 3.5 ms and writes 0.13 to
1 MB instead of 16 ms and 17.1 MB; copying is about 5% of the time. Frames
still peak at about 13 per second, which is Core Animation's software
rendering of 1440x3040 frames on the CPU, not the present path.

## 0121

Default sources (aligned dirty copy, per-second log for the first 300 s)
with the battery driver from [build 0110](build-0110.md).
