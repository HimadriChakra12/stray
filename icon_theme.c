/* icon_theme.c
 *
 * Freedesktop Icon Theme spec lookup, trimmed down to what a tray
 * actually needs: given an icon name (e.g. "discord", "jdownloader"),
 * find the closest-size PNG/XPM in the user's icon theme (or
 * hicolor/pixmaps fallback) and decode it to the same ARGB byte
 * layout used for DBus-provided IconPixmap data, so tray_window.c's
 * blit code doesn't need to care where the icon came from.
 *
 * This is the same lookup mechanism GTK/Qt tray icons and things
 * like polybar rely on - not a "grep i3blocks" ad hoc, but it plays
 * the equivalent role: given a short name, resolve the artwork.
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <Imlib2.h>
#include "tray.h"

#define MAX_SEARCH_DIRS 32

static void collect_base_dirs(char *dirs[], int *n, const char *extra_theme_path) {
	*n = 0;

	if (extra_theme_path && *extra_theme_path)
		dirs[(*n)++] = strdup(extra_theme_path);

	const char *home = getenv("HOME");
	if (home) {
		char buf[1024];
		snprintf(buf, sizeof(buf), "%s/.local/share/icons", home);
		dirs[(*n)++] = strdup(buf);
		snprintf(buf, sizeof(buf), "%s/.icons", home);
		dirs[(*n)++] = strdup(buf);
	}

	const char *xdg_data_dirs = getenv("XDG_DATA_DIRS");
	if (!xdg_data_dirs || !*xdg_data_dirs)
		xdg_data_dirs = "/usr/local/share:/usr/share";

	char *copy = strdup(xdg_data_dirs);
	char *save = NULL;
	for (char *tok = strtok_r(copy, ":", &save); tok && *n < MAX_SEARCH_DIRS - 4;
	     tok = strtok_r(NULL, ":", &save)) {
		char buf[1024];
		snprintf(buf, sizeof(buf), "%s/icons", tok);
		dirs[(*n)++] = strdup(buf);
	}
	free(copy);

	dirs[(*n)++] = strdup("/usr/share/pixmaps");
}

/* parse a "48x48" style directory name, return the size or 0 */
static int parse_size_dir(const char *name) {
	int w = 0, h = 0;
	if (sscanf(name, "%dx%d", &w, &h) == 2 && w == h)
		return w;
	return 0;
}

/* walk <icons_base>/<theme>/**, tracking the best (closest, >=
 * preferred when possible) sized match for icon_name.{png,xpm} plus
 * any scalable/svg fallback. best-effort, not spec-perfect (skips
 * index.theme parsing / inheritance) but good enough for tray icons
 * that are almost always in hicolor or a theme flattened under it. */
static char *search_theme_dir(const char *theme_dir, const char *icon_name,
                               int pref_size, int *best_size) {
	DIR *d = opendir(theme_dir);
	if (!d) return NULL;

	char *best_path = NULL;
	struct dirent *ent;
	while ((ent = readdir(d))) {
		if (ent->d_name[0] == '.') continue;

		char sub[1024];
		snprintf(sub, sizeof(sub), "%s/%s", theme_dir, ent->d_name);
		struct stat st;
		if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

		int size = parse_size_dir(ent->d_name);
		int is_scalable = strcmp(ent->d_name, "scalable") == 0;
		if (size == 0 && !is_scalable) continue;

		/* look in every category subdir (apps, status, categories...) */
		DIR *d2 = opendir(sub);
		if (!d2) continue;
		struct dirent *cat;
		while ((cat = readdir(d2))) {
			if (cat->d_name[0] == '.') continue;
			char catdir[1024];
			snprintf(catdir, sizeof(catdir), "%s/%s", sub, cat->d_name);

			for (int ext = 0; ext < 2; ext++) {
				char candidate[1200];
				snprintf(candidate, sizeof(candidate), "%s/%s.%s",
				         catdir, icon_name, ext == 0 ? "png" : "xpm");
				if (access(candidate, F_OK) != 0) continue;

				int this_size = is_scalable ? pref_size : size;
				int this_dist = abs(this_size - pref_size);
				int best_dist = best_path ? abs(*best_size - pref_size) : 1 << 30;

				/* prefer exact/closer size, and prefer a real raster
				 * over scalable only when strictly closer */
				if (!best_path || this_dist < best_dist) {
					free(best_path);
					best_path = strdup(candidate);
					*best_size = this_size;
				}
			}
		}
		closedir(d2);
	}
	closedir(d);
	return best_path;
}

