/* SPDX-License-Identifier: BSD-2-Clause
 * Offline ARM64e -> ARM64 precompiler for the B2L stripped-pointer development ABI.
 *
 * The device kernel learns trapped instructions at runtime and rewrites the
 * owning root-file pages on the next boot (docs/native-cache.md). That cycle
 * is bounded by the trap cost of every not-yet-learned site. This host tool
 * applies exactly the same allowlisted converter, b2l_nc_native() from
 * common/cache/b2l_cache_core.c, to every pure-instruction section of every
 * ARM64e Mach-O slice and dyld shared-cache image under a prepared root,
 * before the image is flashed. Nothing else in a file is changed: sizes,
 * UUIDs, load commands, data, strings, unwind tables and code signatures stay
 * byte-identical. PACGA, LDRAA/LDRAB and unknown instructions are reported but
 * never rewritten; they keep using the kernel emulator.
 *
 * Code words are selected with the kernel's own definition: a section flagged
 * S_ATTR_PURE_INSTRUCTIONS inside a segment whose initial protection includes
 * execute (xnu-overlay/bsd/kern/b2l_apps_payload_compare.h). Shared-cache
 * images are located through the main cache image table and translated through
 * exactly one executable mapping of the owning (sub)cache file, mirroring
 * b2l_cache_exclusion.h. The integrity-measured corecrypto __TEXT,__text stays
 * original by default, as it does on the device.
 *
 * Build:
 *   clang -O2 -std=c11 -Wall -Wextra -Werror -Icommon/cache -Ixnu-overlay/osfmk/arm64 \
 *       tools/b2l-precompile.c common/cache/b2l_cache_core.c common/cache/CacheSha.c \
 *       -o out/b2l-precompile
 *
 * The default run only reports. --write rewrites files in place through
 * pwrite(2), preserving inodes and hard links, and reads every written section
 * back before counting it as converted. See docs/arm64e-precompile.md.
 */
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "b2l_cache_core.h"
#include "b2l_pacga_keyless.h"

#define MH_MAGIC_64 0xfeedfacfu
#define FAT_MAGIC 0xcafebabeu
#define FAT_MAGIC_64 0xcafebabfu
#define CPU_TYPE_ARM64 0x0100000cu
#define CPU_SUBTYPE_ARM64E 2u
#define LC_SEGMENT_64 0x19u
#define VM_PROT_EXECUTE 4u
#define S_ATTR_PURE_INSTRUCTIONS 0x80000000u
#define SECTION_TYPE_MASK 0xffu
#define S_ZEROFILL 1u
#define S_GB_ZEROFILL 0xcu
#define S_THREAD_LOCAL_ZEROFILL 0x12u

#define MAX_LOAD_COMMANDS 4096u
#define MAX_SIZEOFCMDS (4u * 1024u * 1024u)
#define MAX_SECTION_BYTES (1024u * 1024u * 1024u)
#define MAX_SECTIONS 65536u
#define MAX_CACHE_MAPPINGS 1024u
#define MAX_CACHE_FILES 512u
#define MAX_CACHE_IMAGES 65536u
#define MAX_PATH_BYTES 4096u

enum { CLASS_NONE, CLASS_PAC, CLASS_HINT, CLASS_RET, CLASS_BRANCH, CLASS_LDAPR, CLASS_COUNT };
static const char *const class_names[CLASS_COUNT] = {"none", "pac_identity", "hint_nop", "ret", "branch", "ldapr"};

struct counts {
    uint64_t words, converted, by_class[CLASS_COUNT], excluded, pacga, ldra, already;
};

struct section_ref {
    uint64_t addr;         /* unslid VM address of offset (cache) or 0 */
    int cave;              /* all-zero stub-island page usable for PACGA routines */
    char segname[17], sectname[17];
    uint64_t offset, size; /* absolute file offset within the owning file */
    unsigned file;         /* index into the cache group, 0 for a plain Mach-O */
    int excluded;
};

struct options {
    int write, verify, quiet, xdev, pacga;
    const char *report;
    const char **exclude;
    unsigned excludes;
};

struct totals {
    uint64_t files_seen, machos, slices_arm64e, slices_other, fat, caches, cache_files, images,
        sections, changed_sections, written_bytes, errors, subcaches_deferred, bad_magic, stub_regions, skipped_regions,
        pacga_routed, pacga_nop, pacga_kept, caves;
    struct counts c;
};

struct report {
    FILE *stream;
    int first;
};

static struct options opt;
static struct totals totals;
static struct report report;

static void note(const char *fmt, ...)
{
    va_list ap;
    if (opt.quiet) return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static void fail(const char *path, const char *fmt, ...)
{
    va_list ap;
    totals.errors++;
    fprintf(stderr, "error: %s: ", path);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- converter classification ------------------------------------------ */

static int classify(uint32_t i, uint32_t *out)
{
    if (!b2l_nc_native(i, out)) return CLASS_NONE;
    if ((i & 0xffff0000u) == 0xdac10000u) return CLASS_PAC;
    if ((i & 0xffffff00u) == 0xd5032000u || (i & 0xffffff00u) == 0xd5032100u ||
        (i & 0xffffff00u) == 0xd5032300u) return CLASS_HINT;
    if (i == 0xd65f0bffu || i == 0xd65f0fffu) return CLASS_RET;
    if ((i & 0x3ffffc00u) == 0x38bfc000u) return CLASS_LDAPR;
    return CLASS_BRANCH;
}

static int is_pacga(uint32_t i) { return (i & 0xffe0fc00u) == 0x9ac03000u; }
static int is_ldra(uint32_t i) { return (i & 0xff200400u) == 0xf8200400u; }

/* Rewrite one section buffer; returns the number of changed words. */
static uint64_t convert_buffer(uint8_t *buf, uint64_t bytes, int excluded, struct counts *c)
{
    uint64_t changed = 0;
    for (uint64_t at = 0; at + 4 <= bytes; at += 4) {
        uint32_t word = b2l_nc_u32(buf + at), native;
        int cls = classify(word, &native);
        c->words++;
        if (cls == CLASS_NONE) {
            if (is_pacga(word)) c->pacga++;
            else if (is_ldra(word)) c->ldra++;
            continue;
        }
        if (excluded) { c->excluded++; continue; }
        c->converted++;
        c->by_class[cls]++;
        b2l_nc_p32(buf + at, native);
        changed++;
    }
    return changed;
}

static void counts_add(struct counts *to, const struct counts *from)
{
    to->words += from->words;
    to->converted += from->converted;
    for (unsigned i = 0; i < CLASS_COUNT; i++) to->by_class[i] += from->by_class[i];
    to->excluded += from->excluded;
    to->pacga += from->pacga;
    to->ldra += from->ldra;
    to->already += from->already;
}

/* ---- bounded I/O --------------------------------------------------------- */

static int read_exact(int fd, uint64_t off, void *out, size_t n, uint64_t limit)
{
    uint8_t *p = out;
    if (off > limit || n > limit - off) return -1;
    while (n) {
        ssize_t got = pread(fd, p, n, (off_t)off);
        if (got < 0) { if (errno == EINTR) continue; return -1; }
        if (got == 0) return -1;
        p += got; off += (uint64_t)got; n -= (size_t)got;
    }
    return 0;
}

static int write_exact(int fd, uint64_t off, const void *in, size_t n)
{
    const uint8_t *p = in;
    while (n) {
        ssize_t put = pwrite(fd, p, n, (off_t)off);
        if (put < 0) { if (errno == EINTR) continue; return -1; }
        if (put == 0) return -1;
        p += put; off += (uint64_t)put; n -= (size_t)put;
    }
    return 0;
}

/* ---- JSON report --------------------------------------------------------- */

static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { fputc('\\', f); fputc(ch, f); }
        else if (ch < 0x20) fprintf(f, "\\u%04x", ch);
        else fputc(ch, f);
    }
    fputc('"', f);
}

