/* SPDX-License-Identifier: BSD-2-Clause */
/* Included by kern_exec.c after EXECUTABLE_KEY/exec_add_user_string.
 * Load the GPU translation layer into backboardd and nothing else. The library
 * is published from BOOT, so the previous BOOT image removes it. Boot argument
 * b2l_gpu_ui=0 turns it off. */
#ifndef B2L_EXEC_GPU_UI_H
#define B2L_EXEC_GPU_UI_H
#define B2L_GPU_UI_EXECUTABLE "/usr/libexec/backboardd"
extern int b2l_gpu_ui_is_ready(void);
extern const char b2l_gpu_ui_setting[];
static int
b2l_gpu_ui_enabled(void)
{
	int enabled = 1;
	PE_parse_boot_argn("b2l_gpu_ui", &enabled, sizeof(enabled));
	return enabled == 1 && b2l_gpu_ui_is_ready();
}
static int
b2l_exec_gpu_ui(struct image_params *imgp, int pointer_size)
{
	static const char key[] = "DYLD_INSERT_LIBRARIES=";
	if (pointer_size != 8 || !b2l_gpu_ui_enabled()) return 0;
	const char *path = imgp->ip_strings + strlen(EXECUTABLE_KEY);
	if (strcmp(path, B2L_GPU_UI_EXECUTABLE)) return 0;
	const char *cursor = imgp->ip_endargv;
	while (cursor < imgp->ip_strendp) {
		size_t available = (size_t)(imgp->ip_strendp - cursor);
		size_t length = strnlen(cursor, available);
		if (length == available) return 0;
		if (length >= sizeof(key) - 1 && memcmp(cursor, key, sizeof(key) - 1) == 0) return 0;
		cursor += length + 1;
	}
	size_t bytes = strlen(b2l_gpu_ui_setting) + 1;
	if (imgp->ip_argspace < (int)(bytes + 3 * pointer_size) || imgp->ip_strspace < (int)bytes) return 0;
	int error = exec_add_user_string(imgp, CAST_USER_ADDR_T(b2l_gpu_ui_setting), UIO_SYSSPACE, TRUE);
	if (error) return error;
	imgp->ip_argspace -= pointer_size;
	imgp->ip_envc++;
	b2l_early_trace("B2L GPU UI: library selected for backboardd\n");
	return 0;
}
#endif
