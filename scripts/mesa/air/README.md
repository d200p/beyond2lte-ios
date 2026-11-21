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

Changes made here to these files, each marked "S10 port" in the source and
made in a commit of its own so the difference from upstream stays readable:

- `air_intrinsics.c`: iOS 26.6's AIR (`air64_v28`) reads a texture with
  `air.read_texture_2d(tex, air.get_read_sampler(), coord, offset, level, 0)`.
  `air.get_read_sampler` gives a sampler pointer bound to nothing, and the read
  branch consumes it and the offset (adding a non-zero one to the coordinate).
  QuartzCore's `sdf_gen_field`, `sdf_gen_gradients`, `rim_outline`,
  `brim_jump`, `brim_outline`, `read_surf_frag`, `compute_apl` and
  `compute_average_luma` were refused for it in boot 64.
- `air_to_nir.c`, `air_private.h`: field folding (`plan_field_folding`,
  `field_fold_store`, the head of `air_load_ptr`). iOS 26's uber shader keeps
  its configuration in a `FragmentShaderConfig` struct filled by the
  constructor and read through `select(has_config == 0, @config, runtime)`;
  only scalar globals were folded, so the shader stayed whole whatever the
  constants (72 618 NIR instructions, 47 s in Bifrost, boot 65). A variable
  whose every store is in the first block of an `air.static_init` function,
  through constant getelementptrs and casts only, and whose address escapes
  nowhere, now answers loads of bytes those stores wrote as constants.
- `air_to_nir.c`, `nir_air.h`: `air_module_function_constant_decls`, each
  function constant's index with its name and type name, read from the same
  `!air.function_constants` nodes as `air_module_function_constants`. Core
  Animation gives 46 of its constants by name (`setConstantValue:type:withName:`)
  and the Metal runtime (`scripts/mesa/b2l-target/b2l_mtl.c`, milestone M2)
  matches those names to indices with it.
- `air_to_nir.c`, `nir_air.h`: `air_varying.link`, the string a varying is
  linked by on both stages (after `air.vertex_output` / `air.fragment_input`,
  `generated(5colorDv4_Dh)`); `name` stays as it was, which for a fragment
  input is its `air.arg_name` and does not match the vertex output's. The Metal
  runtime links a pipeline's fragment inputs to its vertex outputs with it.