static void json_counts(FILE *f, const struct counts *c)
{
    fprintf(f, "{\"words\":%" PRIu64 ",\"converted\":%" PRIu64 ",\"excluded\":%" PRIu64
               ",\"pacga\":%" PRIu64 ",\"ldra\":%" PRIu64 ",\"classes\":{",
            c->words, c->converted, c->excluded, c->pacga, c->ldra);
    for (unsigned i = CLASS_PAC; i < CLASS_COUNT; i++)
        fprintf(f, "%s\"%s\":%" PRIu64, i == CLASS_PAC ? "" : ",", class_names[i], c->by_class[i]);
    fputs("}}", f);
}

static void report_entry(const char *path, const char *kind, const char *status, unsigned sections,
                         unsigned changed_sections, const struct counts *c)
{
    FILE *f = report.stream;
    if (!f) return;
    fprintf(f, "%s\n  {\"path\":", report.first ? "" : ",");
    json_string(f, path);
    fprintf(f, ",\"kind\":\"%s\",\"status\":\"%s\",\"sections\":%u,\"changed_sections\":%u,\"counts\":",
            kind, status, sections, changed_sections);
    json_counts(f, c);
    fputc('}', f);
    report.first = 0;
}

/* ---- exclusion ----------------------------------------------------------- */

static int path_excluded(const char *path)
{
    if (!path) return 0;
    while (*path == '/') path++;
    for (unsigned i = 0; i < opt.excludes; i++) {
        const char *x = opt.exclude[i];
        while (*x == '/') x++;
        if (!strcmp(path, x)) return 1;
    }
    return 0;
}

static int section_excluded(const char *image_path, const char *segname, const char *sectname)
{
    return path_excluded(image_path) && !strcmp(segname, "__TEXT") && !strcmp(sectname, "__text");
}

/* ---- Mach-O load command walk -------------------------------------------- */

struct exec_range { uint64_t start, end; };

struct macho_ctx {
    const char *path;        /* diagnostic path */
    const char *image_path;  /* exclusion path (root-relative or cache image path) */
    struct section_ref *sections;
    unsigned count, limit;
    int ok;
    struct exec_range **ranges;  /* optional: executable segment VM ranges collected here */
    unsigned *nranges;
};

/* resolve() maps a (vmaddr, size) or (fileoff, size) to an absolute file offset and file index. */
typedef int (*resolve_fn)(void *ctx, uint64_t addr, uint64_t fileoff, uint64_t size, unsigned *file, uint64_t *out);

static int walk_load_commands(const uint8_t *header, const uint8_t *cmds, uint32_t ncmds, uint32_t sizeofcmds,
                              struct macho_ctx *m, resolve_fn resolve, void *rctx)
{
    uint64_t pos = 0;
    (void)header;
    for (uint32_t i = 0; i < ncmds; i++) {
        if (pos > sizeofcmds || sizeofcmds - pos < 8) { fail(m->path, "truncated load command %u", i); return -1; }
        uint32_t cmd = b2l_nc_u32(cmds + pos), n = b2l_nc_u32(cmds + pos + 4);
        if (n < 8 || (n & 7) || n > sizeofcmds - pos) { fail(m->path, "malformed load command %u", i); return -1; }
        if (cmd == LC_SEGMENT_64) {
            const uint8_t *lc = cmds + pos;
            if (n < 72) { fail(m->path, "short segment command %u", i); return -1; }
            char segname[17] = {0};
            memcpy(segname, lc + 8, 16);
            uint64_t segva = b2l_nc_u64(lc + 24), segvm = b2l_nc_u64(lc + 32);
            uint64_t segoff = b2l_nc_u64(lc + 40), segsize = b2l_nc_u64(lc + 48);
            uint32_t initprot = b2l_nc_u32(lc + 60), nsects = b2l_nc_u32(lc + 64);
            if (nsects > (n - 72) / 80) { fail(m->path, "segment %s section count exceeds command", segname); return -1; }
            if (segva > UINT64_MAX - segvm || segoff > UINT64_MAX - segsize) { fail(m->path, "segment %s range overflow", segname); return -1; }
            if (m->ranges && (initprot & VM_PROT_EXECUTE) && segvm) {
                struct exec_range *grown = realloc(*m->ranges, (*m->nranges + 1) * sizeof(**m->ranges));
                if (!grown) { fail(m->path, "out of memory"); return -1; }
                *m->ranges = grown;
                grown[*m->nranges].start = segva; grown[*m->nranges].end = segva + segvm;
                (*m->nranges)++;
            }
            for (uint32_t j = 0; j < nsects; j++) {
                const uint8_t *s = lc + 72 + (size_t)j * 80;
                char sectname[17] = {0};
                memcpy(sectname, s, 16);
                uint64_t addr = b2l_nc_u64(s + 32), size = b2l_nc_u64(s + 40);
                uint32_t offset = b2l_nc_u32(s + 48), flags = b2l_nc_u32(s + 64), type = flags & SECTION_TYPE_MASK;
                if (!(initprot & VM_PROT_EXECUTE) || !(flags & S_ATTR_PURE_INSTRUCTIONS)) continue;
                if (type == S_ZEROFILL || type == S_GB_ZEROFILL || type == S_THREAD_LOCAL_ZEROFILL) continue;
                if (!size) continue;
                if ((addr & 3) || (size & 3) || (offset & 3)) { fail(m->path, "%s,%s is not word aligned", segname, sectname); return -1; }
                if (size > MAX_SECTION_BYTES) { fail(m->path, "%s,%s exceeds the section limit", segname, sectname); return -1; }
                if (m->count >= m->limit) { fail(m->path, "too many code sections"); return -1; }
                struct section_ref *ref = &m->sections[m->count];
                memset(ref, 0, sizeof(*ref));
                memcpy(ref->segname, segname, 17);
                memcpy(ref->sectname, sectname, 17);
                ref->size = size;
                ref->excluded = section_excluded(m->image_path, segname, sectname);
                ref->addr = addr;
                if (resolve(rctx, addr, offset, size, &ref->file, &ref->offset)) {
                    fail(m->path, "%s,%s (va 0x%" PRIx64 ", %" PRIu64 " bytes) is outside one executable mapping",
                         segname, sectname, addr, size);
                    return -1;
                }
                m->count++;
            }
        }
        pos += n;
    }
    if (pos != sizeofcmds) { fail(m->path, "load commands do not fill sizeofcmds"); return -1; }
    return 0;
}

