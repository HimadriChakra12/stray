/* freedesktop icon-theme lookup, trimmed to what a tray needs: given
 * a name like "discord", find the closest-size file in the icon
 * theme dirs and decode it with Imlib2. skips index.theme inheritance
 * parsing (Papirus falling back to Adwaita, etc) - good enough since
 * tray icons are almost always in hicolor. */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <Imlib2.h>
#include "tray.h"

#define MAX_DIRS 32

static void collect_base_dirs(char *dirs[], int *n, const char *extra) {
	*n = 0;
	if (extra && *extra)
		dirs[(*n)++] = strdup(extra);

	const char *home = getenv("HOME");
	if (home) {
		char buf[1024];
		snprintf(buf, sizeof(buf), "%s/.local/share/icons", home);
		dirs[(*n)++] = strdup(buf);
		snprintf(buf, sizeof(buf), "%s/.icons", home);
		dirs[(*n)++] = strdup(buf);
	}

	const char *xdg = getenv("XDG_DATA_DIRS");
	if (!xdg || !*xdg) xdg = "/usr/local/share:/usr/share";
	char *copy = strdup(xdg), *save = NULL;
	for (char *tok = strtok_r(copy, ":", &save); tok && *n < MAX_DIRS - 4; tok = strtok_r(NULL, ":", &save)) {
		char buf[1024];
		snprintf(buf, sizeof(buf), "%s/icons", tok);
		dirs[(*n)++] = strdup(buf);
	}
	free(copy);
	dirs[(*n)++] = strdup("/usr/share/pixmaps");
}

static int parse_size_dir(const char *name) {
	int w = 0, h = 0;
	return (sscanf(name, "%dx%d", &w, &h) == 2 && w == h) ? w : 0;
}

/* walk <theme_dir>/<size or scalable>/<category>/<icon>.{png,xpm},
 * tracking whichever match is closest to pref_size */
static char *search_theme_dir(const char *theme_dir, const char *icon, int pref_size, int *best_size) {
	DIR *d = opendir(theme_dir);
	if (!d) return NULL;

	char *best = NULL;
	struct dirent *ent;
	while ((ent = readdir(d))) {
		if (ent->d_name[0] == '.') continue;

		char sub[1024];
		snprintf(sub, sizeof(sub), "%s/%s", theme_dir, ent->d_name);
		struct stat st;
		if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

		int size = parse_size_dir(ent->d_name);
		int scalable = !strcmp(ent->d_name, "scalable");
		if (!size && !scalable) continue;

		DIR *d2 = opendir(sub);
		if (!d2) continue;
		struct dirent *cat;
		while ((cat = readdir(d2))) {
			if (cat->d_name[0] == '.') continue;
			char catdir[1024];
			snprintf(catdir, sizeof(catdir), "%s/%s", sub, cat->d_name);

			for (int ext = 0; ext < 2; ext++) {
				char path[1280];
				snprintf(path, sizeof(path), "%s/%s.%s", catdir, icon, ext ? "xpm" : "png");
				if (access(path, F_OK) != 0) continue;

				int this_size = scalable ? pref_size : size;
				int dist = abs(this_size - pref_size);
				int best_dist = best ? abs(*best_size - pref_size) : 1 << 30;
				if (!best || dist < best_dist) {
					free(best);
					best = strdup(path);
					*best_size = this_size;
				}
			}
		}
		closedir(d2);
	}
	closedir(d);
	return best;
}

static char *find_icon_file(const char *icon, int pref_size, const char *extra) {
	if (!icon || !*icon) return NULL;

	char *bases[MAX_DIRS];
	int n = 0;
	collect_base_dirs(bases, &n, extra);

	char *result = NULL;
	int best_size = 0;

	for (int i = 0; i < n && !result; i++) {
		for (int ext = 0; ext < 2 && !result; ext++) {
			char path[1280];
			snprintf(path, sizeof(path), "%s/%s.%s", bases[i], icon, ext ? "xpm" : "png");
			if (access(path, F_OK) == 0)
				result = strdup(path);
		}
		if (result) break;

		char hicolor[1024];
		snprintf(hicolor, sizeof(hicolor), "%s/hicolor", bases[i]);
		result = search_theme_dir(hicolor, icon, pref_size, &best_size);

		if (!result) {
			DIR *d = opendir(bases[i]);
			if (d) {
				struct dirent *ent;
				while (!result && (ent = readdir(d))) {
					if (ent->d_name[0] == '.' || !strcmp(ent->d_name, "hicolor")) continue;
					char themedir[1024];
					snprintf(themedir, sizeof(themedir), "%s/%s", bases[i], ent->d_name);
					result = search_theme_dir(themedir, icon, pref_size, &best_size);
				}
				closedir(d);
			}
		}
	}

	for (int i = 0; i < n; i++) free(bases[i]);
	return result;
}

unsigned char *icon_theme_load(const char *icon, int pref_size, const char *extra, int *out_w, int *out_h) {
	char *path = find_icon_file(icon, pref_size, extra);
	if (!path) {
		fprintf(stderr, "[tray] no themed icon found for '%s'\n", icon);
		return NULL;
	}

	Imlib_Image img = imlib_load_image(path);
	if (!img) {
		fprintf(stderr, "[tray] failed to decode icon file: %s\n", path);
		free(path);
		return NULL;
	}

	imlib_context_set_image(img);
	int w = imlib_image_get_width(), h = imlib_image_get_height();
	DATA32 *data = imlib_image_get_data_for_reading_only();

	unsigned char *buf = malloc((size_t)w * h * 4);
	for (int i = 0; i < w * h; i++) {
		uint32_t v = data[i];
		buf[i * 4 + 0] = (v >> 24) & 0xff;
		buf[i * 4 + 1] = (v >> 16) & 0xff;
		buf[i * 4 + 2] = (v >> 8) & 0xff;
		buf[i * 4 + 3] = v & 0xff;
	}

	imlib_free_image();
	free(path);
	*out_w = w;
	*out_h = h;
	fprintf(stderr, "[tray] loaded themed icon '%s' at %dx%d\n", icon, w, h);
	return buf;
}
