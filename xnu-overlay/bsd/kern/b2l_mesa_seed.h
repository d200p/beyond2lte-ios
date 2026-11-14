/* SPDX-License-Identifier: BSD-2-Clause
 * S10 port: Mesa for iOS over B2LMali (libb2l-mesa.dylib, scripts/mesa),
 * zlib-compressed in the kernel by scripts/build-mesa-payload.py and
 * published at boot like the GPU library: its name carries its hash, an
 * existing file must match it exactly, a new one is written to a staging
 * name and linked in place. Larger than the 4 MiB the small publisher takes:
 * 1 MiB writes, one fsync, and comparison in 1 MiB reads. Included by
 * b2l_userspace_seed.h after the GPU library; boot argument b2l_mesa=0 skips
 * it. A failure leaves backboardd without Mesa and nothing else affected.
 */
#include "b2l_mesa_payload.h"
#ifndef B2L_HAS_MESA_PAYLOAD
#define B2L_HAS_MESA_PAYLOAD 0
#endif
#if B2L_HAS_MESA_PAYLOAD
#include <kern/kalloc.h>
#include <libkern/zlib.h>

#define B2L_MESA_CHUNK (1u << 20)

static voidpf
b2l_mesa_zalloc(voidpf opaque, uInt items, uInt size)
{
    (void)opaque;
    size_t bytes = (size_t)items * size + sizeof(size_t);
    size_t *block = kalloc_data(bytes, Z_WAITOK);
    if (block == NULL) {
        return Z_NULL;
    }
    *block = bytes;
    return block + 1;
}

static void
b2l_mesa_zfree(voidpf opaque, voidpf address)
{
    (void)opaque;
    if (address != NULL) {
        size_t *block = (size_t *)address - 1;
        kfree_data(block, *block);
    }
}

static int
b2l_mesa_inflate(unsigned char **out)
{
    unsigned char *data = kalloc_data(B2L_MESA_BYTES, Z_WAITOK);
    if (data == NULL) {
        return ENOMEM;
    }
    z_stream z;
    bzero(&z, sizeof(z));
    z.zalloc = b2l_mesa_zalloc;
    z.zfree = b2l_mesa_zfree;
    z.next_in = (Bytef *)(uintptr_t)b2l_mesa_compressed;
    z.avail_in = (uInt)sizeof(b2l_mesa_compressed);
    z.next_out = data;
    z.avail_out = B2L_MESA_BYTES;
    int ret = inflateInit(&z);
    if (ret == Z_OK) {
        ret = inflate(&z, Z_FINISH);
        inflateEnd(&z);
    }
    if (ret != Z_STREAM_END || z.total_out != B2L_MESA_BYTES) {
        kfree_data(data, B2L_MESA_BYTES);
        printf("B2L MESA: inflate failed ret=%d out=%lu\n", ret, (unsigned long)z.total_out);
        return EIO;
    }
    *out = data;
    return 0;
}

/* Root-owned, mode 0755, one link, exactly these bytes. */
static int
b2l_mesa_compare(vnode_t vp, const unsigned char *data, size_t bytes, vfs_context_t ctx)
{
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_uid);
    VATTR_WANTED(&va, va_mode);
    VATTR_WANTED(&va, va_nlink);
    VATTR_WANTED(&va, va_data_size);
    int error = root_type(vp, VREG) ? vnode_getattr(vp, &va, ctx) : EINVAL;
    if (error) {
        return error;
    }
    if (!VATTR_IS_SUPPORTED(&va, va_uid) || !VATTR_IS_SUPPORTED(&va, va_mode) ||
        !VATTR_IS_SUPPORTED(&va, va_nlink) || !VATTR_IS_SUPPORTED(&va, va_data_size) ||
        va.va_uid || (va.va_mode & 0777) != 0755 || va.va_nlink < 1 || va.va_nlink > 2 ||
        va.va_data_size != (off_t)bytes) {
        return EINVAL;
    }
    unsigned char *check = kalloc_data(B2L_MESA_CHUNK, Z_WAITOK);
    if (check == NULL) {
        return ENOMEM;
    }
    for (size_t offset = 0; !error && offset < bytes;) {
        size_t n = bytes - offset < B2L_MESA_CHUNK ? bytes - offset : B2L_MESA_CHUNK;
        int resid = 0;
        error = vn_rdwr(UIO_READ, vp, (caddr_t)check, (int)n, (off_t)offset, UIO_SYSSPACE, 0,
            kauth_cred_get(), &resid, current_proc());
        if (!error && (resid || memcmp(check, data + offset, n) != 0)) {
            error = EIO;
        }
        offset += n;
    }
    kfree_data(check, B2L_MESA_CHUNK);
    return error;
}