/* Returns 1 for an ARM64e Mach-O header, 0 for another architecture, -1 for malformed input. */
static int parse_macho_header(const uint8_t *h, uint32_t *ncmds, uint32_t *sizeofcmds)
{
    if (b2l_nc_u32(h) != MH_MAGIC_64) return -1;
    *ncmds = b2l_nc_u32(h + 16);
    *sizeofcmds = b2l_nc_u32(h + 20);
    if (b2l_nc_u32(h + 4) != CPU_TYPE_ARM64 || (b2l_nc_u32(h + 8) & 0xffu) != CPU_SUBTYPE_ARM64E) return 0;
    if (!*ncmds || *ncmds > MAX_LOAD_COMMANDS || *sizeofcmds > MAX_SIZEOFCMDS) return -1;
    return 1;
}

/* ---- section rewrite ----------------------------------------------------- */

struct file_handle { int fd; uint64_t bytes; const char *path; int writable; };

/* Reopen read-write only once a section actually needs writing, so read-only
 * files with nothing to convert never fail on permissions. */
static int ensure_writable(struct file_handle *f)
{
    struct stat before, after;
    int fd;
    if (f->writable) return 0;
    if (fstat(f->fd, &before)) { fail(f->path, "fstat: %s", strerror(errno)); return -1; }
    fd = open(f->path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { fail(f->path, "open for writing: %s", strerror(errno)); return -1; }
    if (fstat(fd, &after) || after.st_dev != before.st_dev || after.st_ino != before.st_ino || (uint64_t)after.st_size != f->bytes) {
        fail(f->path, "file changed while reopening for writing"); close(fd); return -1;
    }
    close(f->fd); f->fd = fd; f->writable = 1;
    return 0;
}

/* Redirect PACGA in shared-cache image text to keyless EL0 routines placed in
 * all-zero stub-island pages within branch range (+/-128 MiB both ways). Sites
 * with an SP operand, XZR destination handled as NOP, excluded sections and
 * sites without a reachable cave keep trapping; the kernel computes the same
 * keyless function for them. */
static int branch_ok(int64_t delta) { return !(delta & 3) && delta >= -(INT64_C(1) << 27) && delta < (INT64_C(1) << 27); }
static uint32_t branch(int64_t delta) { return UINT32_C(0x14000000) | ((uint32_t)(delta >> 2) & UINT32_C(0x03ffffff)); }

static void pacga_trampolines(struct section_ref *refs, unsigned count, uint8_t **buffers, uint64_t *changed)
{
    uint32_t *used = calloc(count ? count : 1, sizeof(*used));
    if (!used) { fail("pacga", "out of memory"); return; }
    for (unsigned i = 0; i < count; i++) {
        struct section_ref *r = &refs[i];
        if (r->cave || !buffers[i] || !strcmp(r->sectname, "cache_stubs")) continue;
        for (uint64_t at = 0; at + 4 <= r->size; at += 4) {
            uint32_t w = b2l_nc_u32(buffers[i] + at), seq[B2L_PACGA_KEYLESS_WORDS];
            unsigned d = w & 31u, n = (w >> 5) & 31u, m = (w >> 16) & 31u;
            uint64_t site = r->addr + at;
            int placed = 0;
            if (!is_pacga(w)) continue;
            if (r->excluded) { totals.pacga_kept++; continue; }
            if (d == 31) { b2l_nc_p32(buffers[i] + at, 0xd503201fu); changed[i]++; totals.pacga_nop++; continue; }
            if (!b2l_pacga_keyless_encode(d, n, m, seq)) { totals.pacga_kept++; continue; }
            for (unsigned j = 0; j < count && !placed; j++) {
                uint64_t slot;
                if (!refs[j].cave || !buffers[j] || used[j] + 32u > refs[j].size) continue;
                slot = refs[j].addr + used[j];
                if (!branch_ok((int64_t)(slot - site)) || !branch_ok((int64_t)((site + 4) - (slot + 28)))) continue;
                for (unsigned k = 0; k < B2L_PACGA_KEYLESS_WORDS; k++) b2l_nc_p32(buffers[j] + used[j] + 4 * k, seq[k]);
                b2l_nc_p32(buffers[j] + used[j] + 28, branch((int64_t)((site + 4) - (slot + 28))));
                b2l_nc_p32(buffers[i] + at, branch((int64_t)(slot - site)));
                used[j] += 32u; changed[j]++; changed[i]++; placed = 1;
                totals.pacga_routed++;
            }
            if (!placed) totals.pacga_kept++;
        }
    }
    free(used);
}

/* Phase 1: read and convert every section into memory; phase 2: write back and verify. */
static int rewrite_sections(struct file_handle *files, unsigned nfiles, struct section_ref *refs, unsigned count,
                            struct counts *c, unsigned *changed_sections, const char *path, int pacga)
{
    uint8_t **buffers = calloc(count ? count : 1, sizeof(*buffers));
    uint64_t *changed = calloc(count ? count : 1, sizeof(*changed));
    int rc = 0;
    if (!buffers || !changed) { fail(path, "out of memory"); free(buffers); free(changed); return -1; }
    for (unsigned i = 0; i < count && !rc; i++) {
        struct section_ref *r = &refs[i];
        struct file_handle *f;
        if (r->file >= nfiles) { fail(path, "section file index out of range"); rc = -1; break; }
        f = &files[r->file];
        if (r->offset > f->bytes || r->size > f->bytes - r->offset) {
            fail(f->path, "%s,%s exceeds the file", r->segname, r->sectname); rc = -1; break;
        }
        buffers[i] = malloc((size_t)r->size);
        if (!buffers[i]) { fail(path, "out of memory for %s,%s", r->segname, r->sectname); rc = -1; break; }
        if (read_exact(f->fd, r->offset, buffers[i], (size_t)r->size, f->bytes)) {
            fail(f->path, "read %s,%s: %s", r->segname, r->sectname, strerror(errno)); rc = -1; break;
        }
        struct counts sc = {0};
        changed[i] = convert_buffer(buffers[i], r->size, r->excluded, &sc);
        counts_add(c, &sc);
        if (!r->cave) totals.sections++;
    }
    if (!rc && pacga) pacga_trampolines(refs, count, buffers, changed);
    for (unsigned i = 0; i < count && !rc; i++) if (changed[i]) (*changed_sections)++;
    for (unsigned i = 0; i < count && !rc && opt.write; i++) {
        struct section_ref *r = &refs[i];
        struct file_handle *f = &files[r->file];
        if (!changed[i]) continue;
        if (ensure_writable(f)) { rc = -1; break; }
        if (write_exact(f->fd, r->offset, buffers[i], (size_t)r->size)) {
            fail(f->path, "write %s,%s: %s", r->segname, r->sectname, strerror(errno)); rc = -1; break;
        }
        totals.written_bytes += r->size;
    }
    if (!rc && opt.write) {
        for (unsigned i = 0; i < nfiles; i++)
            if (files[i].writable && fsync(files[i].fd)) { fail(files[i].path, "fsync: %s", strerror(errno)); rc = -1; }
    }
    for (unsigned i = 0; i < count && !rc && opt.write; i++) {
        struct section_ref *r = &refs[i];
        struct file_handle *f = &files[r->file];
        uint8_t *back;
        if (!changed[i]) continue;
        back = malloc((size_t)r->size);
        if (!back) { fail(path, "out of memory"); rc = -1; break; }
        if (read_exact(f->fd, r->offset, back, (size_t)r->size, f->bytes) || memcmp(back, buffers[i], (size_t)r->size)) {
            fail(f->path, "readback of %s,%s does not match the written bytes", r->segname, r->sectname); rc = -1;
        }
        free(back);
    }
    for (unsigned i = 0; i < count; i++) free(buffers[i]);
    free(buffers);
    free(changed);
    return rc;
}

/* ---- plain Mach-O files -------------------------------------------------- */

struct plain_resolver { uint64_t base, size; };

static int plain_resolve(void *ctx, uint64_t addr, uint64_t fileoff, uint64_t size, unsigned *file, uint64_t *out)
{
    struct plain_resolver *p = ctx;
    (void)addr;
    if (fileoff > p->size || size > p->size - fileoff) return -1;
    *file = 0;
    *out = p->base + fileoff;
    return 0;
}

static int process_slice(struct file_handle *f, uint64_t base, uint64_t size, const char *image_path,
                         struct section_ref *refs, unsigned limit, unsigned *count)
{
    uint8_t h[32];
    uint32_t ncmds, sizeofcmds;
    uint8_t *cmds;
    struct plain_resolver pr = {base, size};
    struct macho_ctx m = {f->path, image_path, refs, 0, limit, 1, NULL, NULL};
    int kind, rc;
    if (size < 32 || base > f->bytes || size > f->bytes - base) { fail(f->path, "slice exceeds the file"); return -1; }
    if (read_exact(f->fd, base, h, 32, f->bytes)) { fail(f->path, "read header: %s", strerror(errno)); return -1; }
    kind = parse_macho_header(h, &ncmds, &sizeofcmds);
    if (kind < 0) { fail(f->path, "malformed Mach-O header"); return -1; }
    if (kind == 0) { totals.slices_other++; *count = 0; return 0; }
    totals.slices_arm64e++;
    if (sizeofcmds > size - 32) { fail(f->path, "load commands exceed the slice"); return -1; }
    cmds = malloc(sizeofcmds ? sizeofcmds : 1);
    if (!cmds) { fail(f->path, "out of memory"); return -1; }
    if (read_exact(f->fd, base + 32, cmds, sizeofcmds, f->bytes)) { fail(f->path, "read load commands: %s", strerror(errno)); free(cmds); return -1; }
    rc = walk_load_commands(h, cmds, ncmds, sizeofcmds, &m, plain_resolve, &pr);
    free(cmds);
    *count = m.count;
    return rc;
}

static int process_macho(struct file_handle *f, const char *image_path)
{
    uint8_t magic[8];
    struct section_ref *refs = NULL;
    unsigned count = 0, changed = 0, limit = MAX_SECTIONS;
    struct counts c = {0};
    int rc = 0;
    const char *kind = "macho";
    if (read_exact(f->fd, 0, magic, 8, f->bytes)) { fail(f->path, "read magic: %s", strerror(errno)); return -1; }
    refs = calloc(limit, sizeof(*refs));
    if (!refs) { fail(f->path, "out of memory"); return -1; }
    totals.machos++;
    if (b2l_nc_u32(magic) == MH_MAGIC_64) {
        rc = process_slice(f, 0, f->bytes, image_path, refs, limit, &count);
    } else {
        /* fat headers are big-endian */
        uint32_t fm = (uint32_t)magic[0] << 24 | (uint32_t)magic[1] << 16 | (uint32_t)magic[2] << 8 | magic[3];
        uint32_t narch = (uint32_t)magic[4] << 24 | (uint32_t)magic[5] << 16 | (uint32_t)magic[6] << 8 | magic[7];
        unsigned entry = fm == FAT_MAGIC ? 20 : 32;
        kind = "fat";
        totals.fat++;
        if (!narch || narch > 64) { fail(f->path, "unsupported fat architecture count %u", narch); rc = -1; }
        for (uint32_t i = 0; i < narch && !rc; i++) {
            uint8_t a[32];
            uint64_t off, size;
            unsigned n = 0;
            if (read_exact(f->fd, 8 + (uint64_t)i * entry, a, entry, f->bytes)) { fail(f->path, "read fat arch %u", i); rc = -1; break; }
            uint32_t cputype = (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
            uint32_t subtype = (uint32_t)a[4] << 24 | (uint32_t)a[5] << 16 | (uint32_t)a[6] << 8 | a[7];
            if (entry == 20) {
                off = (uint32_t)a[8] << 24 | (uint32_t)a[9] << 16 | (uint32_t)a[10] << 8 | a[11];
                size = (uint32_t)a[12] << 24 | (uint32_t)a[13] << 16 | (uint32_t)a[14] << 8 | a[15];
            } else {
                off = 0; size = 0;
                for (unsigned k = 0; k < 8; k++) { off = off << 8 | a[8 + k]; size = size << 8 | a[16 + k]; }
            }
            if (cputype != CPU_TYPE_ARM64 || (subtype & 0xffu) != CPU_SUBTYPE_ARM64E) { totals.slices_other++; continue; }
            if (count >= limit) { fail(f->path, "too many code sections"); rc = -1; break; }
            rc = process_slice(f, off, size, image_path, refs + count, limit - count, &n);
            count += n;
        }
    }
    if (!rc) rc = rewrite_sections(f, 1, refs, count, &c, &changed, f->path, 0);
    counts_add(&totals.c, &c);
    totals.changed_sections += changed;
    report_entry(f->path, kind, rc ? "error" : (changed ? (opt.write ? "written" : "pending") : "clean"), count, changed, &c);
    if (!rc && count && !opt.quiet && (c.converted || c.excluded))
        note("%s: %u code sections, %" PRIu64 " words, %" PRIu64 " %s, %" PRIu64 " excluded, %" PRIu64 " pacga, %" PRIu64 " ldra\n",
             f->path, count, c.words, c.converted, opt.write ? "converted" : "convertible", c.excluded, c.pacga, c.ldra);
    free(refs);
    return rc;
}

/* ---- dyld shared cache groups -------------------------------------------- */

struct cache_mapping { uint64_t address, size, offset; uint32_t initprot; unsigned file; int stubs; /* dyld flag 0x8; -1 unknown */ };

struct cache_group {
    struct file_handle files[MAX_CACHE_FILES];
    unsigned nfiles;
    struct cache_mapping *maps;
    unsigned nmaps;
    const char *path;
};

static int cache_translate(const struct cache_group *g, uint64_t va, uint64_t n, unsigned *file, uint64_t *out)
{
    unsigned found = 0;
    if (!n || va > UINT64_MAX - n) return -1;
    for (unsigned i = 0; i < g->nmaps; i++) {
        const struct cache_mapping *m = &g->maps[i];
        if (va >= m->address && va - m->address < m->size && n <= m->size - (va - m->address)) {
            if (found++ || !(m->initprot & VM_PROT_EXECUTE)) return -1;
            *file = m->file;
            *out = m->offset + (va - m->address);
        }
    }
    return found == 1 ? 0 : -1;
}

static int cache_resolve(void *ctx, uint64_t addr, uint64_t fileoff, uint64_t size, unsigned *file, uint64_t *out)
{
    (void)fileoff;
    return cache_translate(ctx, addr, size, file, out);
}

static int cache_add_mappings(struct cache_group *g, unsigned file, const uint8_t *h)
{
    struct file_handle *f = &g->files[file];
    uint32_t at = b2l_nc_u32(h + 16), count = b2l_nc_u32(h + 20);
    if (!count || count > MAX_CACHE_MAPPINGS || at > f->bytes || count > (f->bytes - at) / 32) { fail(f->path, "malformed mapping table"); return -1; }
    struct cache_mapping *grown = realloc(g->maps, (g->nmaps + count) * sizeof(*grown));
    if (!grown) { fail(f->path, "out of memory"); return -1; }
    g->maps = grown;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t m[32];
        if (read_exact(f->fd, (uint64_t)at + i * 32, m, 32, f->bytes)) { fail(f->path, "read mapping %u", i); return -1; }
        struct cache_mapping *cm = &g->maps[g->nmaps];
        cm->address = b2l_nc_u64(m); cm->size = b2l_nc_u64(m + 8); cm->offset = b2l_nc_u64(m + 16);
        cm->initprot = b2l_nc_u32(m + 28); cm->file = file; cm->stubs = -1;
        /* dyld_cache_mapping_and_slide_info (header 0x138/0x13c, 56-byte entries, flags at +40):
         * DYLD_CACHE_MAPPING_TEXT_STUBS = 0x8 marks the shared stub islands. */
        if (b2l_nc_u32(h + 16) >= 0x140) {
            uint32_t sat = b2l_nc_u32(h + 0x138), scount = b2l_nc_u32(h + 0x13c);
            if (scount && scount <= MAX_CACHE_MAPPINGS && sat <= f->bytes && scount <= (f->bytes - sat) / 56) {
                for (uint32_t k = 0; k < scount; k++) {
                    uint8_t e[56];
                    if (read_exact(f->fd, (uint64_t)sat + k * 56, e, 56, f->bytes)) break;
                    if (b2l_nc_u64(e) == cm->address && b2l_nc_u64(e + 8) == cm->size) { cm->stubs = (b2l_nc_u64(e + 40) & 8) ? 1 : 0; break; }
                }
            }
        }
        if (!cm->size || cm->address > UINT64_MAX - cm->size || cm->offset > f->bytes || cm->size > f->bytes - cm->offset) {
            fail(f->path, "mapping %u exceeds the file", i); return -1;
        }
        g->nmaps++;
    }
    return 0;
}

static int open_handle(struct file_handle *f, const char *path)
{
    struct stat st;
    f->path = path; f->writable = 0;
    f->fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (f->fd < 0) { fail(path, "open: %s", strerror(errno)); return -1; }
    if (fstat(f->fd, &st) || !S_ISREG(st.st_mode)) { fail(path, "not a regular file"); close(f->fd); f->fd = -1; return -1; }
    f->bytes = (uint64_t)st.st_size;
    return 0;
}

static int cache_is_main(const uint8_t *h)
{
    uint32_t mapping_offset = b2l_nc_u32(h + 16);
    if (mapping_offset >= 0x98 && b2l_nc_u64(h + 0x90)) return 1;
    if (mapping_offset >= 0x1c8 && b2l_nc_u32(h + 0x1c4)) return 1;
    if (mapping_offset < 0x98 && b2l_nc_u32(h + 0x1c)) return 1;
    return 0;
}

static int cache_open_subcaches(struct cache_group *g, const uint8_t *h)
{
    struct file_handle *main = &g->files[0];
    uint32_t mapping_offset = b2l_nc_u32(h + 16);
    if (mapping_offset < 0x190) return 0; /* no subcache array in this format */
    uint32_t at = b2l_nc_u32(h + 0x188), count = b2l_nc_u32(h + 0x18c);
    unsigned entry = mapping_offset <= 0x1c8 ? 24 : 56;
    if (!count) return 0;
    if (count >= MAX_CACHE_FILES || at > main->bytes || count > (main->bytes - at) / entry) { fail(main->path, "malformed subcache array"); return -1; }
    size_t base_len = strlen(main->path);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t e[56], sh[0x200];
        char suffix[33] = {0}, *path;
        if (read_exact(main->fd, (uint64_t)at + i * entry, e, entry, main->bytes)) { fail(main->path, "read subcache entry %u", i); return -1; }
        if (entry == 56) memcpy(suffix, e + 24, 32);
        else snprintf(suffix, sizeof(suffix), ".%u", i + 1);
        if (!suffix[0] || strchr(suffix, '/')) { fail(main->path, "subcache entry %u has an unusable suffix", i); return -1; }
        path = malloc(base_len + strlen(suffix) + 1);
        if (!path) { fail(main->path, "out of memory"); return -1; }
        memcpy(path, main->path, base_len);
        strcpy(path + base_len, suffix);
        struct file_handle *f = &g->files[g->nfiles];
        if (open_handle(f, path)) { free(path); return -1; }
        g->nfiles++;
        if (read_exact(f->fd, 0, sh, sizeof(sh), f->bytes) || memcmp(sh, "dyld_v1", 7) || memcmp(sh + 0x58, e, 16)) {
            fail(path, "subcache header or UUID does not match the main cache"); return -1;
        }
        if (cache_add_mappings(g, g->nfiles - 1, sh)) return -1;
    }
    return 0;
}

