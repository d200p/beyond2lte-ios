/* SPDX-License-Identifier: BSD-2-Clause */
/* Bounded existence-only metadata predicates for iOS 26 operation 0x66.
 * Unsupported predicates retain the unavailable response. No trust is granted.
 * Operation 90 answers dyld's amfi_check_dyld_policy_self (see below). */
#ifndef B2L_AMFI_CONSTRAINT_H
#define B2L_AMFI_CONSTRAINT_H
#include "b2l_lwcr.h"
#define B2L_AMFI_CONSTRAINT_MAX 0x4000u
#define B2L_AMFI_CONSTRAINT_RESULT_SIZE 0x404u
/* Observed stock AMFI result for unsupported running-process constraints. */
#define B2L_AMFI_CONSTRAINT_UNSUPPORTED 3u
struct b2l_amfi_constraint_args {
    uint32_t audit_token[8];
    user64_addr_t constraint;
    uint32_t constraint_size;
    uint32_t reserved0;
    user64_addr_t result;
    uint32_t result_size;
    uint32_t reserved1;
};
struct b2l_amfi_constraint_result {
    uint32_t error_code;
    char description[1024];
};
_Static_assert(sizeof(struct b2l_amfi_constraint_args) == 64, "AMFI argument ABI");
_Static_assert(__builtin_offsetof(struct b2l_amfi_constraint_args, constraint) == 0x20, "AMFI constraint ABI");
_Static_assert(__builtin_offsetof(struct b2l_amfi_constraint_args, result) == 0x30, "AMFI result pointer ABI");
_Static_assert(sizeof(struct b2l_amfi_constraint_result) == B2L_AMFI_CONSTRAINT_RESULT_SIZE, "AMFI result ABI");

/* The stock syscall copies its diagnostic result on input/evaluation errors
 * once a valid result pointer/capacity is known. Preserve that dual contract. */
static int
b2l_amfi_constraint_reply(user64_addr_t output, uint32_t code,
    const char *description, int status)
{
    struct b2l_amfi_constraint_result result;
    bzero(&result, sizeof(result));
    result.error_code = code;
    strlcpy(result.description, description, sizeof(result.description));
    int error = copyout(&result, output, sizeof(result));
    return error ? error : status;
}

/* dyld's amfi_check_dyld_policy_self(input, &output): {u64 input flags, u64 output
 * pointer}. Without an answer dyld uses its most restrictive policy and refuses
 * @rpath/@loader_path, so apps with embedded frameworks (MomentsUIService on
 * 0105) die at launch with "security policy does not allow @ path expansion".
 * Grant @-path expansion as release iOS does, except to set-user/group-ID
 * processes. DYLD_* variables, insertion and interposing stay disallowed. */
#define B2L_AMFI_SYSCALL_DYLD_POLICY_SELF 90
#define B2L_AMFI_DYLD_OUTPUT_ALLOW_AT_PATH (1ull << 0)
#define B2L_AMFI_DYLD_OUTPUT_ALLOW_PATH_VARS (1ull << 1)
#define B2L_AMFI_DYLD_OUTPUT_ALLOW_FAILED_INSERTION (1ull << 5)
struct b2l_amfi_dyld_policy_args {
    uint64_t input_flags;
    user64_addr_t output;
};
_Static_assert(sizeof(struct b2l_amfi_dyld_policy_args) == 16, "AMFI dyld policy ABI");
/* Only backboardd may honour DYLD_INSERT_LIBRARIES, for the GPU translation
 * layer, and a library that fails to load must not stop it. */
extern int b2l_gpu_ui_is_ready(void);
static bool
b2l_amfi_gpu_ui_process(struct proc *process)
{
    char path[64];
    int length = sizeof(path);
    if (!b2l_gpu_ui_is_ready()) return false;
    vnode_t vnode = proc_getexecutablevnode(process);
    if (vnode == NULLVP) return false;
    int error = vn_getpath(vnode, path, &length);
    vnode_put(vnode);
    return !error && !strcmp(path, "/usr/libexec/backboardd");
}

