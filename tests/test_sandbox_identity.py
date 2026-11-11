#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Exercise the actual b2l_sandbox_container.h and b2l_sandbox_identity.h with kernel boundary mocks."""
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / 'xnu-overlay/bsd/kern/b2l_sandbox_identity.h'

PROGRAM = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t user_addr_t, user64_addr_t;
#define Z_WAITOK 0
#define NULLVP ((vnode_t)0)
struct proc { int pid; int ent; size_t argslen; user_addr_t user_stack; uint64_t uniqueid; };
typedef struct proc *proc_t;
typedef struct vnode { bool dir; } *vnode_t;
typedef void *vfs_context_t;
#define B2L_PROC_ARGSLEN(p) ((p)->argslen)
#define B2L_PROC_USER_STACK(p) ((p)->user_stack)
#define B2L_PROC_UNIQUEID(p) ((p)->uniqueid)
static int locked, traces;
#define B2L_SANDBOX_CONTAINER_LOCK() do { assert(!locked); locked = 1; } while (0)
#define B2L_SANDBOX_CONTAINER_UNLOCK() do { assert(locked); locked = 0; } while (0)
#define B2L_SANDBOX_TRACE(...) (traces++)
static uint8_t memory[0x80000];
static unsigned refs, objects, allocations, writes;
static int copyin(user_addr_t a, void *p, size_t n) { if (a < 0x100 || a > sizeof(memory) || n > sizeof(memory) - a) return 14; memcpy(p, memory + a, n); return 0; }
static int copyout(const void *p, user_addr_t a, size_t n) { if (a < 0x100 || a > sizeof(memory) || n > sizeof(memory) - a) return 14; memcpy(memory + a, p, n); writes++; return 0; }
static void *kalloc_data(size_t n, int f) { (void)f; allocations++; return malloc(n); }
static void kfree_data(void *p, size_t n) { (void)n; assert(allocations); allocations--; free(p); }
/* Entitlement kinds: 0 none, 1 app-sandbox, 2 embedded profile, 3 seatbelt-profiles, 4 app-sandbox=false */
static struct proc self = {100, 0, 0, 0, 1000}, other = {200, 1, 0, 0, 2000};
static int proc_pid(proc_t p) { return p->pid; }
static proc_t proc_find(int pid) { if (pid == other.pid) { refs++; return &other; } return NULL; }
static void proc_rele(proc_t p) { assert(p == &other && refs); refs--; }
/* Only the entitlement-object accessor exists here: the plain-dictionary one is not accepted by b2l_ent_*. */
static int b2l_amfi_proc_copy_ent(proc_t p, void **out) { objects++; *out = (void *)(intptr_t)(p->ent + 1); return 0; }
static bool b2l_ent_bool(const void *e, const char *k) { return (intptr_t)e - 1 == 1 && !strcmp(k, "com.apple.security.app-sandbox"); }
static void *b2l_ent_copy_object(const void *e, const char *k) {
    int kind = (int)((intptr_t)e - 1);
    if ((kind == 2 && !strcmp(k, "com.apple.private.sandbox.profile:embedded")) || (kind == 3 && !strcmp(k, "seatbelt-profiles"))) { objects++; return (void *)8; }
    return NULL;
}
static void b2l_ent_release(void *p) { if (p) { assert(objects); objects--; } }
static struct vnode dir = {true}, file = {false};
static int lookup_mode; /* 0 dir, 1 file, 2 missing */
static int vnode_lookup(const char *path, int f, vnode_t *vp, vfs_context_t c) {
    (void)f; (void)c; assert(path[0] == '/');
    if (lookup_mode == 2) return 2; *vp = lookup_mode ? &file : &dir; refs++; return 0; }
static bool vnode_isdir(vnode_t v) { return v->dir; }
static void vnode_put(vnode_t v) { (void)v; assert(refs); refs--; }
static vfs_context_t vfs_context_current(void) { return 0; }
#include "b2l_sandbox_container.h"
#include "b2l_sandbox_identity.h"