struct cache_image { uint64_t address; char path[MAX_PATH_BYTES]; };

static int cache_read_path(struct file_handle *main, uint64_t at, char *out)
{
    size_t n = 0;
    while (n + 1 < MAX_PATH_BYTES) {
        size_t chunk = MAX_PATH_BYTES - 1 - n;
        if (at + n >= main->bytes) return -1;
        if (chunk > main->bytes - (at + n)) chunk = (size_t)(main->bytes - (at + n));
        if (chunk > 256) chunk = 256;
        if (read_exact(main->fd, at + n, out + n, chunk, main->bytes)) return -1;
        for (size_t i = 0; i < chunk; i++) if (!out[n + i]) return 0;
        n += chunk;
    }
    return -1;
}

static int cache_read_images(struct cache_group *g, const uint8_t *h, struct cache_image **out, unsigned *count)
{
    struct file_handle *main = &g->files[0];
    uint32_t mapping_offset = b2l_nc_u32(h + 16);
    uint64_t at, n;
    unsigned entry = 32, addr_at, path_at;
    if (mapping_offset >= 0x98 && b2l_nc_u64(h + 0x90)) {
        at = b2l_nc_u64(h + 0x88); n = b2l_nc_u64(h + 0x90); addr_at = 16; path_at = 28;
    } else if (mapping_offset >= 0x1c8 && b2l_nc_u32(h + 0x1c4)) {
        at = b2l_nc_u32(h + 0x1c0); n = b2l_nc_u32(h + 0x1c4); addr_at = 0; path_at = 24;
    } else {
        at = b2l_nc_u32(h + 0x18); n = b2l_nc_u32(h + 0x1c); addr_at = 0; path_at = 24;
    }
    if (!n || n > MAX_CACHE_IMAGES || at > main->bytes || n > (main->bytes - at) / entry) { fail(main->path, "malformed image table"); return -1; }
    *out = calloc((size_t)n + 1, sizeof(**out));
    if (!*out) { fail(main->path, "out of memory"); return -1; }
    for (uint64_t i = 0; i < n; i++) {
        uint8_t e[32];
        struct cache_image *img = &(*out)[i];
        if (read_exact(main->fd, at + i * entry, e, entry, main->bytes)) { fail(main->path, "read image %" PRIu64, i); return -1; }
        img->address = b2l_nc_u64(e + addr_at);
        if (cache_read_path(main, b2l_nc_u32(e + path_at), img->path)) { fail(main->path, "image %" PRIu64 " path is unreadable", i); return -1; }
    }
    *count = (unsigned)n;
    /* dyld itself lives in the cache since iOS 16/macOS 13 but is not a dylib image. */
    if (mapping_offset >= 0x80) {
        uint64_t dyld = b2l_nc_u64(h + 0x78);
        int seen = 0;
        for (uint64_t i = 0; i < n && dyld; i++) if ((*out)[i].address == dyld) seen = 1;
        if (dyld && !seen) {
            (*out)[n].address = dyld;
            strcpy((*out)[n].path, "/usr/lib/dyld");
            *count = (unsigned)n + 1;
        }
    }
    return 0;
}

