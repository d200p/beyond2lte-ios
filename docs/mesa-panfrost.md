# GPU composition through Mesa Panfrost (plan)

Status 2026-11-13: Mesa 24.3.4 builds completely for iOS arm64e in CI
(`.github/workflows/mesa-panfrost.yml`, 214 of 214 targets): Panfrost for
Bifrost v7 (Mali-G76), its shader compiler, the GLES dispatch. Nothing runs
on the phone yet.

Status 2026-11-14: pieces 1 to 3 written, not yet run.
- Kernel (B2LMali): client type 1 with `kB2LMaliProps`, `kB2LMaliBoCreate`,
  `kB2LMaliBoFree`, `kB2LMaliJobSubmit` (`b2l_mali_uapi.h`). The runtime now
  serves several clients at once (one drawing client, any number of
  buffer-object clients) in one address space; buffer objects live at
  0xa0000000-0xffffffff, at most 768 MiB resident, the tiler heap with 32 MiB
  of its range backed. Their memory is pageable, write-combined and wired
  while it exists, the way IOSurface makes write-combined memory, so neither
  the CPU nor the GPU side needs cache maintenance.
- `userspace/b2l-gpu.c` checks it at every boot from backboardd ("BO test"
  lines in the kernel log): properties, a mapped object, a fragment clear and
  a write-value job read back, a tiler heap, a shader object, cleanup.
- libdrm (`scripts/mesa/b2l-drm/b2l_drm.c`) and the `os_mmap` redirect
  (`patch_os_mman.py`), compiled by the Mesa workflow.

Status 2026-11-14 (later): boot 35 ran the kernel check ("BO test") and it
passed in full (properties, write-combined mapping, fragment clear of 256
pixels, write-value job, tiler heap, shader object, cleanup). Mesa moved to
25.0.7 (Panfrost needs no LLVM there, and the source can be read in full).
The Mesa workflow now builds `libb2l-mesa.dylib` (15.1 MB, 3.7 MB
compressed): `scripts/mesa/b2l-target` adds an OSMesa-style frontend
(Panfrost screen on our libdrm, OpenGL ES contexts on our own resources) and
`b2l_mesa_probe()` (an ES 3 context, a clear and a triangle read back). The
kernel workflow fetches it, `scripts/build-mesa-payload.py` compresses it into
the kernel, `bsd/kern/b2l_mesa_seed.h` publishes it at boot in
/usr/local/lib, and backboardd's library runs the probe once per boot.
Next: IOSurface import for buffer-object clients, then the EAGL layer.