static uint64_t check(int64_t pid, uint64_t operation) {
    struct b2l_sandbox_check_args a = {0x1000, pid, operation, 0, 0, 0};
    uint64_t zero = 0; memcpy(memory + 0x1000, &zero, 8); memcpy(memory + 0x200, &a, sizeof(a));
    assert(b2l_sandbox_check_query(&self, 0x200) == 0);
    uint64_t r; memcpy(&r, memory + 0x1000, 8); assert(!refs && !objects); return r;
}
static void args_area(const char *const *strings, size_t count) {
    size_t n = 0; char *top = (char *)memory + 0x60000;
    for (size_t i = 0; i < count; i++) n += strlen(strings[i]) + 1;
    char *p = top - n;
    for (size_t i = 0; i < count; i++) { size_t l = strlen(strings[i]) + 1; memcpy(p, strings[i], l); p += l; }
    self.argslen = n; self.user_stack = 0x60000;
}
static int path(int64_t pid, uint64_t size, char *out) {
    struct b2l_sandbox_path_args a = {pid, 0, 0x2000, size, 0};
    memset(memory + 0x2000, 0x5a, 64); memcpy(memory + 0x300, &a, sizeof(a));
    int r = b2l_sandbox_container_path(&self, 0x300);
    assert(!refs && !objects && !allocations);
    if (!r) strcpy(out, (char *)memory + 0x2000);
    return r;
}
int main(void) {
    /* Identity query: only sandbox-entitled processes read as sandboxed. */
    for (int kind = 0; kind <= 4; kind++) { self.ent = kind; assert(check(0, 0) == (kind >= 1 && kind <= 3)); assert(check(100, 0) == (kind >= 1 && kind <= 3)); }
    self.ent = 1;
    assert(check(100, 0x4000) == 0);           /* permission query: still allowed */
    assert(check(200, 0) == 1);                /* other entitled process, ref released */
    other.ent = 0; assert(check(200, 0) == 0); other.ent = 1;
    assert(check(999, 0) == 0);                /* unknown pid */
    { struct b2l_sandbox_check_args a = {0, 0, 0, 0, 0, 0}; memcpy(memory + 0x200, &a, sizeof(a)); writes = 0;
      assert(b2l_sandbox_check_query(&self, 0x200) == 0 && !writes); }                 /* no result pointer */
    assert(b2l_sandbox_check_query(&self, 0x7fffffffffffull) == 0 && !objects);        /* unreadable input */

    /* Container path: the caller's own HOME, only when entitled and a directory. */
    char out[1100];
    const char *env[] = {"executable_path=/System/Library/ExtensionKit/Extensions/SiriAUSP.appex/SiriAUSP", "SiriAUSP", "PATH=/usr/bin",
                         "HOME=/private/var/mobile/Containers/Data/PluginKitPlugin/1234", "TMPDIR=/tmp", "ptr_munge="};
    args_area(env, 6);
    assert(!path(0, 256, out) && !strcmp(out, "/private/var/mobile/Containers/Data/PluginKitPlugin/1234"));
    assert(!path(100, 256, out));
    assert(path(200, 256, out) == ENOENT);                         /* other processes: never */
    assert(path(0, 20, out) == ERANGE);                            /* buffer too small */
    lookup_mode = 1; assert(path(0, 256, out) == ENOENT);          /* HOME is not a directory */
    lookup_mode = 2; assert(path(0, 256, out) == ENOENT); lookup_mode = 0;
    self.ent = 0; assert(path(0, 256, out) == ENOENT); self.ent = 2;
    assert(!path(0, 256, out));                                    /* embedded-profile entitlement */
    const char *nohome[] = {"executable_path=/x", "x", "HOMEDIR=/nope", "HOME=relative"};
    args_area(nohome, 4); assert(path(0, 256, out) == ENOENT);
    args_area(env, 6); memory[0x60000 - 1] = 'x';                  /* unterminated tail */
    assert(path(0, 256, out) == 0);                                /* HOME found before the tail */
    self.argslen = 0; assert(path(0, 256, out) == ENOENT);
    self.argslen = B2L_SANDBOX_ARGS_MAX + 1; assert(path(0, 256, out) == ENOENT);
    args_area(env, 6);
    { struct b2l_sandbox_path_args a = {0, 0, 0x7ffffffffff0ull, 256, 0}; memcpy(memory + 0x300, &a, sizeof(a));
      assert(b2l_sandbox_container_path(&self, 0x300) == 14 && !allocations && !refs); }   /* bad output pointer */
    const char *s = NULL; const char t[] = "a\0HOME=/v\0";
    assert(b2l_sandbox_home(t, sizeof(t) - 1, &s) == 2 && !memcmp(s, "/v", 2));
    /* Spawn container: libsandbox layout, length at 12, path at 80. */
    uint8_t attr[1200];
    #define SPAWN(path) (memset(attr, 0, sizeof(attr)), *(uint32_t *)(attr + 12) = (uint32_t)strlen(path), strcpy((char *)attr + 80, path), 80 + strlen(path) + 1)
    const char *pp; size_t an;
    an = SPAWN("/private/var/mobile/Containers/Data/PluginKitPlugin/AB");
    assert(b2l_sandbox_spawn_container(attr, an, &pp) == 54 && !strcmp(pp, "/private/var/mobile/Containers/Data/PluginKitPlugin/AB"));
    assert(b2l_sandbox_spawn_container(attr, an - 1, &pp) == 0 && !pp);           /* NUL outside the blob */
    assert(b2l_sandbox_spawn_container(NULL, an, &pp) == 0);
    assert(b2l_sandbox_spawn_container(attr, 80, &pp) == 0);
    *(uint32_t *)(attr + 12) = 53; assert(b2l_sandbox_spawn_container(attr, an, &pp) == 0);   /* length mismatch */
    *(uint32_t *)(attr + 12) = 0xffffffffu; assert(b2l_sandbox_spawn_container(attr, an, &pp) == 0);
    an = SPAWN("relative/path"); assert(b2l_sandbox_spawn_container(attr, an, &pp) == 0);
    memset(attr, 'a', sizeof(attr)); attr[80] = '/'; *(uint32_t *)(attr + 12) = 1024;
    assert(b2l_sandbox_spawn_container(attr, sizeof(attr), &pp) == 0);              /* too long */

    /* Unentitled process with a spawn container: sandboxed, container answered. */
    self.ent = 0; args_area(env, 6);
    an = SPAWN("/private/var/mobile/Containers/Data/PluginKitPlugin/AB");
    b2l_sandbox_container_record(self.uniqueid, attr, an);
    assert(check(0, 0) == 1);
    assert(!path(0, 256, out) && !strcmp(out, "/private/var/mobile/Containers/Data/PluginKitPlugin/AB"));
    assert(path(0, 54, out) == ERANGE && !path(0, 55, out));
    lookup_mode = 2; assert(!path(0, 256, out)); lookup_mode = 0;                  /* no vnode lookup needed */
    assert(path(200, 256, out) == ENOENT);                                          /* still never other pids */
    an = SPAWN("/private/var/containers/Bundle/X");                                 /* re-exec replaces */
    b2l_sandbox_container_record(self.uniqueid, attr, an);
    assert(!path(0, 256, out) && !strcmp(out, "/private/var/containers/Bundle/X"));
    b2l_sandbox_container_record(self.uniqueid, NULL, 0);                            /* exec without one clears */
    assert(path(0, 256, out) == ENOENT && check(0, 0) == 0);
    b2l_sandbox_container_record(self.uniqueid, attr, an);
    other.ent = 0; b2l_sandbox_container_record(other.uniqueid, attr, an);
    assert(check(200, 0) == 1);                                                     /* other pid identity via container */
    b2l_sandbox_container_forget(other.uniqueid); assert(check(200, 0) == 0); other.ent = 1;
    b2l_sandbox_container_forget(self.uniqueid); assert(path(0, 256, out) == ENOENT);
    b2l_sandbox_container_record(0, attr, an); assert(b2l_sandbox_container_lookup(0, out) == 0);

    /* Full table evicts the oldest record; lookups stay exact. */
    for (uint64_t i = 1; i <= B2L_SANDBOX_CONTAINERS + 5; i++) {
        char pathbuf[64]; snprintf(pathbuf, sizeof(pathbuf), "/c/%llu", (unsigned long long)i);
        an = SPAWN(pathbuf); b2l_sandbox_container_record(5000 + i, attr, an);
    }
    for (uint64_t i = 1; i <= 5; i++) assert(b2l_sandbox_container_lookup(5000 + i, out) == 0);
    assert(b2l_sandbox_container_lookup(5006, out) && !strcmp(out, "/c/6"));
    assert(b2l_sandbox_container_lookup(5000 + B2L_SANDBOX_CONTAINERS + 5, out) && !locked && traces);
    puts("Sandbox container: spawn attribute bounds, record/replace/clear/forget, eviction, identity and op 4 from spawn PASS");
    puts("Sandbox identity: entitled-only self/other identity, permission queries unchanged, HOME container path bounds/directory/ownership PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as td:
    d = pathlib.Path(td)
    (d / 't.c').write_text(PROGRAM)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-fsanitize=address,undefined', '-I' + str(HEADER.parent), str(d / 't.c'), '-o', str(d / 't')], check=True)
    subprocess.run([str(d / 't')], check=True)
