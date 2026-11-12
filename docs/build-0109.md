# Build 0109: present only what changed, off the submit path

Status: booted once; the screen stayed dark. The asynchronous copy is turned
off again in [build 0111](build-0111.md); the dirty-span copy is kept. Contains everything in
[build 0108](build-0108.md). Root, firmware and loader are unchanged.

## Before

Every SwapEnd from backboardd copied each updated layer, a full 1440x3040
BGRA frame, into the uncached (write-combined) panel memory. The copy ran
inside the SwapEnd call, so Core Animation could not start its next frame
until about 17 MB had been written, even when only a cursor or a clock
changed.

## Changes

- **Changed pixels only.** The driver keeps a cached shadow copy of what the
  panel shows. For each row of a layer, `b2l_blit_dirty` finds the first and
  last pixel that differ from the shadow and writes only that span, to the
  panel and the shadow. Comparing reads cached memory, so an unchanged frame
  writes nothing to the panel. The shadow is trusted only after one copy has
  covered the whole panel; until then copies are full.
- **Off the submit path.** SwapEnd retains the layer surfaces, queues them
  and returns. A high-priority thread call does the copy, then releases the
  surfaces. The next SwapEnd, or SwapWait, waits for the previous copy first,
  so a submitted buffer is read only until the next swap is accepted: the
  same guarantee scanout hardware gives. Core Animation can render frame N+1
  while frame N is copied.
- **Fallbacks.** If the 17 MB shadow cannot be allocated, every copy is
  full. If the thread call cannot be allocated, the copy runs in SwapEnd as
  before. The boot log states both: `B2L FB: present shadow on|off, copy
  worker on|off`.

## Measurement

```
B2L FB PERF: vsync=... swaps=... copies=... full=... kpix=... written_kb=... copy_ms=... max_copy_us=... waits=... wait_ms=...
```

`kpix` is pixels presented and `written_kb` what reached the panel; their
ratio is the saving. `waits` and `wait_ms` show how often SwapEnd still had
to wait for the previous copy.

## Tests

`tests/test_framebuffer_swap.py` runs the real swap, worker and wait code with
a deferred mock thread call: queueing, references held until the copy,
waiting on the next swap, empty swaps, shadow validation, single-pixel and
row-end spans, 2,000 random edit rounds, disable masks and bounds. The
earlier version of this test referenced diagnostics removed before HEAD and
no longer compiled.
