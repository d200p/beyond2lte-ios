# AIR frontend (Metal shaders to NIR)

These files are HakiOS's AIR frontend, copied unchanged from
[oosexgang/hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel)
at commit `63fc2b4633bac3af8e57dadab9f5a8d4cb940438`, directory `tools/mesa/air/`
(`air_run.c` left out: it needs freedreno). Licence: MIT, "Copyright © 2026
Darwin on redwood contributors", as each file's header says.

What it does: reads Apple's shader bitcode (AIR, one module per function of a
`.metallib`) with its own bitcode reader, no LLVM, and translates it to Mesa
NIR. Its conventions (UBO index = `[[buffer(n)]]`, texture and sampler indices
separate, `shader_in`/`shader_out` variables, upper-left `frag_coord`) are in
HakiOS's `docs/air.md`, section 3.1.

`scripts/mesa/build-panfrost-ios.sh` copies this directory to
`src/compiler/air` of the Mesa 25.0.7 tree and adds `subdir('air')` after
`subdir('spirv')` in `src/compiler/meson.build` (HakiOS's
`tools/mesa/air-frontend.patch`, the same single line). The S10 port's own use
of it is `scripts/mesa/b2l-target/b2l_mtl_sweep.c` (docs/metal-on-panfrost.md,
milestone M0).

Changes made here to these files: none. Fixes go in as separate, marked
commits so the difference from upstream stays readable.
