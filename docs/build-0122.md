# Build 0122: camera band

Status: booted once (log to 290 s); visual check of the band by the owner pending. Contains everything in
build 0121 (aligned dirty copy, battery driver). Root, firmware and loader
are unchanged.

## Change

The S10+ front-camera hole sits in the top-right corner of the 1440x3040
panel, where the iOS status bar draws its battery and signal icons. The
framebuffer driver now keeps the top 136 rows black and presents iOS below
them:

- `GetDisplaySize` reports 1440x2904, exactly 480x968 points at the 3x scale
  this model uses (3040 rows would be 1013.33 points).
- Copies, clipping and the dirty-copy shadow all use the area below the
  band; every row start stays 64-byte aligned (136 x 5760 bytes).
- The band is cleared once, when the first frame arrives, which also removes
  boot-console text there. The default IOSurface stays the whole panel.
- Touch: the controller reports 0..4095 over the whole panel. The vertical
  axis is remapped to the presented area; a touch in the band lands on the
  top edge (`b2l_hid_touch_display_y`, `tests/test_touch_inset.py`).
- Boot argument `b2l_fb_top_inset` overrides the 136 rows (0 disables the
  band); values above a quarter of the panel are ignored.

## Open points

- Whether iOS lays out for the reported size or for the size built into the
  iPhone17,2 model data (2868 rows) must be seen on the device: the UI should
  fill 1440x2904 without letterboxing or cropping.
- 136 rows assume a camera hole of about 5 mm at 522 ppi; if the status bar
  still touches it, the inset needs to grow.

## Device result

- iOS used the reported size: backboardd created its frame surfaces at
  1440x2904 and every swap presents a full 1440x2904 layer at the origin.
- Touches arrive remapped; one touch inside the band was delivered at the
  top edge (y=0).
- Presents: typically 3 to 5 ms; up to 137 swaps per 10 s.
- New crash reports since 0121: HeuristicInterpreter `fjcvtzs` (known) and
  two Settings hang stackshots; no backboardd crash.
- Battery: 100% then 99%, 4.2 V, discharging.
