/* SPDX-License-Identifier: BSD-2-Clause
 * Seed the missing iOS persona manifest through mounted HFS VFS after replay.
 * This is a plain metadata plist, not a serialized keybag or fabricated key.
 * Existing manifests are never overwritten. Publish a fully synced staging
 * inode using an exclusive hard link; interrupted staging is prefix-resumable.
 */
#ifndef B2L_PERSONA_HOST_TEST
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/fcntl.h>
#include <sys/errno.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/vnode_if.h>
#include <sys/uio.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include "b2l_amfi.h"
extern vnode_t rootvnode;
#endif
static const char manifest_path[]="/private/var/keybags/persona.kb";
static const char staging_path[]="/private/var/keybags/.b2l-persona-initial-v1";
static const char initial_manifest[]=
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\"><dict>"
    "<key>UsePersonaManifestVersion</key><integer>1</integer>"
    "<key>UsePersonaGenerationID</key><integer>1</integer>"
    "<key>UserPersonaDictionary</key><dict/>"
    "</dict></plist>\n";
static bool root_type(vnode_t vp,enum vtype type)
{return vp&&vnode_vtype(vp)==type&&vnode_mount(vp)==vnode_mount(rootvnode);}
static void lookup_done(struct nameidata *nd)
{
    nameidone(nd);
    if(nd->ni_vp)vnode_put(nd->ni_vp);
    if(nd->ni_dvp)vnode_put(nd->ni_dvp);
}
static int ensure_keybags(vfs_context_t ctx)
{
    struct nameidata nd;struct vnode_attr va;vnode_t vp=NULL;
    NDINIT(&nd,CREATE,OP_MKDIR,LOCKPARENT,UIO_SYSSPACE,
        CAST_USER_ADDR_T("/private/var/keybags"),ctx);
    nd.ni_cnd.cn_flags|=WILLBEDIR;
    int error=namei(&nd);if(error)return error;
    if(!root_type(nd.ni_dvp,VDIR)){error=EXDEV;goto out;}
    if(nd.ni_vp){error=root_type(nd.ni_vp,VDIR)?0:ENOTDIR;goto out;}
    VATTR_INIT(&va);VATTR_SET(&va,va_type,VDIR);VATTR_SET(&va,va_mode,0700);
    VATTR_SET(&va,va_uid,0);VATTR_SET(&va,va_gid,0);
    error=vn_mkdir(nd.ni_dvp,&vp,&nd,&va,ctx);
    if(!error){
        vnode_update_identity(vp,nd.ni_dvp,nd.ni_cnd.cn_nameptr,
            nd.ni_cnd.cn_namelen,nd.ni_cnd.cn_hash,VNODE_UPDATE_NAME|VNODE_UPDATE_PARENT);
        error=VNOP_FSYNC(vp,MNT_WAIT,ctx);
        if(!error)error=VNOP_FSYNC(nd.ni_dvp,MNT_WAIT,ctx);
    }
    if(vp)vnode_put(vp);
out:lookup_done(&nd);return error;
}
static int sync_seed(vnode_t vp,vfs_context_t ctx)
{
    struct vnode_attr va;char verify[sizeof(initial_manifest)];int resid=0;
    if(!root_type(vp,VREG))return EINVAL;
    VATTR_INIT(&va);VATTR_WANTED(&va,va_data_size);VATTR_WANTED(&va,va_uid);VATTR_WANTED(&va,va_nlink);
    int error=vnode_getattr(vp,&va,ctx);if(error)return error;
    if(!VATTR_IS_SUPPORTED(&va,va_data_size)||!VATTR_IS_SUPPORTED(&va,va_uid)||
       !VATTR_IS_SUPPORTED(&va,va_nlink)||va.va_uid||va.va_nlink!=1||
       va.va_data_size<0||(uint64_t)va.va_data_size>sizeof(initial_manifest)-1)return EINVAL;
    size_t size=(size_t)va.va_data_size;
    if(size){
        error=vn_rdwr(UIO_READ,vp,verify,(int)size,0,UIO_SYSSPACE,0,kauth_cred_get(),&resid,current_proc());
        if(error||resid||memcmp(verify,initial_manifest,size))return error?error:EIO;
    }
    if(size<sizeof(initial_manifest)-1){
        error=vn_rdwr(UIO_WRITE,vp,(caddr_t)initial_manifest+size,
            (int)(sizeof(initial_manifest)-1-size),(off_t)size,UIO_SYSSPACE,IO_SYNC,
            kauth_cred_get(),&resid,current_proc());
        if(error||resid)return error?error:EIO;
    }
    error=VNOP_FSYNC(vp,MNT_WAIT,ctx);if(error)return error;
    error=vn_rdwr(UIO_READ,vp,verify,sizeof(initial_manifest)-1,0,UIO_SYSSPACE,0,kauth_cred_get(),&resid,current_proc());
    return error?error:(resid||memcmp(verify,initial_manifest,sizeof(initial_manifest)-1)?EIO:0);
}
static int publish_seed(vnode_t vp,vfs_context_t ctx)
{
    struct nameidata nd;
    NDINIT(&nd,CREATE,OP_LINK,LOCKPARENT,UIO_SYSSPACE,CAST_USER_ADDR_T(manifest_path),ctx);
    int error=namei(&nd);if(error)return error;
    if(nd.ni_vp){error=EEXIST;goto out;}
    if(!root_type(nd.ni_dvp,VDIR)){error=EXDEV;goto out;}
    error=VNOP_LINK(vp,nd.ni_dvp,&nd.ni_cnd,ctx);
    if(!error)error=VNOP_FSYNC(nd.ni_dvp,MNT_WAIT,ctx);
out:lookup_done(&nd);return error;
}
#ifndef B2L_PERSONA_HOST_TEST
#include "b2l_userspace_seed.h"
#include <pexpert/pexpert.h>
/* Accessibility defaults for CPU-only composition. Blur, glass and slide
 * transitions are the most expensive software-rendered effects on this board
 * and the render server has aborted inside them. Seeded with b2l_ax_seed=1
 * into the managed layer (forced values cfprefsd overlays on every domain)
 * and, only when absent, into the user's own domain file. Existing files with
 * other content are never modified; an interrupted seed resumes by prefix. */
