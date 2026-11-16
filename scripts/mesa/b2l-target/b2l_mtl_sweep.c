// SPDX-License-Identifier: MIT
/*
 * S10 port: Metal milestone M0 (docs/metal-on-panfrost.md).
 *
 * Every function of the phone's own QuartzCore default.metallib goes through
 * HakiOS's AIR frontend (src/compiler/air) to NIR and through Panfrost's
 * Bifrost compiler for this GPU (Mali-G76, gpu_id 0x7211), on the CPU only:
 * nothing is submitted, the GPU and its runtime are not touched, and the
 * library is read where it lies (nothing of Apple's leaves the phone).
 *
 * The window server's uber shader (fixed_frag_lph_cph / _cpf) is compiled
 * again once per texture function the OpenGL ES census saw (52 = vcm, 59 =
 * dff, ...), that function constant (index 28) supplied and every other one
 * left unsupplied, as a pipeline that names only it would.
 *
 * One kernel log line per job ("B2L GPU UI: mtl M0: ..."), through a B2LMali
 * user client of its own that only ever logs. Before each job its number is
 * written to progress_path, so that the parent (userspace/b2l-gpu.c), which
 * runs this in a child process, can start again after the job that killed it.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <CoreFoundation/CoreFoundation.h>

#include "compiler/nir/nir.h"
#include "util/ralloc.h"
#include "util/u_dynarray.h"
#include "panfrost/util/pan_ir.h"
#include "nir_air.h"
#include "b2l_mali_uapi.h"

#define B2L_EXPORT __attribute__((visibility("default")))

B2L_EXPORT int b2l_mtl_sweep(unsigned start, const char *progress_path);

/* Panfrost's own entry points for Bifrost v7 (GENX names), and the Gallium
 * driver's system-value lowering; declared here so that the driver's headers,
 * which want PAN_ARCH, stay out. */
struct panfrost_sysvals;
void bifrost_preprocess_nir(nir_shader *nir, unsigned gpu_id);
const nir_shader_compiler_options *pan_shader_get_compiler_options_v7(void);
void pan_shader_compile_v7(nir_shader *nir, struct panfrost_compile_inputs *inputs,
                           struct util_dynarray *binary, struct pan_shader_info *info);
bool panfrost_nir_lower_sysvals(nir_shader *s, unsigned arch, struct panfrost_sysvals *sysvals);

#define MTL_GPU_ID      0x7211u      /* GPU_ID 0x72110000 in the boot log: Mali-G76 */
#define MTL_LIBRARY     "/System/Library/Frameworks/QuartzCore.framework/default.metallib"
#define MTL_AIR64_CPU   0x01000017u  /* the AIR64 slice of a fat metallib */
#define MTL_FC_TEXTURE  28u          /* fc_texture_function: the CA function code */

/* The CA function codes the OpenGL ES census drew without a program, and a
 * few of the plain ones next to them (clr, img, colour matrices). */
static const uint8_t texture_functions[] = {
   0, 1, 13, 18, 21, 22, 29, 39, 51, 52, 53, 54, 59, 60, 61, 62, 63, 64, 65, 66, 67, 74, 77, 85,
};