/* dyld's cache builder moves every image's __auth_stubs/__objc_stubs into
 * shared stub regions and leaves the per-image section headers empty. Those
 * regions are the only executable bytes outside every image's executable
 * segment, apart from cache headers. A region is converted only when every
 * word is a stub-shaped instruction or zero padding; header pages fail that
 * test and stay untouched. */
static int stub_word_ok(uint32_t w)
{
    uint32_t native;
    if (!w || w == 0xd503201fu) return 1;                       /* padding, NOP */
    if ((w & 0x9f000000u) == 0x90000000u) return 1;              /* ADRP */
    if ((w & 0xff800000u) == 0x91000000u) return 1;              /* ADD Xd, Xn, #imm */
    if ((w & 0xffc00000u) == 0xf9400000u) return 1;              /* LDR Xt, [Xn, #imm] */
    if ((w & 0xfc000000u) == 0x14000000u) return 1;              /* B */
    if ((w & 0xfc000000u) == 0x94000000u) return 1;              /* BL */
    if ((w & 0xfffffc1fu) == 0xd61f0000u) return 1;              /* BR Xn */
    if ((w & 0xffe0001fu) == 0xd4200000u) return 1;              /* BRK */
    return b2l_nc_native(w, &native) && (w & 0xfe000000u) == 0xd6000000u; /* BRAA/BRAB family */
}