Status 2026-11-14 (boots 38-40): Mesa runs on the phone ("Mali-G76
(Panfrost)", ES 3.1). Every job completes (status DONE, no MMU fault), but
the pictures are not all right, and the same way every time: with AFBC the
clear colour reads back as 0 while the triangle is drawn; without AFBC the
clear is right but the first triangle of a context is missing; every later
linear target is right. Transaction elimination (CRC) changes nothing, and
neither did a whole-address-space TLB flush on every buffer change. Boot 41
runs the plain case once more with `PAN_MESA_DEBUG=trace`: every job chain
decoded by pandecode to `/private/var/tmp/b2l-pandecode.ctx-*`, read from
TWRP with `tools/hfs_adb.py`, to compare a lost draw with a right one.

Boot 41 trace: the trace run fails exactly like the others (so not a race
with submission). A lost triangle and a right one have identical
descriptors (tiler heap, tiler context, framebuffer, attributes) and
identical shader code; only addresses differ. With AFBC the clear itself is
right (clear colour 0xffbf8040 in the render target descriptor); what is
lost is the draw of Mesa's readback blit, whose linear target stays 0. So
some draws run on memory the GPU does not see as the CPU wrote it. Boot 42
compares in one boot: plain, plain with every GPU cache cleaned and the
whole TLB dropped before each job (`B2L_MALI_REQ_FLUSH`), Mesa's buffer
cache off (`nocache`), and noafbc with the flush.

Boot 42: all four identical, so neither stale GPU caches or TLB entries nor
buffer reuse explain it. The lost draws are blit 1 (vertices at the very
start of a fresh 1 MiB upload buffer; blit 2 reads them 128 bytes further on
and works) and the first linear triangle; blit 1's tiler context is left
untouched by the GPU (state word 4 stays 0, 1 for every other draw). Boot 43
dumps every CPU-mapped buffer after each submit (`B2L_DRM_DUMP`, libdrm) in
a traced plain run, and draws a lost triangle once more to see whether a
repeat succeeds.

Boot 43: the repeats settle it. Without AFBC the lost first triangle comes
out right when drawn again; the lost first linear triangle comes out as a
full red screen, the shape of the previous blit's quad. Every IDVS draw
tiles the positions its position shader found in its position buffer
(0xa812e000/0xa812e040, the same each batch) before it ran: zeros in a
fresh buffer, the previous draw's positions after. The position shader's
stores are not seen by the tiler inside one IDVS job. Boot 44 compares
plain with IDVS against plain and noafbc with IDVS off (`B2L_NOIDVS`,
`scripts/mesa/b2l-drm/patch_pan_shader_idvs.py`: separate vertex and tiler
jobs), and logs JM_CONFIG (IDVS group size), SHADER_CONFIG and TILER_CONFIG
at the first job.

Boot 44: with IDVS off every probe target is right, with AFBC and without
(clears, triangles, linear targets, readback blits); with IDVS the same
losses as before. JM_CONFIG read 0xf0000 (IDVS group size 0xf, as the
Android driver sets it), SHADER_CONFIG 0x20000 (TLS hashing), TILER_CONFIG
0: the configuration is not at fault, IDVS just does not work on this
G76 r0p0. Our Mesa now compiles vertex shaders without IDVS unless
`B2L_IDVS` is set.

Build 44, IOSurface import (piece 4's first half): `kB2LMaliBoImport` maps
an IOSurface's pages at a fresh buffer-object address and keeps the surface
until the handle is freed; imported surfaces are cleaned and invalidated in
the CPU caches around every job, since IOSurface memory is cacheable. In
libdrm, `b2l_drm_surface_fd(id, bytes)` makes the "dma-buf" Mesa wants (an
unlinked sparse file whose size lseek finds) and `drmPrimeFDToHandle` turns
it into that buffer object, returning the same handle while it is open as
DRM does; CPU mappings of an imported surface keep its cacheable mode. The
probe draws a clear and the triangle into a 64 x 64 BGRA IOSurface through
`resource_from_handle` and checks it through `IOSurfaceLock`. Next: the EAGL
layer.

Status 2026-11-15, the window server on GLES (piece 4, written, not yet run).
The 23G90 window server has no OpenGL ES branch: `IOMFBDisplay::get_ctx`
calls `new_metal_context` directly and, when that fails,
`Server::render_update`/`render_surface` take `Server::sw_renderer()`, which
makes a `CA::OGL::SWContext` (server+0xc0, the same allocation as
`sw_new_context`) and a `CA::OGL::Renderer` around it (server+0xc8).
`GLESContext`'s constructor is reached only from `gles_new_context` (config
`{u32 type = 3; id eagl at +8}`, in the callbacks table at 0x1ee989640 with
the software, null and Metal ones, which QuartzCore's own code never reads)
and from `-[CARenderer _initWithEAGLContext:options:]`. So injecting Mesa
and an EAGL layer is not enough on iOS 26, unlike ios-sailfish on iOS 16:
`userspace/b2l-gpu.c` now replaces the four instructions that begin the
SWContext allocation in `sw_renderer` with a jump to its own stub, which
returns `gles_new_context({3, our EAGL object})` or, on any failure, the
software context as before (`B2L_GLES=0` leaves `sw_renderer` alone). Only the
first backboardd of a boot tries (`/private/var/tmp/b2l-gles.running`): one
that dies leaves the next on the CPU.

What `GLESContext` asks of the EAGL object (selectors sent from its methods):
`API` (above 2 its ES 3 paths), `getMacroContextPrivate`,
`attachImage:toCoreSurface:invertedRender:` (`make_buffer_current`: a 2D
texture, nearest filtering, clamp, the display's IOSurface attached, then a
framebuffer with it as `COLOR_ATTACHMENT0`) and
`texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:`.
It never sends `setCurrentContext:` (only `-[CARenderer render]` does): it
passes the GLIContext as the first argument of every dispatch entry, so the
EAGL layer makes its context current inside each entry. The constructor asks
`get_string` for `GL_EXTENSIONS` and 0x8a1e and `get_integerv` for
`MAX_TEXTURE_IMAGE_UNITS`, `MAX_TEXTURE_SIZE` and `MAX_VIEWPORT_DIMS`.

`scripts/mesa/b2l-target/b2l_eagl.c`: `B2LEAGLContext`, an `NSObject`
subclass made with the Objective-C runtime (missing selectors are logged by
`+resolveInstanceMethod:`); an OpenGL ES 3.0 context with no window-system
buffers; the GLI table of 975 entries, generated at build time by
`gen-gli-dispatch.py` from the runner's macOS SDK `gliDispatch.h` (the
layout QuartzCore uses; the header is never copied into the repository), each
entry a thunk to the Mesa function of the same name (core name first, so
`bind_framebuffer_EXT` is `glBindFramebuffer`). Entries of its own: the ARB
shader-object calls (`create_shader_object_ARB` and the rest, which ES names
differently; handles are pointers), `get_string`, `tex_image2D` (Apple's
`GL_RGBA` internal format with `GL_BGRA` data becomes ES's `GL_BGRA`),
`finish_object_APPLE` (glFinish), the debug markers. IOSurfaces are imported
as in the probe (rows must be multiples of 64 bytes for the Mali), kept in a
48-entry cache, and given to the bound texture with
`glEGLImageTargetTexture2DOES` through the screen's `get_egl_image`. Every
first use of an entry, the GL errors of the first 4000 calls, entries Mesa
lacks, imports and a 5 s summary go to the kernel log as `eagl:` lines.

Boot 46 (build 45): the window server's renderer was a `GLESContext` on
Mesa ("Mali-G76 (Panfrost)", OpenGL ES 3.1, context in 3.8 s, 897 of 975
GLI entries on Mesa functions, `get_string 0x8a1e` answered ""), no GL error
in its first 16 calls, and the display's IOSurface (1179 x 2556 BGRA, 4864
bytes a row) imported and attached as a texture with its framebuffer. Then
backboardd aborted in `CA::OGL::Context::push_surface` (ogl-context.cpp:1186,
`found != slist->end ()`) called from `GLESContext::make_buffer_current`; the
next backboardd of the boot found the marker and stayed on the CPU, as meant.
In 23G90 `GLESContext::make_buffer_current` and
`create_surface_from_iosurface` never call `Context::track_surface`
(0x1840d8c58), unlike its `create_null_surface` and
`create_surface_with_properties` and `MetalContext::get_surface`; the same gap
in the software context is closed by `scripts/patch-ca-software-surface.py`.
`b2l-gpu.c` now also hooks `push_surface`'s entry: for our GLES context, a
surface that is not on the context's list is put there with `track_surface`
first.

Boot 47 (build 46): past `push_surface`, Core Animation drew on the Mali for
about 80 s: shaders compiled and linked, `draw_elements`, `clear`,
`tex_image2D`, both display buffers attached, no GL error, no missing entry
or selector. The panel stayed black (no Apple logo), and no `flush`, `finish`
or fence was ever called. Then backboardd died with EXC_ARM_DA_ALIGN in
libSystem's memset, reached from `gli_tex_image2D` -> `_mesa_TexImage2D` ->
`st_TexImage` -> `st_texture_create` -> Panfrost's
`panfrost_resource_init_afbc_headers`, which zeroes a new texture's AFBC
headers through the write-combined mapping; libSystem clears large ranges with
DC ZVA, which faults there (frames read with the unstripped library from the
Mesa workflow's `b2l-mesa-symbols` artifact; the published library was
rebuilt bit for bit). Fixes: `b2l-target/b2l_memset.c` gives everything in
libb2l-mesa a memset/bzero with plain stores only, and `b2l-gpu.c` hooks
`IOMFBDisplay::fb_swap_set_layer` so that Mesa finishes the frame
(`b2l_eagl_flush`, glFinish) before a display buffer goes to the framebuffer.

Boot 48 (build 47): the Apple logo appeared, drawn by the Mali (four swaps,
glFinish 0 to 10 ms each), then backboardd died again, this time inside
Panfrost: `render_solid_background` -> `fill_rect` -> `GLContext::draw_elements`
-> `_mesa_DrawElementsBaseVertex` -> `panfrost_draw_vbo` ->
`panfrost_update_shader_state`, on `stur xzr, [x0, #0x14]`, a 64-bit store at a
4-byte boundary into a descriptor in a buffer object (two 32-bit words the
compiler merged). With the DC ZVA fault before, this says the CPU mappings of
buffer objects act as device memory, where every access must be aligned to
its size. Mesa is now built with `-mstrict-align`, and `b2l_memset.c` also
gives libb2l-mesa memcpy/memmove that only make aligned accesses on both
sides.

Boot 49 (build 48): the first boot whose whole UI was composed on the Mali.
backboardd stayed up for minutes (over 140,000 GL calls, about 10 swaps a
second, glFinish 10 to 20 ms a frame, no GL error but one, every logged job
done, no MMU fault). Defects (photos in the boot's log folder): some quads
lost a triangle or had one stretched to a wrong point, flickering from frame
to frame; two small layer surfaces with 32-byte rows were refused (the Mali
samples in place only rows of 64-byte multiples); later backboardd died in
`CA::OGL::Context::end_rendering` (not yet understood). The triangles look
like boots 41-43's IDVS failure, intermittent now: the tiler using positions
not yet visible from the vertex job it depends on. Panfrost sets the job
barrier only on jobs with memory side effects; `patch_pan_jm_barrier.py` now
sets it on every tiler job (`B2L_NO_TILER_BARRIER=1` undoes it). Surfaces the
Mali cannot sample in place are now copied into the texture with
glTexImage2D.

Boot 50 (build 49): the copies worked (6 x 90, 6 x 6, 256 x 1), the barrier did
not change the triangles (and the battery icon was now wrong too). Core
Animation redraws about 10 times a second even with nothing moving, and every
frame comes out different: more like intermediate surfaces left with pieces
of other draws than like wrong vertices. backboardd then aborted in
`push_surface` again: a display surface Core Animation had taken off the list
came back with its link still set, which the hook left alone; it now clears a
stale link before `track_surface`. Test for boot 51: `b2l-gpu.c` sets
`PAN_MESA_DEBUG=noafbc,nocrc` (no AFBC, no transaction elimination) before
the Mesa screen is made (`B2L_GLES_PAN_DEBUG` at build time).

Boot 51 (build 50): no change with AFBC and CRC off, so neither is behind the
triangles. backboardd later died after a display off/on: a store into pool
memory from `pan_preload_fb_part` (reached from a batch flushed inside a draw)
hit an unmapped page, the allocation having started 160 bytes before the end
of a mapped page; not understood yet. Boot 52 tests 16-bit arithmetic
(`PAN_MESA_DEBUG=nofp16`: Panfrost runs mediump in fp16, which over a
1179 x 2556 screen could throw single vertices far off), and the EAGL layer
logs Core Animation's first shaders, vertex array setups and indexed draws.

Boot 52 (build 51): no change without fp16. The new log lines show what Core
Animation draws: every draw is one quad, `GL_TRIANGLES` with the indices
0,1,2, 2,3,0 from client memory, its vertices interleaved in client memory
(48 bytes: position as 4 floats at +0, two texture coordinates as 2 floats
at +16 and +24, colour as 4 half floats at +32), rewritten for the next quad
right after the draw. The broken quads lose or stretch exactly the second
triangle, the only one that uses vertex 3, the highest index: as if the last
vertex of the shaded range were not shaded and its position came from an
earlier draw (Mesa's own range arithmetic, `max - min + 1`, is right). Test
and workaround for boot 53: the EAGL layer expands each indexed draw on the
CPU into a non-indexed one, vertices in index order plus one padding vertex
the primitives do not use (`B2L_EAGL_EXPAND=0` keeps indexed draws). The
library also logs its load address, for crash backtraces (boot 52 died in
`capture_backdrop` with two frames inside our libraries).

Boot 53 (build 52): every Core Animation draw was expanded (6952, none kept)
and the quads still broke the same way, so neither the indices nor the copy
of the vertices is the cause; expansion is off again (`B2L_EAGL_EXPAND=1`
turns it on). The kernel side matches the Android driver (memory attributes,
cache clean and invalidate at the start and end of every job, TLB flush when
the cores power up), and our `memcpy` is right for every alignment. For boot
54 the EAGL layer runs a self-test when the context is made: a grid of 8 x 8
quads drawn off screen with Core Animation's own vertex shader and vertex
layout, one quad per indexed draw as Core Animation does, as non-indexed
draws, as two draws per quad, as one draw for all, with the triangle that
uses vertex 3 first, and with scissor and blending on; plus the texture
coordinates as colour. The pixels are read back and the middle of every
triangle checked, three runs per way (`B2L_EAGL_SELFTEST=0` skips it).

Boot 53 also explained the crash after the screen went off and on (boots 49,
51, 53): the push_surface hook left the display surfaces on the context's
surface list, which Core Animation's GLES context never does itself, so when
the display surfaces were replaced the freed ones stayed listed, and
`CA::OGL::Context::end_rendering`, which logs every surface still on that
list at the end of each frame, followed a freed one. The hook now lists a
display surface only for `push_surface`'s own check and takes it off right
after.

Boot 54 (build 53): no crash through two screen off/on cycles (the display
surfaces come off the list after each `push_surface`). The self-test: the
first run of each new way of drawing lost nearly every quad (63 of 64,
nothing but the clear colour), the two runs after it were right, and so were
one draw for all and every later way; the job chains of the lost and the
right runs sat at the same addresses, every job DONE. The GPU used what the
previous run had left at the vertex shader's output addresses: identical
when the same quads are drawn again, wrong when they change, as Core
Animation's flickering triangles. Boot 43 saw the same within IDVS jobs. The
GPU page table mapped every page non-shareable, where Linux maps Panfrost
buffers inner shareable (with memory attribute 0x88, `SH_MIDGARD_INNER`: the
Mali's internal domain, coherent through the L2); non-shareable write-back
data from the shader cores was not coherent with the tiler's reads within a
job chain. Build 54 maps pages inner shareable (`B2L_MALI_PT_PAGE`); the
self-test checks it.

Boot 55 (build 54): the triangles are fixed (self-test: 0 wrong in all 24
runs, the user saw no more glitches). Left: (1) opening Control Center,
backboardd took an alignment fault in `st_draw_quad` (a partial clear
drawn as a quad): `stp s11, s9` wrote two aligned 32-bit words across a
page boundary of an upload buffer, which device memory does not allow; the
build now turns off LLVM's load/store pair pass (`-mllvm
-aarch64-enable-ldst-opt=false`). (2) The clock and calendar icons,
Settings' icons and the lock screen time were missing: Core Animation's
own render targets with rows that are not multiples of 64 bytes failed to
attach and had no storage; they now get a shadow texture (see
`shadow_attach`). (3) `use_program` failed a dozen times: link failures are
now logged with their compile logs. (4) 3-4 frames a second with the GPU
busy a fifth of the time: the 5-second summary now gives the CPU time spent
inside Mesa (and the slowest entries) and in the IOSurface paths.

Boot 56 (build 55): Control Center opens (no fault), but the icons and the
lock screen time were still missing and Control Center showed only the
battery. The shadow textures were made (every attach ok); the new link log
named the cause: several of Core Animation's fragment shaders use `s0`
without declaring it (`'s0' undeclared`), the first right after an 'LA08'
surface (grey and alpha glyphs, tinted by Core Animation): its OpenGL ES
shader generator writes no sampling line for such textures. The EAGL layer
now adds the two lines every other shader has (`uniform lowp sampler2D
textureN;`, `lowp vec4 sN = texture2D(textureN, texcoordN_0);`) to a
fragment shader that uses sN undeclared. Speed: with the screen still,
~10 frames a second, 40-60 ms per 5 s inside Mesa and 25-30 ms of GPU wait
per frame; while interacting, shader compiles (also inside draws, for
Panfrost's variants), texture uploads and partial clears take 0.8-2.3 s per
5 s. The GPU wait includes the kernel cleaning every imported IOSurface
whole before and after each job (two 12 MB display buffers among them):
before a job only surfaces whose seed moved are cleaned now, and the kernel
logs run and cleaning times every 500 jobs.

Boot 57 (build 56): the patched shaders compile (seven fixed, no link
failure, no GL error), but the icons, the lock screen time and Control
Center's controls are still blank, so the cause is elsewhere. The kernel
times: every 500 jobs the GPU ran 70-130 ms while the after-job cleaning
took about 1.3 s (12 GB, 24 MB a job: the display buffers); the before-job
pass is down to 0 ms. Now the GPU driver only marks the imported surfaces
after a job (`b2l_surface_gpu_wrote`), and a CPU reader drops the stale
lines when it reads: a client's `IOSurfaceLock` (`trapLock`) and the
framebuffer's CPU copies (`b2l_surface_cpu_access`).

In software mode (the user, after boot 57) the lock screen time, Control
Center and Settings' icons were drawn; the clock and calendar icons were
missing there too, so those two are not a GPU matter (chronod's 'xf20'
surfaces, refused by our IOSurface driver, are the first suspect). Core
Animation draws no copies or readbacks; what software mode computes on the
CPU and the GPU path does in shaders is vibrancy and the blend modes, with
`GL_EXT_shader_framebuffer_fetch`. The self-test now checks framebuffer
fetch on a texture and on an imported IOSurface, in the same batch and in
the next one (tile buffer preloaded from memory).

## Why

Core Animation in iOS 26 still contains its OpenGL ES renderer
(`CA::OGL::GLESContext`, `gles_new_context`) and falls back Metal -> OpenGL
ES -> software ("Failed to initialize Metal or OpenGLES contexts!"). With no
Apple GPU, Metal fails; OpenGL ES fails only because Apple's OpenGLES
framework sits on Metal. joranv1/ios-sailfish showed the route on iOS 16:
inject Mesa plus an EAGL shim into backboardd with DYLD_INSERT_LIBRARIES and
Core Animation composes on the GPU (20-40 fps on a 2016 phone). The S10's
software renderer reaches about 10-15 fps.

## Pieces

1. **Kernel, B2LMali buffer objects** (new user-client selectors):
   - `GET_PARAM`: GPU_ID, revision, shader/tiler/L2 present, texture and
     thread features, as the Linux panfrost GET_PARAM list;
   - `CREATE_BO` / `FREE_BO`: wired kernel memory, a GPU virtual address from
     a dedicated range, flags NOEXEC/HEAP; the user maps it with
     `IOConnectMapMemory` (memory type = BO handle);
   - `SUBMIT`: one job chain at a GPU address on slot 0 (fragment) or 1
     (vertex/tiler/compute), JS_CONFIG as today, completion awaited in the
     kernel (synchronous first, so every sync object is signaled on return);
   - `WAIT_BO`: immediate while submission is synchronous.
   The existing runtime (page tables, AS0, power, clocks, TLB flush on power
   up) stays.
2. **libdrm for B2LMali** (`scripts/mesa/b2l-drm/`): `drmIoctl` turns the
   `DRM_IOCTL_PANFROST_*` calls Mesa makes (CREATE_BO, MMAP_BO,
   GET_BO_OFFSET, GET_PARAM, SUBMIT, WAIT_BO, MADVISE) into those selectors;
   sync objects are user-space flags; `drmGetVersion` names the driver
   "panfrost"; a fake DRM fd identifies the connection.
3. **Mesa patch, buffer mapping**: `os_mmap` on the fake fd returns the
   `IOConnectMapMemory` address of the BO (macros appended to
   `src/util/os_mman.h` by `scripts/mesa/b2l-drm/patch_os_mman.py`).
4. **EAGL shim**: the `EAGLContext` / renderbuffer / IOSurface entry points
   that `CA::OGL::GLESContext` uses, on a Gallium frontend that creates a
   Panfrost screen and GLES contexts, with IOSurfaces (CA layers, the
   display surface) imported as BOs through `mapRange`.
5. **Packaging**: one dylib (Mesa static libraries + libdrm + EAGL shim),
   too large for the BOOT image: installed in the root, injected into
   backboardd like today's GPU library.

## Order

1 and 2 first, checked with a small test program (a clear and a triangle
into a BO, read back on the CPU) before Core Animation is involved; then 3,
then 4 against Core Animation, then 5.

## Licences

Mesa: MIT (kept with its notices). The libdrm interface in
`scripts/mesa/include/xf86drm.h` is newly written from libdrm's public API.
ios-sailfish has no clear licence: its approach is followed, no code is
copied.
