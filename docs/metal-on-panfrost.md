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

## M2a result (boot 68, build 67)

All 14 steps passed (log `logs/2026-11-21-userspace-boot68-build67`). The test
child ran at about 86 s, took 2 s and exited 0; the kernel freed its 16 buffer
objects with its client.

- `MTLCreateSystemDefaultDevice` gave `B2LMetalDevice` in 451 ms, opening
  libb2l-mesa and its Panfrost context included (25 ms); the only message
  without a method was again `-featureProfile`. `supportsMemorylessRenderTargets`
  is now NO.
- Buffers zeroed and from bytes; a blit copy and fill, 0 bytes wrong (0.9 ms).
- A 64 x 64 BGRA8 texture written and read back unchanged; an IOSurface (id
  19) imported as a texture.
- Render passes clearing the IOSurface and the texture: 0 of 4096 pixels wrong
  in each, read back through the IOSurface's own lock (1.1 ms for both, two
  fragment jobs); `copyFromTexture:toTexture:` 0 pixels wrong.
- A sampler; QuartzCore's library, 189 functions, in 2 ms.
- std_vert0_lph + attachment_clear_frag_lph: AIR 7 ms, Panfrost 3 ms.
- fixed_vert_lph_spc + fixed_frag_lph_cph with `fc_texture_function` = 0 given
  by name: AIR 316 ms (utility QoS), Panfrost 15 ms; the fragment shader is
  278 NIR instructions, the size boot 66 measured for that constant, so the
  constant given by name was applied; 10 fragment inputs linked to vertex
  outputs, none unmatched.
- No GPU fault, no crash before the shutdown to TWRP; the interface stayed on
  OpenGL ES throughout.

Next, M2b: draws (vertex buffers and bytes, constant buffers, textures and
samplers bound per stage, viewport and scissor, rasterizer state, depth-
stencil, blend colour, draw and indexed draw), checked by a triangle drawn
with std_vert0 into an IOSurface and read back.

## M2b as written (build 67 + 1)

- `b2l_mtl.h`: table version 2, the render encoder: pipeline, buffers (and
  offsets), bytes, textures and samplers per stage, viewport, scissor, cull,
  winding, fill, depth clip, depth-stencil state, stencil reference, blend
  colour, draws (vertex count or indices, instances, base vertex and
  instance).