static int range_compare(const void *a, const void *b)
{
    const struct exec_range *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static int cache_stub_regions(struct cache_group *g, struct exec_range *ranges, unsigned nranges,
                              struct section_ref *refs, unsigned limit, unsigned *count,
                              unsigned *regions, unsigned *skipped)
{
    qsort(ranges, nranges, sizeof(*ranges), range_compare);
    for (unsigned i = 0; i < g->nmaps; i++) {
        const struct cache_mapping *m = &g->maps[i];
        uint64_t cursor = m->address, end = m->address + m->size;
        if (!(m->initprot & VM_PROT_EXECUTE)) continue;
        /* Where dyld records the mapping kind, trust it: text mappings hold only
         * image segments, headers and padding, never shared stubs. Their
         * all-zero padding pages are still usable for PACGA routines. */
        int caves_only = m->stubs == 0;
        if (caves_only && !opt.pacga) continue;
        for (unsigned j = 0; j <= nranges; j++) {
            uint64_t gap_start = cursor, gap_end = j < nranges ? ranges[j].start : end;
            if (j < nranges) {
                if (ranges[j].end <= cursor) continue;
                if (ranges[j].start >= end) gap_end = end;
            }
            if (gap_end > end) gap_end = end;
            if (gap_end > gap_start && gap_end - gap_start >= 16 && !((gap_start | gap_end) & 3)) {
                /* Judge 4 KiB pages separately: a subcache header shares its gap
                 * with the stubs that follow it. Consecutive accepted pages with
                 * any content merge into one region; rejected pages are skipped. */
                struct file_handle *f = &g->files[m->file];
                uint64_t off = m->offset + (gap_start - m->address), n = gap_end - gap_start, run_start = 0, run_len = 0;
                uint8_t *buf = malloc((size_t)n);
                if (!buf) { fail(f->path, "out of memory"); return -1; }
                if (read_exact(f->fd, off, buf, (size_t)n, f->bytes)) { fail(f->path, "read stub region"); free(buf); return -1; }
                for (uint64_t at = 0; at < n;) {
                    uint64_t page_end = ((gap_start + at) | 4095u) + 1 - gap_start, chunk_end = page_end < n ? page_end : n;
                    int ok = 1, content = 0;
                    for (uint64_t w = at; w < chunk_end && ok; w += 4) {
                        uint32_t word = b2l_nc_u32(buf + w);
                        ok = stub_word_ok(word);
                        if (word) content = 1;
                    }
                    if (getenv("B2L_PRECOMPILE_DEBUG"))
                        fprintf(stderr, "gap %s va 0x%" PRIx64 "-0x%" PRIx64 " %s\n", f->path, gap_start + at, gap_start + chunk_end,
                                ok ? (content ? "stubs" : "zero") : "skipped");
                    if (!ok && caves_only) { at = chunk_end; continue; }
                    if (ok && !content && opt.pacga && m->stubs >= 0 && !((gap_start + at) & 4095u) && chunk_end - at == 4096u) {
                        if (*count >= limit) { fail(g->path, "too many code sections"); free(buf); return -1; }
                        struct section_ref *cave = &refs[(*count)++];
                        memset(cave, 0, sizeof(*cave));
                        strcpy(cave->segname, "__TEXT"); strcpy(cave->sectname, "pacga_cave");
                        cave->file = m->file; cave->offset = off + at; cave->size = 4096u;
                        cave->addr = gap_start + at; cave->cave = 1;
                        totals.caves++;
                    }
                    if (ok && content && !caves_only) {
                        if (!run_len) run_start = at;
                        run_len = chunk_end - run_start;
                    } else {
                        if (!ok) (*skipped)++;
                        if (run_len) {
                            if (*count >= limit) { fail(g->path, "too many code sections"); free(buf); return -1; }
                            struct section_ref *ref = &refs[(*count)++];
                            memset(ref, 0, sizeof(*ref));
                            strcpy(ref->segname, "__TEXT"); strcpy(ref->sectname, "cache_stubs");
                            ref->file = m->file; ref->offset = off + run_start; ref->size = run_len;
                            ref->addr = gap_start + run_start;
                            (*regions)++; run_len = 0;
                        }
                    }
                    at = chunk_end;
                }
                if (run_len) {
                    if (*count >= limit) { fail(g->path, "too many code sections"); free(buf); return -1; }
                    struct section_ref *ref = &refs[(*count)++];
                    memset(ref, 0, sizeof(*ref));
                    strcpy(ref->segname, "__TEXT"); strcpy(ref->sectname, "cache_stubs");
                    ref->file = m->file; ref->offset = off + run_start; ref->size = run_len;
                    ref->addr = gap_start + run_start;
                    (*regions)++;
                }
                free(buf);
            }
            if (j < nranges && ranges[j].end > cursor) cursor = ranges[j].end;
            if (j < nranges && ranges[j].start >= end) break;
        }
    }
    return 0;
}

static int process_cache_group(struct cache_group *g, const uint8_t *h)
{
    struct cache_image *images = NULL;
    struct section_ref *refs = NULL;
    unsigned nimages = 0, count = 0, changed = 0, limit = MAX_SECTIONS * 4, nranges = 0, regions = 0, skipped_regions = 0;
    struct exec_range *ranges = NULL;
    struct counts c = {0};
    int rc = 0;
    if (cache_open_subcaches(g, h)) rc = -1;
    if (!rc && cache_read_images(g, h, &images, &nimages)) rc = -1;
    if (!rc) {
        refs = calloc(limit, sizeof(*refs));
        if (!refs) { fail(g->path, "out of memory"); rc = -1; }
    }
    for (unsigned i = 0; i < nimages && !rc; i++) {
        struct cache_image *img = &images[i];
        unsigned file;
        uint64_t header;
        uint8_t mh[32];
        uint32_t ncmds, sizeofcmds;
        uint8_t *cmds;
        int kind;
        if (cache_translate(g, img->address, 32, &file, &header)) { fail(g->path, "image %s header is outside one executable mapping", img->path); rc = -1; break; }
        struct file_handle *f = &g->files[file];
        if (read_exact(f->fd, header, mh, 32, f->bytes)) { fail(f->path, "read image %s header", img->path); rc = -1; break; }
        kind = parse_macho_header(mh, &ncmds, &sizeofcmds);
        if (kind <= 0) { fail(f->path, "image %s is not an ARM64e Mach-O", img->path); rc = -1; break; }
        if (cache_translate(g, img->address + 32, sizeofcmds, &file, &header)) { fail(f->path, "image %s load commands leave the mapping", img->path); rc = -1; break; }
        cmds = malloc(sizeofcmds ? sizeofcmds : 1);
        if (!cmds) { fail(f->path, "out of memory"); rc = -1; break; }
        if (read_exact(f->fd, header, cmds, sizeofcmds, f->bytes)) { fail(f->path, "read image %s load commands", img->path); free(cmds); rc = -1; break; }
        struct macho_ctx m = {img->path, img->path, refs + count, 0, limit - count, 1, &ranges, &nranges};
        rc = walk_load_commands(mh, cmds, ncmds, sizeofcmds, &m, cache_resolve, g);
        free(cmds);
        count += m.count;
        totals.images++;
    }
    if (!rc) rc = cache_stub_regions(g, ranges, nranges, refs, limit, &count, &regions, &skipped_regions);
    if (!rc) rc = rewrite_sections(g->files, g->nfiles, refs, count, &c, &changed, g->path, opt.pacga);
    counts_add(&totals.c, &c);
    totals.changed_sections += changed;
    totals.cache_files += g->nfiles;
    totals.stub_regions += regions; totals.skipped_regions += skipped_regions;
    report_entry(g->path, "dyld_cache", rc ? "error" : (changed ? (opt.write ? "written" : "pending") : "clean"), count, changed, &c);
    if (!rc)
        note("%s: %u files, %u images, %u code sections (%u shared stub regions, %u non-code gaps skipped), %" PRIu64 " words, %" PRIu64 " %s, %" PRIu64 " excluded, %" PRIu64 " pacga, %" PRIu64 " ldra\n",
             g->path, g->nfiles, nimages, count, regions, skipped_regions, c.words, c.converted, opt.write ? "converted" : "convertible", c.excluded, c.pacga, c.ldra);
    free(ranges);
    free(images);
    free(refs);
    return rc;
}

static int process_cache(struct file_handle *main)
{
    uint8_t h[0x200];
    struct cache_group g;
    int rc;
    memset(&g, 0, sizeof(g));
    if (read_exact(main->fd, 0, h, sizeof(h), main->bytes)) { fail(main->path, "short cache header"); return -1; }
    if (!cache_is_main(h)) { totals.subcaches_deferred++; return 0; }
    totals.caches++;
    g.path = main->path;
    g.files[0] = *main;
    g.nfiles = 1;
    rc = cache_add_mappings(&g, 0, h);
    if (!rc) rc = process_cache_group(&g, h);
    for (unsigned i = 1; i < g.nfiles; i++) {
        if (g.files[i].fd >= 0) close(g.files[i].fd);
        free((char *)g.files[i].path);
    }
    free(g.maps);
    return rc;
}

/* ---- visited inode set --------------------------------------------------- */

struct visited { dev_t dev; ino_t ino; int used; };
static struct visited *visited;
static size_t visited_cap, visited_n;

static int visit_once(dev_t dev, ino_t ino)
{
    if (visited_n * 2 >= visited_cap) {
        size_t cap = visited_cap ? visited_cap * 2 : 4096;
        struct visited *grown = calloc(cap, sizeof(*grown));
        if (!grown) return 1;
        for (size_t i = 0; i < visited_cap; i++) if (visited[i].used) {
            size_t k = ((size_t)visited[i].ino * 0x9e3779b97f4a7c15ull ^ (size_t)visited[i].dev) % cap;
            while (grown[k].used) k = (k + 1) % cap;
            grown[k] = visited[i];
        }
        free(visited); visited = grown; visited_cap = cap;
    }
    size_t k = ((size_t)ino * 0x9e3779b97f4a7c15ull ^ (size_t)dev) % visited_cap;
    while (visited[k].used) {
        if (visited[k].dev == dev && visited[k].ino == ino) return 0;
        k = (k + 1) % visited_cap;
    }
    visited[k].dev = dev; visited[k].ino = ino; visited[k].used = 1; visited_n++;
    return 1;
}

/* ---- file dispatch ------------------------------------------------------- */

static int process_file(const char *path, const char *relative)
{
    struct file_handle f;
    uint8_t magic[16];
    struct stat st;
    struct timespec times[2];
    int rc = 0, macho, cache;
    /* Anything shorter than a Mach-O header is never a target; do not even open it
     * (iOS roots carry an unreadable, empty mode-0 /.file). */
    if (lstat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 32) return 0;
    /* Sniff every file read-only; only real targets are reopened for writing. */
    if (open_handle(&f, path)) return -1;
    if (fstat(f.fd, &st)) { close(f.fd); return -1; }
    if (!visit_once(st.st_dev, st.st_ino)) { close(f.fd); return 0; }
    totals.files_seen++;
    if (f.bytes < 32 || read_exact(f.fd, 0, magic, sizeof(magic), f.bytes)) { close(f.fd); return 0; }
#ifdef __APPLE__
    times[0] = st.st_atimespec; times[1] = st.st_mtimespec;
#else
    times[0] = st.st_atim; times[1] = st.st_mtim;
#endif
    macho = b2l_nc_u32(magic) == MH_MAGIC_64 || b2l_nc_u32(magic) == 0xbebafecau || b2l_nc_u32(magic) == 0xbfbafecau;
    cache = !memcmp(magic, "dyld_v1", 7);
    if (macho)
        rc = process_macho(&f, relative);
    else if (cache)
        rc = process_cache(&f);
    else if (b2l_nc_u32(magic) == 0xfeedfaceu || b2l_nc_u32(magic) == 0xcefaedfeu || b2l_nc_u32(magic) == 0xcffaedfeu)
        totals.bad_magic++; /* 32-bit or big-endian Mach-O: never ARM64e */
    if (opt.write && !rc && f.writable) futimens(f.fd, times);
    close(f.fd);
    return rc;
}

static int process_tree(const char *root)
{
    char *const paths[] = {(char *)root, NULL};
    FTS *fts = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR | (opt.xdev ? FTS_XDEV : 0), NULL);
    FTSENT *e;
    int rc = 0;
    size_t rootlen = strlen(root);
    if (!fts) { fail(root, "fts_open: %s", strerror(errno)); return -1; }
    while ((e = fts_read(fts))) {
        if (e->fts_info == FTS_DNR || e->fts_info == FTS_ERR || e->fts_info == FTS_NS) {
            fail(e->fts_path, "%s", strerror(e->fts_errno)); rc = -1; continue;
        }
        if (e->fts_info != FTS_F) continue;
        const char *relative = e->fts_path + rootlen;
        while (*relative == '/') relative++;
        if (process_file(e->fts_path, relative)) rc = -1;
    }
    if (errno) { fail(root, "fts_read: %s", strerror(errno)); rc = -1; }
    fts_close(fts);
    return rc;
}

