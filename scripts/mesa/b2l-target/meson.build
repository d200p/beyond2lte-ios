# SPDX-License-Identifier: MIT
# S10 port: libb2l-mesa.dylib, Mesa for iOS over B2LMali. Copied into
# src/gallium/targets/b2l by scripts/mesa/build-panfrost-ios.sh, with the
# B2LMali libdrm (b2l_drm.c) and the kernel interface header beside it.
# Linked like the OSMesa target: the GL state tracker, gallium, Panfrost and
# its winsys, glapi; nothing exported but the b2l_* entry points.

libb2l_mesa = shared_library(
  'b2l-mesa',
  [files('b2l_mesa.c', 'b2l_drm.c'), main_dispatch_h],
  gnu_symbol_visibility : 'hidden',
  include_directories : [
    inc_include, inc_src, inc_gallium, inc_gallium_aux, inc_gallium_winsys,
    inc_gallium_drivers, inc_mapi, inc_mesa, include_directories('.'),
  ],
  c_args : ['-DGALLIUM_PANFROST'],
  link_whole : [libglapi_static],
  link_with : [libmesa, libgallium, libglapi],
  link_args : ['-framework', 'IOKit', '-framework', 'CoreFoundation'],
  dependencies : [driver_panfrost, idep_mesautil, dep_thread],
  install : false,
)