static const char ax_managed_dir[]="/private/var/Managed Preferences";
static const char ax_managed_user_dir[]="/private/var/Managed Preferences/mobile";
static const char ax_managed_path[]="/private/var/Managed Preferences/mobile/com.apple.Accessibility.plist";
static const char ax_user_path[]="/private/var/mobile/Library/Preferences/com.apple.Accessibility.plist";
static const char ax_plist[]=
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\"><dict>"
    "<key>ReduceMotionEnabled</key><true/>"
    "<key>EnhancedBackgroundContrastEnabled</key><true/>"
    "<key>ReduceMotionReduceSlideTransitionsPreference</key><true/>"
    "<key>AutoplayAnimatedImagesEnabled</key><false/>"
    "<key>ReduceMotionAutoplayMessagesEffectsEnabled</key><false/>"
    "</dict></plist>\n";
static int ax_directory(const char *path,uid_t uid,gid_t gid,mode_t mode,vfs_context_t ctx)
{
    struct nameidata nd;struct vnode_attr va;vnode_t vp=NULL;
    NDINIT(&nd,CREATE,OP_MKDIR,LOCKPARENT,UIO_SYSSPACE,CAST_USER_ADDR_T(path),ctx);
    nd.ni_flag|=NAMEI_NOFOLLOW_ANY;nd.ni_cnd.cn_flags|=WILLBEDIR;
    int error=namei(&nd);if(error)return error;
    if(!root_type(nd.ni_dvp,VDIR)){error=EXDEV;goto out;}
    if(nd.ni_vp){error=root_type(nd.ni_vp,VDIR)?0:ENOTDIR;goto out;}
    VATTR_INIT(&va);VATTR_SET(&va,va_type,VDIR);VATTR_SET(&va,va_mode,mode);
    VATTR_SET(&va,va_uid,uid);VATTR_SET(&va,va_gid,gid);
    error=vn_mkdir(nd.ni_dvp,&vp,&nd,&va,ctx);
    if(!error){
        vnode_update_identity(vp,nd.ni_dvp,nd.ni_cnd.cn_nameptr,
            nd.ni_cnd.cn_namelen,nd.ni_cnd.cn_hash,VNODE_UPDATE_NAME|VNODE_UPDATE_PARENT);
        error=VNOP_FSYNC(vp,MNT_WAIT,ctx);
        if(!error)error=VNOP_FSYNC(nd.ni_dvp,MNT_WAIT,ctx);
    }
    if(vp)vnode_put(vp);
out:lookup_done(&nd);return error;
}
/* Returns 0 when the file now holds exactly ax_plist, EEXIST when a different
 * file is preserved, or an I/O error. Only a missing file or an identical
 * prefix (interrupted earlier seed) is written. */
