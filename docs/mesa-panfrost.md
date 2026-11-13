# GPU composition through Mesa Panfrost (plan)

Status 2026-11-13: Mesa 24.3.4 builds completely for iOS arm64e in CI
(`.github/workflows/mesa-panfrost.yml`, 214 of 214 targets): Panfrost for
Bifrost v7 (Mali-G76), its shader compiler, the GLES dispatch. Nothing runs
on the phone yet.

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
   `IOConnectMapMemory` address of the BO (one small patch to
   `src/util/os_mman.h`, applied by the build script).
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