/* ---- main ---------------------------------------------------------------- */

static void usage(FILE *f)
{
    fputs("usage: b2l-precompile [--write] [--verify] [--report FILE] [--exclude-text PATH]... [--quiet] [--no-xdev]\n"
          "                      [--pacga-trampolines] PATH...\n"
          "  --pacga-trampolines routes shared-cache PACGA to keyless EL0 routines; the root then\n"
          "  requires a kernel booted with b2l_pacga_keyless=1.\n"
          "  Rewrite ARM64e Mach-O and dyld shared-cache code sections with the B2L\n"
          "  allowlisted converter. Without --write, only report. --verify exits 2 when\n"
          "  convertible words remain. PATH is a prepared root, directory or file.\n"
          "  Default exclusion: /usr/lib/system/libcorecrypto.dylib __TEXT,__text.\n", f);
}

int main(int argc, char **argv)
{
    static const char *default_exclude[] = {"/usr/lib/system/libcorecrypto.dylib"};
    const char **paths = calloc((size_t)argc + 1, sizeof(*paths));
    const char **excludes = calloc((size_t)argc + 1, sizeof(*excludes));
    unsigned npaths = 0;
    int rc = 0;
    if (!paths || !excludes) return 1;
    opt.xdev = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--write")) opt.write = 1;
        else if (!strcmp(argv[i], "--verify")) opt.verify = 1;
        else if (!strcmp(argv[i], "--quiet")) opt.quiet = 1;
        else if (!strcmp(argv[i], "--pacga-trampolines")) opt.pacga = 1;
        else if (!strcmp(argv[i], "--no-xdev")) opt.xdev = 0;
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) opt.report = argv[++i];
        else if (!strcmp(argv[i], "--exclude-text") && i + 1 < argc) excludes[opt.excludes++] = argv[++i];
        else if (!strcmp(argv[i], "--no-default-exclusions")) opt.excludes = (unsigned)-1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(stdout); return 0; }
        else if (argv[i][0] == '-' && argv[i][1]) { usage(stderr); return 1; }
        else paths[npaths++] = argv[i];
    }
    if (opt.excludes == (unsigned)-1) opt.excludes = 0;
    else if (!opt.excludes) { excludes[0] = default_exclude[0]; opt.excludes = 1; }
    opt.exclude = excludes;
    if (!npaths) { usage(stderr); return 1; }
    if (opt.report) {
        report.stream = fopen(opt.report, "w");
        if (!report.stream) { fprintf(stderr, "error: %s: %s\n", opt.report, strerror(errno)); return 1; }
        report.first = 1;
        fprintf(report.stream, "{\"mode\":\"%s\",\"files\":[", opt.write ? "write" : "report");
    }
    for (unsigned i = 0; i < npaths; i++) {
        struct stat st;
        if (lstat(paths[i], &st)) { fail(paths[i], "%s", strerror(errno)); rc = -1; continue; }
        if (S_ISDIR(st.st_mode)) { if (process_tree(paths[i])) rc = -1; }
        else if (S_ISREG(st.st_mode)) { if (process_file(paths[i], NULL)) rc = -1; }
        else fail(paths[i], "not a regular file or directory");
    }
    if (report.stream) {
        fputs("\n],\"summary\":{", report.stream);
        fprintf(report.stream, "\"files_seen\":%" PRIu64 ",\"machos\":%" PRIu64 ",\"fat\":%" PRIu64 ",\"arm64e_slices\":%" PRIu64
                               ",\"other_slices\":%" PRIu64 ",\"caches\":%" PRIu64 ",\"cache_files\":%" PRIu64 ",\"images\":%" PRIu64
                               ",\"pacga_routed\":%" PRIu64 ",\"pacga_nop\":%" PRIu64 ",\"pacga_kept\":%" PRIu64 ",\"caves\":%" PRIu64
                               ",\"stub_regions\":%" PRIu64 ",\"skipped_gaps\":%" PRIu64
                               ",\"sections\":%" PRIu64 ",\"changed_sections\":%" PRIu64 ",\"written_bytes\":%" PRIu64 ",\"errors\":%" PRIu64 ",\"counts\":",
                totals.files_seen, totals.machos, totals.fat, totals.slices_arm64e, totals.slices_other, totals.caches, totals.cache_files,
                totals.images, totals.pacga_routed, totals.pacga_nop, totals.pacga_kept, totals.caves,
                totals.stub_regions, totals.skipped_regions, totals.sections, totals.changed_sections, totals.written_bytes, totals.errors);
        json_counts(report.stream, &totals.c);
        fputs("}}\n", report.stream);
        if (fclose(report.stream)) { fprintf(stderr, "error: %s: %s\n", opt.report, strerror(errno)); rc = -1; }
    }
    printf("B2L precompile %s: files=%" PRIu64 " machos=%" PRIu64 " arm64e_slices=%" PRIu64 " other_slices=%" PRIu64
           " caches=%" PRIu64 " cache_files=%" PRIu64 " images=%" PRIu64 " stub_regions=%" PRIu64 " skipped_gaps=%" PRIu64
           " sections=%" PRIu64 " changed_sections=%" PRIu64 "\n",
           opt.write ? "write" : "report", totals.files_seen, totals.machos, totals.slices_arm64e, totals.slices_other,
           totals.caches, totals.cache_files, totals.images, totals.stub_regions, totals.skipped_regions, totals.sections, totals.changed_sections);
    printf("  words=%" PRIu64 " %s=%" PRIu64 " (pac=%" PRIu64 " hint=%" PRIu64 " ret=%" PRIu64 " branch=%" PRIu64 " ldapr=%" PRIu64
           ") excluded=%" PRIu64 " remaining pacga=%" PRIu64 " ldra=%" PRIu64 " written_bytes=%" PRIu64 " errors=%" PRIu64 "\n",
           totals.c.words, opt.write ? "converted" : "convertible", totals.c.converted, totals.c.by_class[CLASS_PAC], totals.c.by_class[CLASS_HINT],
           totals.c.by_class[CLASS_RET], totals.c.by_class[CLASS_BRANCH], totals.c.by_class[CLASS_LDAPR], totals.c.excluded,
           totals.c.pacga, totals.c.ldra, totals.written_bytes, totals.errors);
    if (opt.pacga)
        printf("  pacga %s=%" PRIu64 " nop=%" PRIu64 " kept_trapping=%" PRIu64 " caves=%" PRIu64 "\n",
               opt.write ? "routed" : "routable", totals.pacga_routed, totals.pacga_nop, totals.pacga_kept, totals.caves);
    if (totals.subcaches_deferred && !totals.caches)
        fprintf(stderr, "warning: %" PRIu64 " subcache file(s) seen without their main cache; pass the main cache or its directory\n",
                totals.subcaches_deferred);
    free(visited);
    free(paths);
    free(excludes);
    if (rc || totals.errors) return 1;
    if (opt.verify && totals.c.converted && !opt.write) { fprintf(stderr, "verify: %" PRIu64 " convertible words remain\n", totals.c.converted); return 2; }
    return 0;
}