static int ax_file(const char *path,uid_t uid,gid_t gid,vfs_context_t ctx)
{
    const size_t n=sizeof(ax_plist)-1;static char verify[sizeof(ax_plist)];
    struct vnode_attr va;vnode_t vp=NULL;int resid=0;
    int error=vnode_open(path,FREAD|FWRITE|O_CREAT|O_NOFOLLOW,0644,VNODE_LOOKUP_NOFOLLOW_ANY,&vp,ctx);
    if(error)return error;
    if(!root_type(vp,VREG)){error=EINVAL;goto close;}
    VATTR_INIT(&va);VATTR_WANTED(&va,va_data_size);VATTR_WANTED(&va,va_nlink);
    error=vnode_getattr(vp,&va,ctx);if(error)goto close;
    if(!VATTR_IS_SUPPORTED(&va,va_data_size)||!VATTR_IS_SUPPORTED(&va,va_nlink)||
       va.va_nlink!=1||va.va_data_size<0||(uint64_t)va.va_data_size>n){error=EEXIST;goto close;}
    size_t size=(size_t)va.va_data_size;
    if(size){
        error=vn_rdwr(UIO_READ,vp,verify,(int)size,0,UIO_SYSSPACE,0,kauth_cred_get(),&resid,current_proc());
        if(error)goto close;
        if(resid||memcmp(verify,ax_plist,size)){error=EEXIST;goto close;}
    }
    if(size<n){
        error=vn_rdwr(UIO_WRITE,vp,(caddr_t)ax_plist+size,(int)(n-size),(off_t)size,UIO_SYSSPACE,IO_SYNC,
            kauth_cred_get(),&resid,current_proc());
        if(!error&&resid)error=EIO;
        if(error)goto close;
    }
    VATTR_INIT(&va);VATTR_SET(&va,va_uid,uid);VATTR_SET(&va,va_gid,gid);VATTR_SET(&va,va_mode,0644);
    error=vnode_setattr(vp,&va,ctx);if(error)goto close;
    error=VNOP_FSYNC(vp,MNT_WAIT,ctx);if(error)goto close;
    error=vn_rdwr(UIO_READ,vp,verify,(int)n,0,UIO_SYSSPACE,0,kauth_cred_get(),&resid,current_proc());
    if(!error&&(resid||memcmp(verify,ax_plist,n)))error=EIO;
close:
    {int close_error=vnode_close(vp,FREAD|FWRITE,ctx);if(!error)error=close_error;}
    return error;
}
static void b2l_accessibility_before_init(vfs_context_t ctx)
{
    int seed=0;
    if(!PE_parse_boot_argn("b2l_ax_seed",&seed,sizeof(seed))||seed!=1){
        printf("B2L AXSEED: disabled; accessibility defaults untouched\n");return;
    }
    int managed=ax_directory(ax_managed_dir,0,0,0755,ctx);
    if(!managed)managed=ax_directory(ax_managed_user_dir,0,0,0755,ctx);
    if(!managed)managed=ax_file(ax_managed_path,0,0,ctx);
    int user=ax_file(ax_user_path,501,501,ctx);
    int error=VFS_SYNC(vnode_mount(rootvnode),MNT_WAIT,ctx);
    printf("B2L AXSEED: reduce motion/transparency defaults managed=%d user=%d sync=%d (0 seeded, %d preserved different file)\n",
        managed,user,error,EEXIST);
}
#endif
#ifndef B2L_PERSONA_HOST_TEST
extern void b2l_acpm_dvfs_start(void);
#endif
void b2l_persona_before_init(void)
{
#ifndef B2L_PERSONA_HOST_TEST
    b2l_userspace_before_init();
    b2l_acpm_dvfs_start();
    if(rootvnode&&!vfs_isrdonly(vnode_mount(rootvnode))&&
       !strcmp(vfs_statfs(vnode_mount(rootvnode))->f_fstypename,"hfs"))
        b2l_accessibility_before_init(vfs_context_current());
#endif
    if(!b2l_amfi_active()||!rootvnode||vfs_isrdonly(vnode_mount(rootvnode))||
       strcmp(vfs_statfs(vnode_mount(rootvnode))->f_fstypename,"hfs"))return;
    vfs_context_t ctx=vfs_context_current();vnode_t vp=NULL;
    int error=vnode_lookup(manifest_path,VNODE_LOOKUP_NOFOLLOW_ANY,&vp,ctx);
    if(!error){
        printf("B2L PERSONA: existing manifest preserved (regular=%d)\n",root_type(vp,VREG));
        vnode_put(vp);return;
    }
    if(error!=ENOENT)goto out;
    error=ensure_keybags(ctx);if(error)goto out;
    error=vnode_open(staging_path,FREAD|FWRITE|O_CREAT|O_NOFOLLOW,0600,
        VNODE_LOOKUP_NOFOLLOW_ANY,&vp,ctx);
    if(error)goto out;
    error=sync_seed(vp,ctx);
    if(!error)error=publish_seed(vp,ctx);
    int close_error=vnode_close(vp,FREAD|FWRITE,ctx);vp=NULL;
    if(!error)error=close_error;
    if(!error)error=VFS_SYNC(vnode_mount(rootvnode),MNT_WAIT,ctx);
    if(!error)printf("B2L PERSONA: initial v1 manifest published after journal replay\n");
out:
    if(error)printf("B2L PERSONA: initialization error=%d; existing data preserved\n",error);
}
