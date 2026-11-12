/* SPDX-License-Identifier: BSD-2-Clause
 * Remember the container launchd assigns at spawn; enforce nothing.
 *
 * launchd describes a sandboxed job's container through libsandbox's spawn
 * attributes (sandbox_spawnattrs_setcontainer) and passes them to the policy
 * named "Sandbox" with posix_spawnattr_setmacpolicyinfo_np. The iOS 26.6.2
 * layout is: path length (u32) at offset 12, NUL-terminated absolute path
 * (< 1024 bytes) at offset 80. XNU hands each policy its own blob in
 * mpo_proc_check_launch_constraints, which every policy receives (unlike the
 * exec label-update check, which stops at the first policy wanting a
 * transition).
 *
 * The record is keyed by the process unique ID, replaced on every exec and
 * dropped at exit, so Sandbox operation 4 can answer what the real policy
 * would: the caller's own container. A full table evicts its oldest entry.
 * Paths are never logged.
 */
#ifndef B2L_SANDBOX_CONTAINER_H
#define B2L_SANDBOX_CONTAINER_H

#define B2L_SANDBOX_PATH_MAX 1024u
#define B2L_SANDBOX_CONTAINERS 64u
#define B2L_SANDBOX_SPAWN_LEN_AT 12u
#define B2L_SANDBOX_SPAWN_PATH_AT 80u

struct b2l_sandbox_container {
    uint64_t owner;                  /* process unique ID; 0 = free */
    uint64_t stamp;                  /* last record, for eviction */
    uint32_t len;
    char path[B2L_SANDBOX_PATH_MAX];
};
static struct b2l_sandbox_container b2l_sandbox_containers[B2L_SANDBOX_CONTAINERS];
static uint64_t b2l_sandbox_container_clock;

/* The container path in a Sandbox spawn attribute blob; 0 if absent/invalid. */
static size_t
b2l_sandbox_spawn_container(const void *attr, size_t n, const char **path)
{
    uint32_t len;
    *path = NULL;
    if (attr == NULL || n <= B2L_SANDBOX_SPAWN_PATH_AT + 1) return 0;
    memcpy(&len, (const uint8_t *)attr + B2L_SANDBOX_SPAWN_LEN_AT, sizeof(len));
    const char *p = (const char *)attr + B2L_SANDBOX_SPAWN_PATH_AT;
    if (len < 2 || len >= B2L_SANDBOX_PATH_MAX || len >= n - B2L_SANDBOX_SPAWN_PATH_AT) return 0;
    if (p[0] != '/' || p[len] != '\0' || strnlen(p, len) != len) return 0;
    *path = p;
    return len;
}

/* Record (or, for an exec without a container, clear) the owner's container. */
static void
b2l_sandbox_container_record(uint64_t owner, const void *attr, size_t n)
{
    const char *path;
    size_t len = b2l_sandbox_spawn_container(attr, n, &path);
    if (owner == 0) return;
    B2L_SANDBOX_CONTAINER_LOCK();
    struct b2l_sandbox_container *slot = NULL, *oldest = NULL;
    for (unsigned i = 0; i < B2L_SANDBOX_CONTAINERS; i++) {
        struct b2l_sandbox_container *c = &b2l_sandbox_containers[i];
        if (c->owner == owner) { slot = c; break; }
        if (oldest == NULL || (oldest->owner && (!c->owner || c->stamp < oldest->stamp))) oldest = c;
    }
    if (len) {
        if (slot == NULL) slot = oldest;
        slot->owner = owner;
        slot->stamp = ++b2l_sandbox_container_clock;
        slot->len = (uint32_t)len;
        memcpy(slot->path, path, len);
        slot->path[len] = '\0';
    } else if (slot != NULL) {
        memset(slot, 0, sizeof(*slot));
    }
    B2L_SANDBOX_CONTAINER_UNLOCK();
}

/* Copy the owner's container into out (B2L_SANDBOX_PATH_MAX bytes); its length or 0. */
static size_t
b2l_sandbox_container_lookup(uint64_t owner, char *out)
{
    size_t len = 0;
    if (owner == 0) return 0;
    B2L_SANDBOX_CONTAINER_LOCK();
    for (unsigned i = 0; i < B2L_SANDBOX_CONTAINERS; i++) {
        struct b2l_sandbox_container *c = &b2l_sandbox_containers[i];
        if (c->owner == owner) {
            len = c->len;
            memcpy(out, c->path, len + 1);
            break;
        }
    }
    B2L_SANDBOX_CONTAINER_UNLOCK();
    return len;
}

static void
b2l_sandbox_container_forget(uint64_t owner)
{
    if (owner == 0) return;
    B2L_SANDBOX_CONTAINER_LOCK();
    for (unsigned i = 0; i < B2L_SANDBOX_CONTAINERS; i++) {
        if (b2l_sandbox_containers[i].owner == owner) memset(&b2l_sandbox_containers[i], 0, sizeof(b2l_sandbox_containers[i]));
    }
    B2L_SANDBOX_CONTAINER_UNLOCK();
}
#endif /* B2L_SANDBOX_CONTAINER_H */
