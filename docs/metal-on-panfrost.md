# Metal on the Mali-G76 (Panfrost)

Goal: Core Animation's own Metal renderer (`CA::OGL::MetalContext`) drawing the
window server on the S10's GPU, so that the effects iOS 26 only has in Metal
(Liquid Glass, vibrancy, SDF shapes, the blur passes) are drawn by Apple's own
shaders instead of being approximated on the OpenGL ES path.

Status (2026-11-17): M0 written, not built yet. The OpenGL ES path
(`docs/mesa-panfrost.md`) stays the default and the fallback.

## M0 as written

- `scripts/mesa/air/`: HakiOS's AIR frontend, unchanged (its README says from
  which commit); `build-panfrost-ios.sh` copies it to `src/compiler/air` and
  adds `subdir('air')`; the b2l target links `idep_air`.
- `scripts/mesa/b2l-target/b2l_mtl_sweep.c`, exported `b2l_mtl_sweep(start,
  progress_path)`: thin the fat library to its AIR64 slice, `air_metallib_parse`,
  then per job `air_to_nir` (Panfrost v7's NIR options) →
  `nir_assign_io_var_locations` → `bifrost_preprocess_nir(0x7211)` →
  `panfrost_nir_lower_sysvals` → `pan_shader_compile_v7`. Jobs: every function
  once with no function constant supplied, then each `fixed_frag_*` once per
  texture function in the GLES census list with constant 28 supplied. One log
  line per job through a B2LMali client that only logs (the kernel's runtime is
  reached by no log call).
- `userspace/b2l-gpu.c`: with `b2l_mtl_sweep=1`, 45 s after backboardd starts,
  once per boot, `posix_spawn` another `/usr/libexec/backboardd` with
  `B2L_MTL_SWEEP=<job>`; the library's constructor sees it first and runs the
  sweep at background QoS, then exits. A child that dies or makes no progress
  for 120 s is restarted after its job (40 restarts at most); the progress file
  is `/private/var/tmp/b2l-mtl-sweep.next`, 4294967295 when finished.
- `uefi/XnuLoader/XnuLoader.c`: `b2l_mtl_sweep=1`.

What its log is to answer: which of QuartzCore's functions the frontend takes
on iOS 26's AIR (HakiOS measured iOS 16.5's), where Bifrost's compiler stops,
and how long the uber shader takes to compile on the phone's CPU.

## M0 result (boot 64, build 63)

QuartzCore's library: AIR64 slice 1,552,128 bytes, MTLB 1.2, 189 functions.

- 137 compiled to Bifrost code: every vertex function (the fixed_vert uber
  shaders in 1 to 4 s), the downsample, narrow and variable blurs, all six
  glass functions (glass_background[_sdf][_no_bleed], glass_foreground[_sdf],
  5 to 14 KB of code), the framebuffer-fetch ones (p3_clamp, edr_clamp,
  path_blit).
- 16 refused as tile shaders / imageblocks: expected, CA asks for them only
  when the device supports tile shaders.
