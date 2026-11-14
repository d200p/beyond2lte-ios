# Build 0144: float and RGBA images live, masks and tints in shadow

Status: BOOT image built; see the result below once booted. Contains everything
in [build 0143](build-0143.md).

- Image formats 11 and 13 may draw live from 100,000 pixels, through the aligned
  copy when their rows need it. In 0143 neither showed a wrong pixel, but those
  samples were small, so large draws are new.
- A key with a wrong comparison in the current 5 s window stays on the CPU until
  the window ends.
- `tinted`, a new shader, multiplies the texture by a full colour. It replaces the
  old rejection of tinted draws (`R_TINT`).
- Image format 9 is read as a one-channel mask, R8 swizzled to RRRR. Texture and
  shader passed on the real GPU in recovery: `tinted bad_pixels=0`.
- Tinted and mask draws are compared in shadow only.

| Sign | Meaning |
| --- | --- |
| `LIVE ... u=1` with `fail=0` for large draws | Images are drawing live. |
| `wr` near 0 for format 9 and tinted keys | They can go live next. |
| Wrong colours in images or icons | Formats 11 or 13 go back to shadow. |
