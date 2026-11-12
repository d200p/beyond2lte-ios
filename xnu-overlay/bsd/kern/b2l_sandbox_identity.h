/* SPDX-License-Identifier: BSD-2-Clause
 * Report sandbox identity to processes that iOS would sandbox; enforce nothing.
 *
 * The B2L Sandbox policy is unconfined. Several services refuse to run unless
 * they find themselves sandboxed (owner-approved option 2, build 0107):
 *   - MessagesBlastDoorService requires sandbox_check(getpid(), NULL, 0) == 1
 *     ("BlastDoor cannot run unsandboxed.").
 *   - ExtensionKit appex (speech synthesizer audio units) abort in PlugInKit with
 *     "plugin has invalid sandbox or container" when
 *     sandbox_container_path_for_pid(getpid(), ...) fails.
 * A process counts as sandboxed when launchd assigned it a container at spawn
 * (b2l_sandbox_container.h) or its entitlements request a sandbox:
 * com.apple.security.app-sandbox = true, a com.apple.private.sandbox.profile:
 * embedded profile, or seatbelt-profiles. Only then:
 *   - Sandbox op 2 with a NULL operation ("is this pid sandboxed?") writes 1;
 *     every permission query keeps the unconfined "allowed" answer.
 *   - Sandbox op 4 for the caller itself returns its spawn container, or
 *     else its HOME directory if HOME is set and is a directory.
 * The includer defines B2L_PROC_UNIQUEID, B2L_PROC_ARGSLEN, B2L_PROC_USER_STACK
 * and B2L_SANDBOX_TRACE (bounded, numeric-only logging).
 * This states a sandbox that does not exist. It is development-only identity,
 * not isolation: BlastDoor parses untrusted message content without one.
 */
#ifndef B2L_SANDBOX_IDENTITY_H
#define B2L_SANDBOX_IDENTITY_H

#define B2L_SANDBOX_ARGS_MAX (256u * 1024u)

struct b2l_sandbox_check_args {      /* libsystem_sandbox sandbox_check, op 2 */
    user64_addr_t result;            /* u64 written nonzero = sandboxed/denied */
    int64_t pid;
    user64_addr_t operation;         /* NULL: identity query */
    uint64_t filter_type, filter_arg, flags;
};
struct b2l_sandbox_path_args {       /* sandbox_container_path_for_pid, op 4 */
    int64_t pid;
    uint64_t reserved0;
    user64_addr_t buffer;
    uint64_t size;
    uint64_t reserved1;
};
_Static_assert(sizeof(struct b2l_sandbox_check_args) == 48, "sandbox_check ABI");
_Static_assert(sizeof(struct b2l_sandbox_path_args) == 40, "container path ABI");

/* HOME=value from a process argument area (NUL-separated exec path, argv,
 * environment, apple strings). Returns the value length, 0 if absent/invalid. */
static size_t
b2l_sandbox_home(const char *area, size_t n, const char **value)
{
    size_t at = 0;
    *value = NULL;
    while (at < n) {
        size_t end = at;
        while (end < n && area[end]) end++;
        if (end == n) return 0;              /* unterminated tail: ignore */
        if (end - at > 6 && !memcmp(area + at, "HOME=/", 6)) {
            size_t len = end - at - 5;
            if (len >= B2L_SANDBOX_PATH_MAX) return 0;
            *value = area + at + 5;
            return len;
        }
        at = end + 1;
    }
    return 0;
}

static bool
b2l_sandbox_entitled(proc_t process)
{
    char path[B2L_SANDBOX_PATH_MAX];
    if (b2l_sandbox_container_lookup(B2L_PROC_UNIQUEID(process), path)) return true;
    void *e = NULL;
    if (b2l_amfi_proc_copy_ent(process, &e) || !e) return false;
    bool sandboxed = b2l_ent_bool(e, "com.apple.security.app-sandbox");
    if (!sandboxed) {
        void *v = b2l_ent_copy_object(e, "com.apple.private.sandbox.profile:embedded");
        if (!v) v = b2l_ent_copy_object(e, "seatbelt-profiles");
        sandboxed = v != NULL;
        b2l_ent_release(v);
    }
    b2l_ent_release(e);
    return sandboxed;
}

static int
b2l_sandbox_check_query(proc_t self, user_addr_t argument)
{
    struct b2l_sandbox_check_args args;
    /* Permission queries and unreadable input keep the unconfined answer. */
    if (copyin(argument, &args, sizeof(args)) || args.operation || !args.result) return 0;
    proc_t target = (args.pid == 0 || args.pid == proc_pid(self)) ? self : proc_find((int)args.pid);
    bool sandboxed = target != NULL && b2l_sandbox_entitled(target);
    if (target != NULL && target != self) proc_rele(target);
    if (!sandboxed) return 0;
    uint64_t one = 1;
    return copyout(&one, args.result, sizeof(one));
}

static int
b2l_sandbox_container_path(proc_t self, user_addr_t argument)
{
    struct b2l_sandbox_path_args args;
    int error = copyin(argument, &args, sizeof(args));
    if (error) return error;
    if ((args.pid != 0 && args.pid != proc_pid(self)) || !args.buffer || !args.size) return ENOENT;
    char path[B2L_SANDBOX_PATH_MAX];
    size_t len = b2l_sandbox_container_lookup(B2L_PROC_UNIQUEID(self), path);
    if (len) {
        B2L_SANDBOX_TRACE("B2L Sandbox: container query answered from spawn (%zu bytes)\n", len + 1);
        return len + 1 > args.size ? ERANGE : copyout(path, args.buffer, len + 1);
    }
    if (!b2l_sandbox_entitled(self)) return ENOENT;
    size_t n = B2L_PROC_ARGSLEN(self);
    user_addr_t top = B2L_PROC_USER_STACK(self);
    if (!n || n > B2L_SANDBOX_ARGS_MAX || top < n) return ENOENT;
    char *area = kalloc_data(n, Z_WAITOK);
    if (!area) return ENOMEM;
    const char *home = NULL;
    error = copyin(top - n, area, n) ? ENOENT : 0;
    if (!error) len = b2l_sandbox_home(area, n, &home);
    if (!error && (!len || len + 1 > args.size)) error = len ? ERANGE : ENOENT;
    if (!error) {
        memcpy(path, home, len);
        path[len] = 0;
    }
    kfree_data(area, n);
    if (error) return error;
    vnode_t vp = NULLVP;
    if (vnode_lookup(path, 0, &vp, vfs_context_current()) || vp == NULLVP) return ENOENT;
    bool directory = vnode_isdir(vp);
    vnode_put(vp);
    if (!directory) return ENOENT;
    B2L_SANDBOX_TRACE("B2L Sandbox: container query answered from HOME (%zu bytes)\n", len + 1);
    return copyout(path, args.buffer, len + 1);
}
#endif /* B2L_SANDBOX_IDENTITY_H */
