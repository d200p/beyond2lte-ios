/* SPDX-License-Identifier: BSD-2-Clause */
/* S10 port: the processes that get the keyboard relief library
 * (userspace/b2l-kbd.c). The system keyboard runs inside each app's own
 * process, so every app gets it: system apps, apps from the App cryptex,
 * installed apps, and SpringBoard. The executable must sit in an .app bundle;
 * daemons and extensions (.appex) are left alone. Shared by the exec hook,
 * which adds the library, and the AMFI dyld policy, which lets it load. */
#ifndef B2L_KBD_PATHS_H
#define B2L_KBD_PATHS_H
static inline int
b2l_kbd_starts(const char *path, const char *prefix)
{
	return strncmp(path, prefix, strlen(prefix)) == 0;
}
static inline int
b2l_kbd_app_path(const char *path)
{
	if (!strcmp(path, "/System/Library/CoreServices/SpringBoard.app/SpringBoard")) return 1;
	if (!b2l_kbd_starts(path, "/Applications/") &&
	    !b2l_kbd_starts(path, "/private/preboot/Cryptexes/App/Applications/") &&
	    !b2l_kbd_starts(path, "/private/var/containers/Bundle/Application/") &&
	    !b2l_kbd_starts(path, "/var/containers/Bundle/Application/")) return 0;
	/* The executable directly inside the bundle: "<name>.app/<executable>". */
	const char *last = NULL;
	for (const char *p = path; *p; p++) {
		if (*p == '/') last = p;
	}
	if (last == NULL || last - path < 6 || last[1] == 0) return 0;
	if (strncmp(last - 4, ".app", 4) != 0 || last[-5] == '/') return 0;
	return 1;
}
#endif