static char *find_icon_file(const char *icon_name, int pref_size, const char *extra_theme_path) {
	if (!icon_name || !*icon_name) return NULL;

	char *bases[MAX_SEARCH_DIRS];
	int n_bases = 0;
	collect_base_dirs(bases, &n_bases, extra_theme_path);

	char *result = NULL;
	int best_size = 0;

	for (int i = 0; i < n_bases && !result; i++) {
		/* flat dir (e.g. pixmaps, or an app-provided theme path with
		 * icons directly inside it) */
		for (int ext = 0; ext < 2; ext++) {
			char candidate[1200];
			snprintf(candidate, sizeof(candidate), "%s/%s.%s",
			         bases[i], icon_name, ext == 0 ? "png" : "xpm");
			if (access(candidate, F_OK) == 0) {
				result = strdup(candidate);
				break;
			}
		}
		if (result) break;

		/* theme subdirs: prefer hicolor, then whatever else is there */
		char hicolor[1024];
		snprintf(hicolor, sizeof(hicolor), "%s/hicolor", bases[i]);
		result = search_theme_dir(hicolor, icon_name, pref_size, &best_size);

		if (!result) {
			DIR *d = opendir(bases[i]);
			if (d) {
				struct dirent *ent;
				while (!result && (ent = readdir(d))) {
					if (ent->d_name[0] == '.') continue;
					if (strcmp(ent->d_name, "hicolor") == 0) continue;
					char themedir[1024];
					snprintf(themedir, sizeof(themedir), "%s/%s", bases[i], ent->d_name);
					result = search_theme_dir(themedir, icon_name, pref_size, &best_size);
				}
				closedir(d);
			}
		}
	}

	for (int i = 0; i < n_bases; i++) free(bases[i]);
	return result;
}

unsigned char *icon_theme_load(const char *icon_name, int pref_size,
                                const char *extra_theme_path, int *out_w, int *out_h) {
	char *path = find_icon_file(icon_name, pref_size, extra_theme_path);
	if (!path) {
		fprintf(stderr, "[tray] no themed icon found for '%s'\n", icon_name);
		return NULL;
	}

	Imlib_Image img = imlib_load_image(path);
	if (!img) {
		fprintf(stderr, "[tray] failed to decode icon file: %s\n", path);
		free(path);
		return NULL;
	}

	imlib_context_set_image(img);
	int w = imlib_image_get_width();
	int h = imlib_image_get_height();
	DATA32 *data = imlib_image_get_data_for_reading_only();

	unsigned char *buf = malloc((size_t)w * h * 4);
	for (int i = 0; i < w * h; i++) {
		uint32_t v = data[i];
		unsigned char a = (v >> 24) & 0xff;
		unsigned char r = (v >> 16) & 0xff;
		unsigned char g = (v >> 8) & 0xff;
		unsigned char b = v & 0xff;
		buf[i * 4 + 0] = a;
		buf[i * 4 + 1] = r;
		buf[i * 4 + 2] = g;
		buf[i * 4 + 3] = b;
	}

	imlib_free_image();
	free(path);

	*out_w = w;
	*out_h = h;
	fprintf(stderr, "[tray] loaded themed icon '%s' at %dx%d\n", icon_name, w, h);
	return buf;
}