static int
b2l_amfi_dyld_policy(struct proc *process, user_addr_t argument)
{
    struct b2l_amfi_dyld_policy_args args;
    int error = copyin(argument, &args, sizeof(args));
    if (error) return error;
    if (!args.output) return EINVAL;
    uint64_t flags = proc_issetugid(process) ? 0 : B2L_AMFI_DYLD_OUTPUT_ALLOW_AT_PATH;
    if (flags && b2l_amfi_gpu_ui_process(process)) {
        flags |= B2L_AMFI_DYLD_OUTPUT_ALLOW_PATH_VARS | B2L_AMFI_DYLD_OUTPUT_ALLOW_FAILED_INSERTION;
    }
    return copyout(&flags, args.output, sizeof(flags));
}

static int
b2l_amfi_constraint_syscall(struct proc *process, int operation, user_addr_t argument)
{
    if (!b2l_amfi_active() || !proc_is64bit(process)) return ENOPOLICY;
    if (operation == B2L_AMFI_SYSCALL_DYLD_POLICY_SELF) return b2l_amfi_dyld_policy(process, argument);
    if (operation != 0x66) return ENOTSUP;
    struct b2l_amfi_constraint_args args;
    int error = copyin(argument, &args, sizeof(args));
    if (error) return error;
    if (!args.result || args.result_size != B2L_AMFI_CONSTRAINT_RESULT_SIZE) return EINVAL;
    if (!args.constraint || !args.constraint_size ||
        args.constraint_size > B2L_AMFI_CONSTRAINT_MAX) {
        return b2l_amfi_constraint_reply(args.result, 5,
            "Invalid launch constraint buffer or length; not evaluated", EINVAL);
    }
    if (args.constraint > UINT64_MAX - (args.constraint_size - 1u)) {
        return b2l_amfi_constraint_reply(args.result, 5,
            "Unreadable launch constraint range; not evaluated", EFAULT);
    }
    uint8_t *bytes = kalloc_data(args.constraint_size, Z_WAITOK);
    if (!bytes) return b2l_amfi_constraint_reply(args.result, 5,
        "Constraint allocation failed; not evaluated", ENOMEM);
    error = copyin(args.constraint, bytes, args.constraint_size);
    char key[B2L_LWCR_KEY_MAX];
    int parsed = error ? error : b2l_lwcr_entitlement_exists(bytes, args.constraint_size, key);
    kfree_data(bytes, args.constraint_size);
    if (error) return b2l_amfi_constraint_reply(args.result, 5,
        "Failed to read launch constraint; not evaluated", error);
    if (parsed == EINVAL) return b2l_amfi_constraint_reply(args.result, 5,
        "Malformed launch constraint; not evaluated", EINVAL);
    if (parsed) return b2l_amfi_constraint_reply(args.result, B2L_AMFI_CONSTRAINT_UNSUPPORTED,
        "Launch constraint matching unavailable in B2L development mode; not evaluated", ENOTSUP);
    audit_token_t token;
    memcpy(&token, args.audit_token, sizeof(token));
    proc_t target = proc_find_audit_token(token);
    if (!target) return b2l_amfi_constraint_reply(args.result, 1,
        "Target process audit identity is no longer available", 0);
    void *entitlements = proc_copy_ent(target);
    void *value = b2l_ent_copy_object(entitlements, key);
    bool exists = value != NULL && proc_pidversion(target) == token.val[7];
    b2l_ent_release(value);
    b2l_ent_release(entitlements);
    proc_rele(target);
    return b2l_amfi_constraint_reply(args.result, exists ? 0 : 1,
        exists ? "" : "Entitlement key is absent from target metadata", 0);
}
static const struct mac_policy_ops b2l_amfi_constraint_ops = {
    .mpo_policy_syscall = b2l_amfi_constraint_syscall,
};
static struct mac_policy_conf b2l_amfi_constraint_conf = {
    .mpc_name = "AMFI",
    .mpc_fullname = "B2L existence-only launch constraint metadata API",
    .mpc_ops = &b2l_amfi_constraint_ops,
};
static mac_policy_handle_t b2l_amfi_constraint_handle;
static bool b2l_amfi_constraint_registered;
static void
b2l_amfi_constraint_register(void)
{
    if (!b2l_amfi_active() || b2l_amfi_constraint_registered) return;
    int error = mac_policy_register(&b2l_amfi_constraint_conf,
        &b2l_amfi_constraint_handle, NULL);
    if (!error) b2l_amfi_constraint_registered = true;
    printf("B2L AMFI: existence-only constraint metadata API %s (%d)\n",
        error ? "not registered" : "registered", error);
}
#endif /* B2L_AMFI_CONSTRAINT_H */