static int
b2l_mesa_write(const unsigned char *data, size_t bytes, vfs_context_t ctx)
{
    vnode_t vp = NULL;
    int error = vnode_lookup(B2L_MESA_LIBRARY, VNODE_LOOKUP_NOFOLLOW_ANY, &vp, ctx);
    if (!error) {
        error = b2l_mesa_compare(vp, data, bytes, ctx);
        vnode_put(vp);
        return error;
    }
    if (error != ENOENT) {
        return error;
    }
    error = vnode_open(B2L_MESA_STAGING, FREAD | FWRITE | O_CREAT | O_TRUNC, 0755, VNODE_LOOKUP_NOFOLLOW_ANY, &vp, ctx);
    if (error) {
        return error;
    }
    for (size_t offset = 0; !error && offset < bytes;) {
        size_t n = bytes - offset < B2L_MESA_CHUNK ? bytes - offset : B2L_MESA_CHUNK;
        int resid = 0;
        error = vn_rdwr(UIO_WRITE, vp, (caddr_t)(uintptr_t)(data + offset), (int)n, (off_t)offset, UIO_SYSSPACE, 0,
            kauth_cred_get(), &resid, current_proc());
        if (!error && resid) {
            error = EIO;
        }
        offset += n;
    }
    if (!error) {
        error = VNOP_FSYNC(vp, MNT_WAIT, ctx);
    }
    if (!error) {
        error = b2l_mesa_compare(vp, data, bytes, ctx);
    }
    if (!error) {
        struct nameidata nd;
        NDINIT(&nd, CREATE, OP_LINK, LOCKPARENT, UIO_SYSSPACE, CAST_USER_ADDR_T(B2L_MESA_LIBRARY), ctx);
        nd.ni_flag |= NAMEI_NOFOLLOW_ANY;
        error = namei(&nd);
        if (!error) {
            if (nd.ni_vp) {
                error = EEXIST;
            } else if (!root_type(nd.ni_dvp, VDIR)) {
                error = EXDEV;
            } else {
                error = VNOP_LINK(vp, nd.ni_dvp, &nd.ni_cnd, ctx);
                if (!error) {
                    error = VNOP_FSYNC(nd.ni_dvp, MNT_WAIT, ctx);
                }
            }
            lookup_done(&nd);
        }
    }
    int close_error = vnode_close(vp, FREAD | FWRITE, ctx);
    return error ? error : close_error;
}

static void
b2l_mesa_publish(vfs_context_t ctx)
{
    int enabled = 1;
    PE_parse_boot_argn("b2l_mesa", &enabled, sizeof(enabled));
    if (enabled != 1) {
        return;
    }
    if (!b2l_amfi_active() || !rootvnode || vfs_isrdonly(vnode_mount(rootvnode)) ||
        strcmp(vfs_statfs(vnode_mount(rootvnode))->f_fstypename, "hfs")) {
        printf("B2L MESA: library not published, root not a writable HFS volume\n");
        return;
    }
    unsigned char *data = NULL;
    int error = b2l_apps_directory("/usr/local", ctx);
    if (!error) {
        error = b2l_apps_directory("/usr/local/lib", ctx);
    }
    if (!error) {
        error = b2l_mesa_inflate(&data);
    }
    if (!error) {
        error = b2l_mesa_write(data, B2L_MESA_BYTES, ctx);
    }
    if (!error) {
        error = VFS_SYNC(vnode_mount(rootvnode), MNT_WAIT, ctx);
    }
    if (data != NULL) {
        kfree_data(data, B2L_MESA_BYTES);
    }
    printf("B2L MESA: %s publication error=%d bytes=%u compressed=%lu\n", B2L_MESA_LIBRARY, error,
        B2L_MESA_BYTES, (unsigned long)sizeof(b2l_mesa_compressed));
}
#endif /* B2L_HAS_MESA_PAYLOAD */