- 16 refused for `air.get_read_sampler` (iOS 26's form of texture.read()).
- 9 functions (x2) crashed Bifrost in `bi_alu_src_index` (stack from the
  kernel's B2L BT lines with the Mesa symbol map): all and only the fragment
  functions that read `[[position]]`. The frontend emits it as a system-value
  variable for the consumer to lower; nothing lowered it.
- `fixed_frag_lph_cph`/`_cpf` ran past the 120 s limit, unsupplied and with only
  the texture function supplied; which phase was not known.

## M0.1

- The AIR frontend takes the iOS 26 read form (scripts/mesa/air/README.md).
- `nir_lower_system_values` (and the compute one), the half-pixel centre and
  then the driver locations before Bifrost.
- The uber shader only with constants, six texture functions, each phase
  (AIR, preprocessing, Bifrost) logged with its time and NIR size; the stall
  limit 600 s per phase; the child at utility QoS with the CPU monitor off;
  the parent logs the phase a child died in.

## M0.1 result (boot 65, build 64)

- 173 of 189 functions compile; the 16 left are the tile shaders. The read
  form and the position fix work (dynamic_system_ootf_decimate, the radial and
  conic gradients, variable_blur_frag, path_rect/path_cubic all compile).
- Ordinary functions: AIR 5 to 200 ms, Bifrost 2 to 150 ms at utility QoS.
- The uber shader did not specialise: 72 618 NIR instructions for texture
  functions 0, 1 and 52 alike, 56 475 after preprocessing, 47.4 s in Bifrost,
  296 160 bytes of code, 64 registers, 220 bytes of thread storage. Its
  configuration is a struct the frontend did not fold.
- QuartzCore's `default.pipelinelib` is an empty directory on iOS 26: there
  are no prebuilt variants to take, the specialisation is ours.
- backboardd (the real one) crashed at 196 s opening Weather, in the GLES
  path: `gli_tex_image2D` → `_mesa_TexImage2D` → `st_TexSubImage` →
  `util_copy_box` → `memcpy` reading the pixels Core Animation passed. Not in
  the Metal code (another process), while the child compiled the uber shader
  at utility QoS; no jetsam.

## M0.2

- Field folding in the frontend (scripts/mesa/air/README.md).
- The sweep compiles only the two uber shaders: six texture functions each,
  and the generic shader (fc_generic = 1, the configuration from its buffer at
  run time) once, which is the one that could be compiled once and kept as the
  fallback while specialised variants compile. A NIR clean-up after the
  frontend shows what folding left; 120 s after backboardd starts, background
  QoS.

## What iOS 26 does (read from the 23G90 cache)

- **Discovery.** `-[MTLIOAccelServiceGlobalContext init]` (Metal) matches
  `IOAcceleratorES` services; `getMetalPluginClassForService` reads the
  service's `MetalPluginName` (bundle `/System/Library/Extensions/<name>.bundle`)
  and `MetalPluginClassName` (`-classNamed:`, else the principal class), and
  keeps the class only if it is a subclass of Metal's base device class
  (`_MTLDevice`, as HakiOS found on iOS 16). The device is then made with
  `-initWithAcceleratorPort:`. Same mechanism as iOS 16.
- **Core Animation's side.** `CAMetalContextCreate` probes `IOAcceleratorES`
  for about 30 s today and falls back to OpenGL ES (`docs/build-0065.md`).
  QuartzCore's Metal code (600 functions) sends 353 distinct selectors. It asks
  the device first what it can do: `supportsTileShaders`,
  `supportsMemorylessRenderTargets`, `supportsRasterOrderGroups`,
  `supportsSIMDGroup`, `supportsSIMDReduction`, `supportsSIMDShuffleAndFill`,
  `supportsNativeHardwareFP16`, `supportsLossyCompression`, `supportsFamily:`,
  limits (`maxTextureWidth2D`, `maxFragmentTextures`, alignments). Answering NO
  where the Mali has nothing keeps CA off tile shaders and imageblocks.
- **Shaders.** `QuartzCore.framework/default.metallib` exists on iOS 26 (fat:
  AIR64 slice + Apple GPU binary; 189 functions per slice). The window server's
  fragment work is one uber shader (`fixed_frag_lph_cph`) specialised by
  function constants; `FragmentShaderConfig` field 2 (`texture_function`) is the
  same function code the OpenGL ES path reports (52 = `vcm`, 59 = `dff`, ...).
  Separate functions: downsample/blur passes, variable blur (render and
  compute), glass background/foreground, SDF field generation, path rendering.
  LLVM 23's `llvm-dis` reads the AIR (tools/third-party, not committed).

## Architecture

```
backboardd (CA::OGL::MetalContext)
   |  Metal.framework: MTLCreateSystemDefaultDevice -> IOAcceleratorES -> bundle
   v
B2LMetal.bundle (Objective-C)       _MTLDevice / _MTLCommandQueue / ... subclasses,
   |                                MTLDevice + MTLDeviceSPI etc. declared,
   |                                unknown selectors logged once, answered 0
   v
libb2l-mesa.dylib: b2l_mtl_* (C)    a Gallium frontend over the same pipe_screen
   |                                the EAGL layer uses (b2l_fscreen):
   |                                buffers, textures (IOSurface import), samplers,
   |                                pipelines (blend/raster/DSA CSOs + shaders),
   |                                render passes (framebuffer, clears, draws),
   |                                blits, compute dispatch, fences
   |  shaders: AIR --air_to_nir (HakiOS, MIT)--> NIR --Metal lowering--> Panfrost
   v
Panfrost (Gallium, Mesa 25.0.7) -> b2l_drm -> B2LMali (kernel)
```

Why Gallium and not a hand-written backend (hakios-metal does PM4 for the
Adreno by hand) or Metal on Vulkan (metal2vulkan + PanVK): Panfrost already
runs here, handles job chains, tiler, blend shaders, framebuffer fetch, AFBC
and IOSurface import; PanVK on Bifrost v7 is not conformant in 25.0.7 and the
Vulkan route measured half the speed on the Adreno.

### Metal semantics on Gallium

| Metal | Gallium / NIR |
|---|---|
| NDC y up, row 0 at the top, depth 0..1 | viewport y scale negated, `clip_halfz`, front face flipped with it, `[[position]]` upper-left origin |
| `[[buffer(n)]]` constant / `setVertexBytes` | UBO n+1 (user buffers for bytes) |
| `[[buffer(n)]]` device | SSBO n |
| `[[texture(n)]]`, `[[sampler(m)]]` | sampler view n, sampler state m (separate indices in NIR tex) |
| `constexpr sampler` | a sampler state made at pipeline creation, bound past the API's |
| `[[color(n)]]` input (framebuffer fetch) | `load_output` in the fragment shader (Panfrost: tile buffer read) |
| function constants | folded by air_to_nir (`is_function_constant_defined` kept) |
| vertex descriptor | `pipe_vertex_element` + vertex buffers |
| blend descriptor per attachment | `pipe_blend_state` (independent blend) |
| load/store actions | `clear` at pass begin; store Don'tCare = invalidate |
| shared storage buffers | persistent coherent mapped `pipe_resource` |
| `MTLSharedEvent`, completion handlers | `pipe_fence_handle` + a completion thread |
| memoryless textures | ordinary textures |
| tile shaders, imageblocks, raster order groups, SIMD-group functions | not offered (device answers NO) |

## Milestones (each a build and a boot; GLES stays the default)

| # | What | Checked by |
|---|---|---|
| M0 | air_to_nir in the Mesa build; at boot `b2l-gpu` compiles every function of the phone's own QuartzCore `default.metallib` (no file leaves the phone) through NIR to Bifrost code, the uber shader with the function-constant sets the GLES census saw | kernel log: compiled / refused per function, with the reason |
| M1 | kernel: an `IOAcceleratorES` nub under B2LMali with `MetalPluginName`/`MetalPluginClassName` behind boot-arg `b2l_metal=N` (1: a test program only); B2LMetal.bundle installed in the root; device + queue + caps, every other selector logged | `b2l-mtltest`: the device, its name, the first unanswered selectors |
| M2 | buffers, textures (IOSurface), samplers, library/functions, render and compute pipelines, command buffers, blit, render pass with clear and draw | `b2l-mtltest` draws a triangle and a textured quad into an IOSurface, read back pixel by pixel |
| M3 | backboardd shown the device (`b2l_metal=2`); a marker falls back to OpenGL ES after a crash, as the GLES bring-up did | the lock screen and home screen drawn by Metal; crash reports |
| M4 | the rest CA asks: events, heaps/residency sets, variable blur compute, glass passes; speed | the GLES glitches gone (labels, Calculator keys, widgets, tab bars), frame rate |

## Licences

- HakiOS `tools/mesa/air/` (AIR to NIR, metallib reader): MIT, "Darwin on
  redwood contributors"; copied with its headers.
- hakios-metal `plugin/HakiosMetal.m`: MIT; read as reference.
- HakiOS `userland/src/HakiosMetalCA.m`: origin unverified there; not copied.
- Apple's `default.metallib` and the AIR disassembly stay off the repository.

## M0.2 result (boot 66, build 65)

Field folding works. fixed_frag_lph_cph and _cpf, background QoS (little
cores), texture function supplied and nothing else:

| texture function | NIR after the frontend | after clean-up | Bifrost | code |
|---|---|---|---|---|
| 0 clr | 677 | 278 | 34 ms | 1392 B |
| 1 img | 665 | 268 | 21 ms | 1216 B |
| 52 vcm | 963 | 349 | 36 ms | 1216 B |
| 59 dff | 748 | 298 | 24 ms | 1216 B |
| 66 kfh | 956 | 398 | 39 ms | 1216 B |
| 70 ghn | 664 | 268 | 22 ms | 1264 B |
| generic | 72 657 | 24 910 | 176 555 ms | 273 392 B |

The frontend takes 0.9 to 1.9 s per variant (the whole module is read and
translated each time); Bifrost tens of milliseconds. Every variant still has
148 bytes of thread storage: the constructor's stores into the folded struct
stay, and nothing removes stores to scratch nobody loads. No crash in the
boot; spindump stayed quiet.

What it decides for M2: compile the specialised variant a pipeline asks for,
on demand, and keep a cache on disk; the generic shader is not worth its
three minutes. To do later: read each module once per library, not once per
variant; drop dead scratch stores.

## M1 as written (build 66)

- `xnu-overlay/iokit/Kernel/arm/B2LMali.cpp`: a class `IOAcceleratorES` and,
  with boot argument `b2l_metal=N` (N >= 1), one such nub under B2LMali once
  its self-test passed, with `MetalPluginName` = `B2LMetal-<hash>` and
  `MetalPluginClassName` = `B2LMetalDevice` ("B2L METAL:" in the kernel log).
- `xnu-overlay/bsd/kern/b2l_userspace_seed.h`: the bundle published with the
  GPU library, `/System/Library/Extensions/B2LMetal-<hash>.bundle/{Info.plist,
  B2LMetal}`, the hash over both so a new build is a new bundle.
- `scripts/build-gpu-library.py`: builds `userspace/b2l-metal.c` (a dylib,
  ad-hoc signed) and its Info.plist into the kernel's payload header.
- `userspace/b2l-metal.c`: the plug-in. A `_MTLDevice` subclass made at run
  time with `MTLDevice` and `MTLDeviceSPI` declared, `-initWithAcceleratorPort:`,
  `-name`, `-registryID`, `-maxBufferLength`, `-newCommandQueue` (a
  `_MTLCommandQueue` subclass with the `MTLCommandQueue*` protocols); every
  other message answered with a zero of the protocol's return type and logged
  once ("mtl M1:"). The approach is HakiOS's (hakios-metal plugin, MIT).
  Level 1 shows the device only to a process with `B2L_MTL_TEST` set; level 2
  (M3) to everyone.
- `userspace/b2l-gpu.c`: 60 s after backboardd starts, once per boot, another
  backboardd with `B2L_MTL_TEST=1`, whose constructor calls
  `MTLCreateSystemDefaultDevice()`, logs the device, its name, a command queue
  and a few capability answers, and exits.
- `uefi/XnuLoader/XnuLoader.c`: `b2l_metal=1`; the M0 sweep off.

Also in this build, for the OpenGL ES path: `gli_tex_image2D` and
`gli_tex_sub_image2D` (b2l_eagl.c) check with `vm_region_64` that every page
the upload reads is mapped and readable, and upload nothing otherwise -- boot
65's backboardd crash (memcpy under `_mesa_TexImage2D`), the check UniXNU's
AdrenoMetal added on 18 November for client buffers.

## M1 result (boot 67, build 66)

- The kernel published `B2LMetal-431d96f31bbd.bundle` (error 0) and registered
  the IOAcceleratorES at level 1.
- The test child: `MTLCreateSystemDefaultDevice()` returned a `B2LMetalDevice`
  in 80 ms; `-name` "Mali-G76 MP12 (Panfrost, S10)", `-registryID` 8451 (the
  accelerator port); `-newCommandQueue` a `B2LMetalQueue`. While adding the
  device Metal asked one message the class has no method for: `-featureProfile`
  (answered 0). `_MTLDevice` itself answers the capability questions:
  `supportsTileShaders` 0, `supportsRasterOrderGroups` 0,
  `supportsNativeHardwareFP16` 0, `supportsFamily:` Apple4 0 -- and
  `supportsMemorylessRenderTargets` 1, which M2 has to answer itself.
- backboardd, SpringBoard, chronod, iconservicesagent, WidgetRenderer, Weather
  and MobilePhone loaded the bundle, were not shown the device, and stayed as
  they were. No crash in the boot.
- With an IOAcceleratorES present Core Animation stops looking for one sooner:
  backboardd made its OpenGL ES renderer at about 49 s instead of 59 s; the
  context was ready at about 61 s in both boots.
- The texture upload page check did not have to act (Weather opened).

## M2a as written (build 67)

M2 in two builds: M2a the objects, the shaders through Gallium and render
passes that only clear; M2b the draws.

- `scripts/mesa/b2l-target/b2l_mtl.h`, `b2l_mtl.c`: the Gallium half, one
  table (`b2l_mtl_api(1)`) the plug-in takes with one dlsym. The process's
  Panfrost screen (`b2l_fscreen`, the EAGL layer's) and a context of its own
  under a lock; buffers mapped once, persistently, for `-contents`; textures,
  IOSurfaces imported as the EAGL layer imports them; sampler and
  depth-stencil CSOs; libraries read by the AIR frontend (fat or thin, from a
  file or bytes); functions with their constants (by index, or by name through
  `air_module_function_constant_decls`), translated when a pipeline first needs
  them and kept; render pipelines: fragment inputs linked to vertex outputs by
  `air_varying.link`, system values lowered, the half-pixel position, driver
  locations, then Panfrost's own `create_vs_state` / `create_fs_state` (which
  compile the default variant there and then), blend and vertex element CSOs;
  command buffers recorded as a list and executed in order at commit (render
  passes: framebuffer, Clear load actions as `clear` per target; buffer copy
  and fill; texture copy; buffer to texture and back), then a flush with a
  fence. B2LMali runs each job to its end, so commit returns with the GPU's
  work done.
- `scripts/mesa/air`: two frontend additions (README): constant names and
  types; the name varyings link by.
- `userspace/b2l-metal.c`: buffers, textures (IOSurface ones), samplers,
  depth-stencil states, libraries (URL, file, data, default), functions
  (`newFunctionWithName:`, `...constantValues:error:`,
  `newFunctionWithDescriptor:error:`, the constants read through Metal's own
  `newIndexedConstantArray` / `newNamedConstantArray`), render pipeline states,
  command buffers (commit, handlers on a serial queue, status, error), render,
  blit and compute encoders. Every class but the device and the queue is made
  on NSObject, so nothing of Metal's private bases runs uninitialised. Draws
  and compute are recorded as not implemented: the command buffer ends in
  error. `supportsMemorylessRenderTargets` is NO; a few limits answered.
- `userspace/b2l-mtltest.c`: the test child's steps, each logged with ok or
  FAILED ("mtl M2: test:"): buffers zeroed and from bytes; a blit copy and
  fill; a 64 x 64 BGRA8 texture written and read back; an IOSurface texture;
  render passes clearing the IOSurface to (0.25, 0.5, 0.75, 1) and the texture
  to red, read back pixel by pixel; a texture copy; a sampler; QuartzCore's
  library; pipelines std_vert0 + attachment_clear and fixed_vert_lph_spc +
  fixed_frag_lph_cph with `fc_texture_function` = 0 given by name. The child
  may run 120 s.

Risk: the test's jobs share the GPU address space with backboardd's OpenGL ES
work; a job that faults stops it until the GPU is reset, which would freeze
the interface until a reboot.