- `b2l_mtl.c`:
  - A pipeline records what each stage reads, from its functions' interface:
    the UBO count, device buffers, textures (null textures), samplers; the
    shader's own constexpr samplers become sampler CSOs when it is made.
    Storage images and argument buffers are logged as not bound.
  - The encoder's state starts afresh with each pass (viewport and scissor the
    whole target, Metal's defaults). A draw binds the shaders, blend, the
    depth-stencil state (none: depth off), stencil reference and blend colour,
    a rasterizer (cached per state: scissor always on, top-left rule,
    half-pixel centre, first vertex provoking, depth 0 to 1, Metal's clockwise
    = Gallium's counter-clockwise on a y-down screen), the viewport with its y
    scale negated (Metal's NDC +y is up, row 0 the top), the vertex buffers the
    elements read (bytes through the context's upload buffer), UBOs, SSBOs,
    sampler views and samplers; a zero buffer, a zero 1 x 1 texture and a
    nearest sampler stand in for what the shader reads and nothing bound.
  - Indexed draws: the index bounds are computed from the buffer's mapping
    (no Panfrost index cache), restart at the all-ones index for strips.
  - After a command buffer that drew, nothing of it stays bound (Panfrost links
    the bound vertex and fragment shaders whenever either is bound again).
- `b2l-metal.c`: the render encoder's methods (with Core Animation's
  `setFragmentTexture:atTextureIndex:samplerState:atSamplerIndex:`); depth
  bias and visibility results are logged as not applied yet.
- `b2l-mtltest.c`: two draws read back pixel by pixel:
  - a triangle, std_vert0 + attachment_clear (which writes zero), vertices
    from a buffer and the matrix from `setVertexBytes`, NDC (-1, 1), (1, 1),
    (-1, -1) onto the IOSurface cleared blue: the top left half zero, the rest
    blue (checks the orientation);
  - a textured quad, image_rect_blit_vert + _frag (four vertices from
    `[[vertex_id]]`, the rectangle in buffer 1), a 2 x 2 texture at
    texture(3) with a nearest sampler: the left half of the target shows the
    four texels in quarters, the right half the clear colour.

### Added to the same build (so that one boot answers more)

- Compute: `compute_pipeline_new` (Panfrost's `create_compute_state`, which
  compiles there and then; the kernel's resources as for a render stage, the
  textures it writes as storage images), compute encoders (`dispatchThreadgroups`
  and `dispatchThreads` rounded up to whole threadgroups, threadgroup memory
  as Panfrost's variable shared memory, a barrier after each dispatch).
- Storage images in every stage (`set_shader_images`) for textures a shader
  writes.
- Texture views (`newTextureViewWithPixelFormat:` and the ranged forms): the
  parent's resource from a level and layer offset, another format of the same
  size per pixel; sampled, rendered to, copied, read back.
- What Core Animation's Metal renderer creates besides: events and shared
  events (signalled when a command buffer has run, listeners called on their
  queue, `waitUntilSignaledValue:timeoutMS:`), fences, heaps (each resource an
  allocation of its own, counted against the heap), residency sets (no-ops),
  an empty binary archive that accepts additions, the asynchronous pipeline
  creations, `newTextureWithBytesNoCopy:` (copied, as buffers are).
- The test child's further steps: an indexed draw; culling back faces keeps
  the clockwise triangle, culling front faces drops it (the winding mapping);
  a scissor; blending (a zero source alpha keeps the target); two triangles
  covering the target; `read_write_surf_compute` copying a 64 x 64 RGBA8
  texture; a shared event signalled by a command buffer with a listener; a
  heap and a buffer from it; a fence; an sRGB view cleared and read through
  its parent; and last, Core Animation's own Metal renderer: `CARenderer
  rendererWithMTLTexture:` drawing a red layer half covered by a green one
  into a texture of this device, in the test process only. The child may run
  180 s; each step is logged before the risky ones.
- Checked before the build: `userspace/b2l-metal.c` and `b2l-mtltest.c` pass
  clang 23 `-fsyntax-only -Wall -Wextra -Werror` against stand-ins for the
  SDK headers (two errors found and fixed); `b2l_mtl.c`'s Gallium calls and
  structures were read against Mesa 25.0.7's own headers.

QuartzCore's own selector references have no `drawPrimitives` or
`setVertexBytes`: Core Animation's draws reach the encoder another way, which
M3's log of unanswered messages will show.

## M2b result (boot 70, build 68)

27 of 31 checks passed, with no GPU fault. Right to the pixel: the triangle
into the IOSurface, the textured quad (`image_rect_blit`), the indexed
triangle, two triangles covering the target, the scissor, blending, the
compute kernel copying a texture (`read_write_surf_compute`, 384 threads),
the heap, the fence and the sRGB texture view. The uber pipeline took AIR
332 ms and Panfrost 202 ms.

Failed:

- **Culling, both checks**: culling back faces dropped the clockwise triangle
  and culling front faces kept it. With the negative-y viewport, Panfrost
  takes Metal's clockwise as Gallium's clockwise, so `front_ccw` is now set
  for counter-clockwise winding.
- **Shared-event listener**: the command buffer set the value to 5, but the
  block did not run within 1 s. The cause is unknown; the plug-in now logs
  the registration, the value reaching it, the dispatch and the block's run
  ("event listener:" lines).
- **CARenderer**: no command buffer reached the queue and the texture stayed
  zero. The test attached the layer after its transaction's commit, so the
  renderer had no committed tree. It now attaches it inside a transaction and
  flushes. Labelled empty buffers before and after the frame
  (`test: before/after CARenderer`) count what Core Animation commits.

## M2c as written (build 69 + 1)

- **Listener**: boot 71 showed the test's `MTLSharedEventListener` was nil,
  so the plug-in was never called. The test now says whether the class was
  found and tries `+sharedListener` and `-initWithDispatchQueue:`.
- **CARenderer**: boot 71 committed Core Animation's own command buffer
  (`com.apple.coreanimation.command-buffer`, two draws and a fragment job,
  23 ms) and the target still read zero. Every pipeline's colour format,
  blending and write mask and its vertex attributes are logged, and the
  encoders of Core Animation's command buffers log what they are told
  (`ca:` lines: the pass with its target, load/store and clear, pipelines,
  buffers, the first 64 bytes of every `setVertexBytes`/`setFragmentBytes`,
  textures, viewport, scissor, raster state, draws).
- **Back to TWRP by itself**: `b2l_recovery_after=480` restarts into
  recovery 8 minutes after start, synced as the volume-key chord does
  (B2LPower.cpp), so each test boot's log can be read without anyone at the
  phone. Holding both volume keys for 3 s still does it at once.

### Added after boot 72 (build 70 + 1)

Boot 72's trace: Core Animation's pass loads and stores our 64 x 64 BGRA8
target; its pipeline (`fixed_vert_lph_spc` + `fixed_frag_lph_cph`, no
blending, write mask 0xf; attributes Float4 at 0, Float2 at 16 and 24, Half4
at 32, stride 48, buffer 1) draws two indexed triangle pairs with the
matrix in vertex buffer 2 and uniforms in fragment buffer 1 (one 256 KiB
buffer), no bytes, no textures, and the target stays zero. Both shaders came
out exactly as the test's built with default constants (130 + 278 NIR
instructions), though the fragment shader declares about 60 function
constants (`fc_texture_function`, `fc_color_inputs0`, `fc_color_opcodes0`,
`fc_image_count`...) and the vertex shader's inputs and outputs depend on
five: so Core Animation's constants may not arrive. Logged now: every
function made (name, the constant-values class, each array's count, every
constant with its value, how many of the declared ones were supplied), the
contents of each buffer at its offset, the indices of each draw and buffer
offset changes. The listener test, Apple's `MTLSharedEventListener` being nil
whichever way, now calls our `notifyListener` without one. The `B2L CS`
lines (17,000 a session, which pushed everything else out of the log ring)
are kept for the first 64, then one in 1000 with the count.

### Boot 73 (build 71) and what was added after it

30 of 31 tests pass. The listener test now passes: with a nil listener our
`notifyListener` runs the block on the completion queue at value 5. The
`B2L CS` limit kept the log readable. CARenderer still leaves its target
zero, and the new lines narrow down why:

- Core Animation's constants do arrive: 5 to `fixed_vert_lph_spc` (all zero)
  and 60 to `fixed_frag_lph_cph` (`fc_attachment_count=1`,
  `fc_blend_function=0x3e`, ...). The line that would say how many of the 60
  were matched was cut at the log's 256 bytes, and the fragment shader is
  still 278 NIR instructions, as with the test's one constant. So whether
  the 60 are applied is not settled yet.
- The vertex shader's `buffer(2)` is a `float4x4` (the AIR metadata says so,
  64 bytes). At bind time Core Animation's vertex buffer 2 at offset 256
  and fragment buffer 1 at offset 0 hold only zeros. The vertices at
  offset 512 are already in place: pixel positions (0,0), (32,0), (32,64)
  with the colour as half4 (1,0,0,1), and so are the indices at 768
  (0 1 2 2 3 0). With a zero matrix every vertex lands on one point and
  nothing is drawn, which matches the zero target.
- No `newBufferWithBytesNoCopy`, blit or compute is involved. The only
  unimplemented calls were `-featureProfile` and the protection options.

Our command buffers run all their commands at `commit`. If Core Animation
filled the matrix only after binding, the GPU would still see it, so it is
either written somewhere else or not at all. Added to the next build:

- Each buffer a traced encoder binds (stage, index, last offset, including
  `setVertexBufferOffset:`) is read again at each draw and at `commit`: 64
  bytes, and 128 for the uniforms of buffer 2 and up.
- A function's summary (matched constants) comes first, then its constants
  on as many lines as they need.

### Boot 74 (build 72) and what was added after it

The reads at draw and at commit settle the geometry. Core Animation writes
its matrix after binding the buffer and before the draw, and at commit it is
right: column-major (1/32, 0, 0, 0), (0, -1/32, 0, 0), (0, 0, 0, 0),
(-1, 1, 0, 1), pixels 0..64 to NDC -1..1, y flipped. The vertices are right
too: red half4 for the first layer, green for the second. Its fragment
uniforms (`CA::OGL::Metal::Uniform`, 256 bytes in buffer 1) are still zero
at commit. All 60 fragment constants are matched. Among them,
`fc_framebuffer_fetch` is 1, `fc_attachment_count` 1 and
`fc_blend_function` 0x3e: pipeline blending is off, and the shader reads its
target (`dest_0`, an `air.render_target` input) and blends itself. The
shader's `air.static_init` gives `frg_has_color0 = (fc_generic &&
fc_attachment_count != 0) || fc_dest_index == 0` (true here) and
`has_dest_0` true for these constants. So this shader should differ from the
test's (fetch off), yet both came out at 278 NIR instructions, and the frame
is still zero.

Added to the next build:

- One line per fragment shader made into a pipeline. It gives the constants
  it was translated with (count, nonzero, a digest) and what came out: the
  outputs it writes, its output stores, framebuffer fetches (and whether
  `uses_fbfetch_output` is set), UBO loads and discards.
- Two new tests draw Core Animation's red layer by hand, without Core
  Animation: the same pipeline, all 60 constants typed as the AIR declares
  them, the same vertex layout, matrix, zero uniforms and indices, onto a
  target cleared blue. They read back whether the left half is red and the
  right half blue. One uses Core Animation's constants (fetch on, blend
  0x3e), the other turns fetch and blend off. If only the first fails, the
  framebuffer fetch is to blame. If both fail, the fixed-function shader
  itself is.

### Boot 75 (build 73) and what was added after it

Both draws of Core Animation's red layer by hand left their target
untouched, blue everywhere, with fetch on and with fetch off. The
fragment-shader lines show why the two cannot differ: whatever the
constants (the test's one, Core Animation's 60 with three nonzero, the
fetch-off set with one nonzero), `fixed_frag_lph_cph` comes out the same.
It writes outputs `0x70` (three colour targets) with 3 stores and 2
framebuffer fetches, and `uses_fbfetch_output` is set even with
`fc_framebuffer_fetch` 0. So the constants the `air.static_init`
constructor turns into program-scope variables (and into the
`__metal_implicit_fc_pred_*` predicates of the interface) do not specialise
the shader. The output equals the destination, as a blend with a
transparent source would give: blue over blue here, zero over zero in
Core Animation's frame.

Added to the next build:

- The whole NIR of the first fragment shader with a nonzero constant (the
  fetch-on draw by hand) and of its vertex shader, one log line per NIR
  line. With it, the per-shader line also counts scratch loads and stores:
  where the constructor's variables would sit if they are not folded.
- A third draw by hand: Core Animation's vertex shader and data with
  `attachment_clear_frag_lph`, which the other tests prove. A zero left
  half clears the vertex shader, and blue there puts it in question too.

### Boot 76 (build 74) and what was added after it

The vertex-shader check failed too: Core Animation's vertex shader with
`attachment_clear` (a fragment shader the other tests prove) left the
target blue. So nothing `fixed_vert_lph_spc` emits gets rasterised. The
fragment shader's specialisation (122 scratch stores, 2 or 4 scratch loads)
is a second problem behind this one. The dumped NIR of the vertex shader
reads correctly: the position is the four `buffer(2)` columns (UBO 2 at 0,
16, 32, 48) times the position input. Its other outputs are ten varyings,
two of them `f16vec4` colours, and a point size (PSIZ, written as 0 for
triangles). It also keeps 35 bytes of scratch for the constructor's
variables, read twice. No GPU fault was reported, and the vertex and tiler
jobs completed as for every other draw.

Added to the next build: six probes, each one change from that failing
draw, all with `attachment_clear`, so a zero left half means the
triangles were drawn:

- `std_vert0_lph`, a proven vertex shader, with Core Animation's buffer
  layout and data;
- the vertices in NDC with an identity matrix;
- the vertices in buffer 0;
- six vertices and no index buffer;
- `fixed_vert_lph_spc` without constant values;
- NDC, buffer 0 and no indices together.

### Boot 77 (build 75) and what was added after it

The probes found the side at fault. `std_vert0_lph` with Core Animation's
exact layout, matrix, vertices and indices drew right (left half zero).
`fixed_vert_lph_spc` drew nothing in every variant: NDC with identity,
buffer 0, no indices, no constant values, and all three together. So the
data and our draw path are right, and the shader as we translate it is the
problem.

Its dumped NIR shows what no proven vertex shader has. It has two
`[[point_size]]` outputs, `point_size_per` and `point_size_lin`: in the AIR
they are alternatives chosen by function constants, and since those are not
folded both remain, both at `VARYING_SLOT_PSIZ`. One of them is stored to
without being declared in the shader's variables (driver slot 1 is a gap
in the declared outputs). A store to an output the driver never placed can
land anywhere, including on the position.

`b2l_mtl.c` now fixes the outputs before Panfrost sees a shader. Stores to
undeclared outputs are dropped, and of several declared `[[point_size]]`
outputs only the first is kept (only one exists for any set of constants,
and Panfrost reads it only for points). It logs what it dropped. The
pipeline label (newly passed from the plug-in, appended to the descriptor
struct so an older library still reads the rest) turns this off with
`b2l:keep`, the tests' control, logged and not counted. `b2l:no-psiz`
removes every point size instead. The six probes run again with the fix on,
plus the control and the no-psiz case, and Core Animation's own frame runs
with the fix too.

### Boot 78 (build 76) and what was added after it

The output fixes work. With them, `fixed_vert_lph_spc` draws Core
Animation's data in every probe that keeps the vertices in buffer 1, and the
control without them (`b2l:keep`) still draws nothing. 37 of 42 tests
passed, against 31 of 40. Three things remain.

Core Animation's red layer comes out `00000000`: the triangles are drawn,
with a zero colour. Its fragment shader, as Panfrost gets it, is one block.
`air.static_init` stores the function constants as constants into a struct
in scratch (148 bytes), and four byte loads of that struct choose the
outputs. For colour 0 the choice is `color_per`, `color_lin`, or zero.
The stores before those loads make it `color_lin`, and the frame is what a
zero load gives. No pass forwarded a scratch store to a later load (the
frontend's field folding sees only values that are already constant while
it translates). `b2l_mtl.c` now does that before Panfrost: in the entry
block, in order, a scratch load whose bytes earlier stores set to constants
becomes those constants. In a later block it does the same for bytes that
no store outside the entry block touches. Then it folds, and with no scratch
load left it drops the stores and the scratch size. A fragment shader with
no framebuffer fetch left no longer asks for one. That was the other
leftover of the unfolded constants: fbfetch on even with
`fc_framebuffer_fetch` 0. It logs what it forwarded. The label
`b2l:raw-scratch` skips it, as the control.

`color_lin` is a 16-bit noperspective varying. Panfrost has no
noperspective interpolation of its own: it keys a vertex shader variant on
the fragment shader's noperspective inputs and corrects by w in both
stages, code written for GL, where such varyings are 32-bit. Such inputs
are now made 32-bit in both stages, converted around the store and the
load. The label `b2l:narrow` skips that.

With the vertices in buffer 0 nothing was drawn. The attribute the
descriptor does not describe (the vertex shader's `point_size`) read buffer
0 with stride 0, and Panfrost keeps one stride per vertex buffer, so every
vertex became the first. Undescribed attributes now read a slot of their
own, where the zero buffer is always bound.

The red layer runs again: with `fc_perspective` set (the smooth
`color_per`), and as controls without the scratch forwarding and without
the 32-bit varyings. Each can say which of the two was at fault. The
`CARenderer` frame was still all zero in boot 78, which fits the same
fragment shader.

### Boot 79 (build 77) and what was added after it

39 of 43 passed. The zero slot works: the probes with the vertices in
buffer 0 now draw. The scratch forwarding works too: every scratch load of
both shaders is forwarded, the scratch goes from 148 and 35 bytes to 0, and
the framebuffer fetch is no longer asked for. Core Animation's fragment
shader is now three stores: `color0 = color_lin`, and NaN to `color1` and
`color2`. Yet the red layer, and every variant of it (with
`fc_perspective`, and both controls), still came out `00000000`, which is
NaN as unorm8.

The pipeline has one colour attachment. Bifrost writes each colour output
with a `BLEND` instruction that reads the blend descriptor of its render
target from FAU (`BIR_FAU_BLEND_0 + rt`, `bifrost_compile.c`), and Panfrost
lays out descriptors only for the targets that are bound. So a store to
colour 1 or 2 does not land nowhere, and the last one written, NaN, is what
target 0 shows. In Metal a colour output with no attachment is simply not
written. `b2l_mtl.c` now removes such outputs and their stores before
Panfrost sees the shader, and logs the mask. The label `b2l:all-targets`
skips this, as the control. The `b2l:raw-scratch` and `b2l:narrow` controls
stay, and with this fix in place they now say whether those two fixes are
needed as well.

### Boot 80 (build 78) and what was added after it

The stores to the missing targets are gone (colours 1 and 2 of
`fixed_frag_lph_cph`, colour 1 of `attachment_clear` and `image_rect_blit`),
and the red layer is still `00000000` in every variant and control. With
the fragment shader down to one store of the vertex colour, the colour
itself has to be zero. No test so far carried a vertex attribute to a
fragment output: `image_rect_blit` has no attributes, and the probes'
`attachment_clear` ignores the colour.

Panfrost counts a vertex shader's attributes as the inputs it reads
(`pan_shader.c`: `attribute_count = util_bitcount64(inputs_read)`), and
`bifrost_compile.c` indexes them by the input's driver location. Gallium's
frontends number only the inputs a shader reads, so that fits. We numbered
every declared input. `fixed_vert_lph_spc` declares five (position, two
texture coordinates, colour, point size) and reads two, so its colour had
driver location 3, past the 2 attribute descriptors Panfrost emits. The
position, at location 0, was right, which is why the geometry always was.
`b2l_mtl.c` now removes the vertex inputs a shader does not read before
the driver locations are given, and the vertex elements follow them. The
label `b2l:all-inputs` skips this, as the control. The earlier controls stay
and now say which of the other fixes are still needed.

### Boot 81 (build 79): M2c done, and M3 as written

With only the inputs it reads numbered, Core Animation's vertex colour
reaches the fragment shader. The red layer with Core Animation's own
constants comes out red (`ffff0000`). `CARenderer`, Core Animation's own
Metal renderer driven from outside, draws its two layers: red
`ffff0000` and green `ff00ff00`. 41 of 43 passed. The `b2l:all-inputs`
control, which keeps the old numbering, is zero again. The two that failed
used `fc_blend_function` 0, which is not "copy" in Core Animation's
encoding: that draw is now a control. Most of the other controls ran with
blend 0 and said nothing. They now use Core Animation's own constants, so
they say whether the scratch forwarding, the 32-bit varyings and the
dropped stores are still needed.

M3 (`b2l_metal=2`): the plug-in shows the device to backboardd too, and
Core Animation's render server draws the lock screen and home screen with
Metal. `b2l_metal=3` shows it to every process. As the OpenGL ES
bring-up had one, there is a fallback. Each backboardd start counts itself
in `/private/var/tmp/b2l-metal-backboardd.starts`, and a backboardd started
again in the same boot (after a crash, or the watchdog's kill) is not shown
the device. Core Animation then stays on OpenGL ES. A marker older than the
boot counts for nothing. Command buffer errors after the first 32 are only
counted, and every 600 command buffers one line gives the count, the
errors and the GPU time.

### Boot 82 (build 80): M3 reached, and M4 as written

backboardd took the device on its first start and drew the lock screen and
home screen with Metal: no crash, no fallback to OpenGL ES, more than 600
command buffers with one error (`generateMipmapsForTexture:`, not
implemented), about 6.5 ms each. On the phone most things looked right,
but everything was much slower. The first frame came at about 200 s
(OpenGL ES, boot 81: 68 s). Core Animation asked for its generic fragment
shader, `fixed_frag_lph_cpf` with 25 of its 60 constants (`fc_generic=1`,
25,996 NIR instructions after the clean-up, framebuffer fetch on), in six
pipelines that differ only in their colour format (71, 81, 80, 43, 70,
115). Each translated the AIR again (1.4-2.3 s) and Panfrost compiled it
again (about 20 s); backboardd's CPU was busy for those two minutes. After
that, 52-151 frames reached the screen per 600 vsyncs (OpenGL ES: 103-165).
Opening Mesa took 12 s in backboardd, as for OpenGL ES at boot 81.
backboardd also refused 59 IOSurfaces whose rows are 480 bytes (the Mali
needs multiples of 64), so what they hold was not drawn.

The test program: 41 of 42 passed. The draw with `fc_perspective=1` still
comes out near zero. The controls: without the scratch forwarding, the
32-bit varyings or the vertex input numbering Core Animation's red layer
is wrong again, so all three stay. Without the dropped stores to colour
outputs with no attachment it is right, so that one is no longer needed
(it stays: it costs nothing).

M4, first step (speed, build 81):

- A translated function is kept by its bitcode and constants (SHA-1): the
  next `MTLFunction` with the same ones uses the same NIR. Core Animation
  makes a new `MTLFunction` for every pipeline.
- A Panfrost shader is kept by the SHA-1 of its final NIR (after the
  per-pipeline fixes): the next pipeline with the same NIR uses the same
  shader. Bifrost's fragment variants do not depend on the colour format,
  as the framebuffer fetch reads its conversion at draw time. So the six
  pipelines above need one compile, not six. Up to 128 shaders are kept,
  and a shader no pipeline holds makes room. The label `b2l:no-share` turns
  this off for a pipeline.
- An IOSurface whose rows the Mali cannot use gets a shadow, as in the EAGL
  layer since boot 55: storage of its own, filled from the surface. Every
  texture of the same surface shares it. A shadow nothing has drawn into is
  filled again when the next texture of it is made. It is not written back.
- `generateMipmapsForTexture:` through `util_gen_mipmap` (API version 3; a
  plug-in asking for 2 gets the same table). A texture of several levels is
  made a render target when its format allows it, so the blits can draw it.
- Diagnostics: a draw that takes 100 ms or more on the CPU (a shader variant
  made at the draw), and every 600 command buffers the time spent encoding
  and waiting for the GPU.
- Tests: `generateMipmapsForTexture:` (level 3 of a green texture), and a
  120 x 16 IOSurface with 480-byte rows cleared through one texture and read
  back through another.

Still to do for M4: a disk cache for the compiled shaders across boots,
which needs Mesa's `pan_disk_cache.c` adapted to Mach-O (it identifies the
driver by an ELF build-id), and executing command buffers off the caller's
thread. Today `commit` waits for the GPU before it returns.

### Boot 83 (build 81): one compile, and M4's second step

The shaders were shared. backboardd compiled Core Animation's generic
fragment shader once (21 s, done at 86 s after start). Every later pipeline
used it, which saved about 105 s of Panfrost compiling by 101 s. Shared
translations saved about 12 s more. The first frame reached the screen at
about 110 s (boot 82: 200 s; OpenGL ES at boot 81: 68 s). The panel then went
to sleep at 119 s (the lock screen's idle time) and was not woken, so this
boot says nothing about the frame rate. The test program: 43 of 44, with
the mipmap and 480-byte-row IOSurface tests passing; the perspective draw
still fails. backboardd asked one selector the plug-in did not have,
`-[MTLBuffer newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:]`,
and was answered nil.

Added for build 82:

- Panfrost's disk shader cache. The build enables Mesa's `shader-cache`
  (zlib is there). `patch_pan_disk_cache.py` names the driver by a SHA-1 of
  the Mesa release and every file under `scripts/mesa`, as a Mach-O library
  has no ELF build-id note. That build-id lookup is what upstream uses, and
  without the note a release build would read through a null pointer.
  `B2L_IDVS` joins the cache key, as it changes the programs. The cache
  lives in `/private/var/tmp/b2l-mesa-cache`, 256 MB at most. backboardd can
  write there (its Metal start marker is there too), and it outlives a boot.
  The log line `mesa: disk shader cache on/OFF` says which. The probe's
  screens still compile everything. The first boot fills the cache; from
  the second one, the 21 s compile should be a disk read.
- `newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:` (API
  version 4). The texture has storage of its own, as the Mali's linear rows
  need 64-byte strides and Panfrost cannot alias a buffer. It is filled from
  the buffer when made and before each command buffer that uses it, and
  written back after each one that draws, copies or computes into it. A
  test checks both directions.

### Boots 84 and 85 (build 82): the cache written but not found, and M4's third step

Both boots said `mesa: disk shader cache on ... (writable)`. Boot 84 filled
the cache: read back read-only from the phone, `mesa_shader_cache_db/part0`
held 174 KB of data and a 2,848-byte index. Boot 85, a plain restart, still
compiled the generic fragment shader (20.8 s). The cache works; the key does
not match. Panfrost keys a shader by the SHA-1 of its NIR, which is
evidently not the same from one boot to the next. Core Animation used the
textures on buffers at once (a 718 x 466 BGRA8 image, a8 and rg8 glyph
textures), which boot 83 answered nil. The test program: 44 of 45 (the new
linear-texture test passed). In 600 command buffers, encoding took 8.1-8.6 s
of CPU and waiting for the GPU 0.8-1.1 s: 57 draws and about 390 commands
each.

Added for build 83 (speed first, as asked after boot 85):

- A stage made for a keyed pipeline is named `b2l:` and the SHA-1 of what
  made it: the functions' bitcode and constants, and the pipeline's
  descriptor without Core Animation's label. `patch_pan_disk_cache.py` keys
  such a shader by that name, so the same shader is found at the next boot
  whatever its NIR's bytes. A long compile logs the first bytes of the NIR's
  SHA-1, to see whether it changes between boots. The disk cache is probed
  at each start: what the last start stored is read back, then this start's
  boot and process are stored.
- What the context holds is remembered between draws (`dev->bound`, which
  `unbind_all` forgets): shaders, blend, depth, rasterizer, stencil
  reference, blend colour, viewport, scissor, vertex elements and buffers,
  constant buffers (the same buffer and offset, or the same bytes), sampler
  views and samplers are set only when they differ. Rebinding a shader made
  Panfrost link the two stages again at every draw. The summary every 600
  command buffers counts the state calls made and skipped.
- A render pipeline is kept by its functions' keys and its descriptor, and
  the same request gets the same pipeline (256 kept): no clone, fixes,
  serialization and hashing again. Boot 84 made more than 150 of the same
  few functions.
- The Metal test child starts 200 s after backboardd instead of 60 s, so it
  no longer runs while backboardd makes its first frames.
- No `b2l_recovery_after` any more: the phone stays in iOS. Both volume keys
  held for 3 s sync the log and restart into TWRP.

### Boots 86 and 87 (build 83): a crash, white widgets, a cache that does not outlive a boot

From the user: no more visual glitches. Widgets come out white, as they
always have. Swiping the lock screen stops for a few seconds, and opening
Files crashed backboardd; its OpenGL ES fallback took over.

- The disk cache probe: the test child read back what backboardd stored in
  the same boot, but at the next boot nothing was there, and the generic
  shader was compiled again. Its NIR's SHA-1 was the same in both boots
  (`a0f2cd35`), so the key was not at fault: `/private/var/tmp` is emptied at
  each boot. The clock starts at 1970 at every boot, so a boot is now named
  by `kern.bootsessionuuid`.
- The swipe's stop: a new variant of `fixed_frag_lph_cph` took Panfrost 11 s
  at that moment.
- The crash (read back from the phone's crash reports, read-only): SIGSEGV in
  `pan_preload_fb_part` < `pan_preload_fb_v7` < `submit_batch` <
  `panfrost_flush`, from `cmdbuf_execute`'s flush. It came right after the
  first `generateMipmapsForTexture:` of 1179 x 2556 textures. The likely
  cause: drawing the levels made Panfrost lay the texture out anew while
  earlier work still named the old memory.
- Encoding: 9.7-14.1 s of CPU per 600 command buffers (42,000 draws), the
  state cache skipping about 16 calls a draw.
- Widgets: `WidgetRenderer_*` and `chronod` were never shown the device.

Added for build 84:

- `generateMipmapsForTexture:` submits and waits for what was recorded
  before it, and again after its blits.
- The disk cache lives in the first directory the process can make and
  write: `/private/var/mobile/Library/Caches/b2l-mesa-cache` (Core
  Animation's processes run as uid 501), then `/private/var/db`, then
  `/private/var/tmp`.
- `WidgetRenderer_*` and `chronod` are shown the device at `b2l_metal=2`.
- Textures on buffers are linear, and a command buffer copies the buffer in
  only when its bytes differ from the last copy.
- The 600-buffer summary splits the encoding time: draws, Panfrost's
  `draw_vbo` alone, and the textures on buffers (copied in, unchanged,
  written back).

### Boot 88 (build 84): the same crash, without mipmaps

The disk cache now lives in `/private/var/mobile/Library/Caches/b2l-mesa-cache`
and is writable. chronod and WidgetRenderer_Default were shown the device.
backboardd died again while the user swiped between home screen pages, at
the same place: `pan_preload_fb_part+0x4fc`, from `cmdbuf_execute`'s flush,
this time with no mipmaps before it. The faulting instruction (decoded from
the unstripped library) is a store, `str w8, [x0, #0x108]`. Panfrost was
packing a descriptor (stores at +0x0 to +0x10c) whose start lies 0xa0 bytes
before a 16 KB page boundary. The next page was unmapped (boot 87) or
protected (boot 88), so the write landed about 0x6c bytes past the end of a
CPU mapping.

Added for build 85:

- Every buffer object the CPU can map is made one 16 KB page larger than
  asked (if the kernel can give it), so such an overrun lands inside the
  object. `b2l_drm_mmap` logs a mapping shorter than its object.
- A colour attachment with `MTLLoadActionDontCare` is cleared to zero, not
  preloaded: undefined contents allow it. This also saves the preload's
  full-screen pass.