typedef mach_port_t io_object_t, io_service_t, io_connect_t;
extern const mach_port_t kIOMainPortDefault;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t port, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t task, uint32_t type, io_connect_t *connect);
extern kern_return_t IOServiceClose(io_connect_t connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallMethod(io_connect_t c, uint32_t selector, const uint64_t *in, uint32_t in_count,
    const void *struct_in, size_t struct_in_size, uint64_t *out, uint32_t *out_count, void *struct_out,
    size_t *struct_out_size);

static io_connect_t log_connection = MACH_PORT_NULL;

static void
note(const char *format, ...)
{
   char text[B2L_MALI_MAX_LOG - 1];
   va_list ap;
   va_start(ap, format);
   int n = vsnprintf(text, sizeof(text), format, ap);
   va_end(ap);
   if (n <= 0)
      return;
   size_t len = (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1;
   if (log_connection != MACH_PORT_NULL)
      IOConnectCallMethod(log_connection, kB2LMaliLog, NULL, 0, text, len, NULL, NULL, NULL, NULL);
}

static void
log_open(void)
{
   io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("B2LMali"));
   if (service == MACH_PORT_NULL)
      return;
   /* Only kB2LMaliLog is ever sent: the kernel's runtime (clientBegin) is
    * reached by no other selector, so backboardd's GPU client is untouched. */
   if (IOServiceOpen(service, mach_task_self(), B2L_MALI_CLIENT_BO, &log_connection) != KERN_SUCCESS)
      log_connection = MACH_PORT_NULL;
   IOObjectRelease(service);
}

static uint64_t
now_us(void)
{
   static mach_timebase_info_data_t tb;
   if (tb.denom == 0)
      mach_timebase_info(&tb);
   return mach_absolute_time() * tb.numer / tb.denom / 1000;
}

static void
progress(const char *path, unsigned job)
{
   if (path == NULL)
      return;
   int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
   if (fd < 0)
      return;
   char text[16];
   int n = snprintf(text, sizeof(text), "%u\n", job);
   if (n > 0)
      (void)!write(fd, text, (size_t)n);
   close(fd);
}

/* The AIR64 slice of a fat (0xCAFEBABE, big-endian) metallib, or the whole
 * file when it is thin. */
static const uint8_t *
thin_slice(const uint8_t *data, size_t size, size_t *out_size)
{
   if (size < 8 || data[0] != 0xca || data[1] != 0xfe || data[2] != 0xba || data[3] != 0xbe) {
      *out_size = size;
      return data;
   }
   uint32_t n = (uint32_t)data[4] << 24 | (uint32_t)data[5] << 16 | (uint32_t)data[6] << 8 | data[7];
   for (uint32_t i = 0; i < n && 8 + 20 * (size_t)i + 20 <= size; i++) {
      const uint8_t *e = data + 8 + 20 * i;
      uint32_t cpu = (uint32_t)e[0] << 24 | (uint32_t)e[1] << 16 | (uint32_t)e[2] << 8 | e[3];
      uint32_t off = (uint32_t)e[8] << 24 | (uint32_t)e[9] << 16 | (uint32_t)e[10] << 8 | e[11];
      uint32_t len = (uint32_t)e[12] << 24 | (uint32_t)e[13] << 16 | (uint32_t)e[14] << 8 | e[15];
      if (cpu == MTL_AIR64_CPU && (size_t)off + len <= size) {
         *out_size = len;
         return data + off;
      }
   }
   return NULL;
}

static bool
is_uber(const char *name)
{
   return strncmp(name, "fixed_frag_", 11) == 0;
}

struct tally {
   unsigned ok, refused, failed;
   unsigned by_category[6]; /* reader stage interface instruction intrinsic other */
};

static unsigned
category(const char *error)
{
   static const char *names[] = { "reader", "stage", "interface", "instruction", "intrinsic" };
   for (unsigned i = 0; i < 5; i++)
      if (error != NULL && strncmp(error, names[i], strlen(names[i])) == 0)
         return i;
   return 5;
}

/* One job: one function, with the texture function constant supplied
 * (texture_function >= 0) or nothing supplied. */
static void
run_job(unsigned job, const struct air_metallib_function *f, int texture_function, struct tally *t)
{
   void *mem = ralloc_context(NULL);
   struct air_constant_value *constants = NULL;
   unsigned num_constants = air_module_function_constants(mem, f->bitcode, f->bitcode_size, &constants);
   bool supplied = false;
   if (texture_function >= 0) {
      for (unsigned i = 0; i < num_constants; i++) {
         if (constants[i].index == MTL_FC_TEXTURE) {
            constants[i].bits = (uint64_t)texture_function;
            constants[i].supplied = true;
            supplied = true;
         }
      }
   }

   struct air_to_nir_options options = { .constants = constants, .num_constants = num_constants };
   struct air_shader_info info;
   memset(&info, 0, sizeof(info));
   const char *error = NULL;
   char label[96];
   if (texture_function >= 0)
      snprintf(label, sizeof(label), "%s tf=%d%s", f->name, texture_function, supplied ? "" : "(no fc28)");
   else
      snprintf(label, sizeof(label), "%s", f->name);

   uint64_t t0 = now_us();
   nir_shader *nir = air_to_nir(mem, f->bitcode, f->bitcode_size, NULL, &options,
                                pan_shader_get_compiler_options_v7(), &info, &error);
   uint64_t t1 = now_us();
   if (nir == NULL) {
      t->refused++;
      t->by_category[category(error)]++;
      note("mtl M0: [%u] %s: AIR refused (%s) %llu ms", job, label, error ? error : "?",
           (unsigned long long)((t1 - t0) / 1000));
      ralloc_free(mem);
      return;
   }

   gl_shader_stage stage = nir->info.stage;
   if (stage == MESA_SHADER_VERTEX || stage == MESA_SHADER_FRAGMENT || stage == MESA_SHADER_COMPUTE) {
      nir_assign_io_var_locations(nir, nir_var_shader_in, &nir->num_inputs, stage);
      nir_assign_io_var_locations(nir, nir_var_shader_out, &nir->num_outputs, stage);
   } else {
      t->failed++;
      note("mtl M0: [%u] %s: stage %s not compiled", job, label, gl_shader_stage_name(stage));
      ralloc_free(mem);
      return;
   }

   bifrost_preprocess_nir(nir, MTL_GPU_ID);
   /* struct panfrost_sysvals is an array and a count; this is far larger. */
   static uint64_t sysvals[1024];
   memset(sysvals, 0, sizeof(sysvals));
   panfrost_nir_lower_sysvals(nir, 7, (struct panfrost_sysvals *)sysvals);

   struct panfrost_compile_inputs inputs;
   memset(&inputs, 0, sizeof(inputs));
   inputs.gpu_id = MTL_GPU_ID;
   struct util_dynarray binary;
   util_dynarray_init(&binary, NULL);
   struct pan_shader_info pinfo;
   memset(&pinfo, 0, sizeof(pinfo));
   uint64_t t2 = now_us();
   pan_shader_compile_v7(nir, &inputs, &binary, &pinfo);
   uint64_t t3 = now_us();

   if (binary.size == 0) {
      t->failed++;
      note("mtl M0: [%u] %s %s: Bifrost gave no code (AIR %llu ms)", job, label, gl_shader_stage_name(stage),
           (unsigned long long)((t1 - t0) / 1000));
   } else {
      t->ok++;
      note("mtl M0: [%u] %s %s: ok %u bytes, %u regs, tls %u, wls %u, res %u, fbfetch 0x%x; AIR %llu ms, "
           "Bifrost %llu ms",
           job, label, gl_shader_stage_name(stage), binary.size, pinfo.work_reg_count, pinfo.tls_size,
           pinfo.wls_size, info.num_resources, info.fbfetch_targets, (unsigned long long)((t1 - t0) / 1000),
           (unsigned long long)((t3 - t2) / 1000));
   }
   util_dynarray_fini(&binary);
   ralloc_free(mem);
}

int
b2l_mtl_sweep(unsigned start, const char *progress_path)
{
   log_open();
   int fd = open(MTL_LIBRARY, O_RDONLY);
   struct stat st;
   if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0) {
      note("mtl M0: %s: cannot open (errno %d)", MTL_LIBRARY, errno);
      if (fd >= 0)
         close(fd);
      return 2;
   }
   size_t size = (size_t)st.st_size;
   const uint8_t *data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
   close(fd);
   if (data == MAP_FAILED) {
      note("mtl M0: mmap of %zu bytes failed (errno %d)", size, errno);
      return 2;
   }
   size_t thin_size = 0;
   const uint8_t *thin = thin_slice(data, size, &thin_size);
   void *mem = ralloc_context(NULL);
   struct air_metallib lib;
   memset(&lib, 0, sizeof(lib));
   const char *error = NULL;
   if (thin == NULL || !air_metallib_parse(mem, thin, thin_size, &lib, &error)) {
      note("mtl M0: QuartzCore library not read: %s", thin == NULL ? "no AIR64 slice" : (error ? error : "?"));
      ralloc_free(mem);
      munmap((void *)data, size);
      return 3;
   }

   /* The jobs: every function once, then the uber shaders once per texture
    * function. Their numbering is fixed by the library, so a restart after a
    * crash names the same job. */
   unsigned uber = 0;
   for (unsigned i = 0; i < lib.num_functions; i++)
      uber += is_uber(lib.functions[i].name);
   unsigned per_uber = sizeof(texture_functions) / sizeof(texture_functions[0]);
   unsigned total = lib.num_functions + uber * per_uber;
   if (start == 0)
      note("mtl M0: QuartzCore library %zu bytes (AIR64 slice %zu), version %u.%u, %u functions, %u uber; "
           "%u jobs for gpu 0x%x",
           size, thin_size, lib.version_major, lib.version_minor, lib.num_functions, uber, total, MTL_GPU_ID);
   else
      note("mtl M0: resuming at job %u of %u", start, total);

   struct tally t;
   memset(&t, 0, sizeof(t));
   uint64_t began = now_us();
   unsigned job = 0;
   for (unsigned i = 0; i < lib.num_functions; i++, job++) {
      if (job < start)
         continue;
      progress(progress_path, job);
      run_job(job, &lib.functions[i], -1, &t);
   }
   for (unsigned i = 0; i < lib.num_functions; i++) {
      if (!is_uber(lib.functions[i].name))
         continue;
      for (unsigned k = 0; k < per_uber; k++, job++) {
         if (job < start)
            continue;
         progress(progress_path, job);
         run_job(job, &lib.functions[i], texture_functions[k], &t);
      }
   }
   /* UINT32_MAX: finished (b2l-gpu.c MTL_SWEEP_FINISHED), whatever the
    * parent learns of the exit status. */
   progress(progress_path, UINT32_MAX);
   note("mtl M0: done jobs %u..%u in %llu s: compiled %u, AIR refused %u (reader %u stage %u interface %u "
        "instruction %u intrinsic %u other %u), not compiled %u",
        start, total, (unsigned long long)((now_us() - began) / 1000000), t.ok, t.refused, t.by_category[0],
        t.by_category[1], t.by_category[2], t.by_category[3], t.by_category[4], t.by_category[5], t.failed);

   ralloc_free(mem);
   munmap((void *)data, size);
   if (log_connection != MACH_PORT_NULL)
      IOServiceClose(log_connection);
   return 0;
}
