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
