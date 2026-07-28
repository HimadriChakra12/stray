/* bar_detect.c
 *
 * Almost every bar (polybar, waybar, lemonbar with -x dock hints,
 * sxbar, i3bar, ...) marks its own window as
 * _NET_WM_WINDOW_TYPE_DOCK - that's the standard EWMH way of saying
 * "I'm a panel, reserve screen space for me." We don't need to know
 * anything about the specific bar; we just look for that hint,
 * read the window's real on-screen geometry, and sample a pixel
 * from it for a matching background color.
 *
 * This makes the tray's positioning "dynamic to the system it's
 * used on" - it adapts to whatever bar is actually running instead
 * of requiring hand-tuned gravity/offset config per setup.
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include "tray.h"

/* returns 1 and fills out_* if win is mapped and tagged as a dock */
static int window_is_mapped_dock(Display *dpy, Window win,
                                  Atom type_atom, Atom dock_atom,
                                  XWindowAttributes *out_attrs) {
	Atom actual_type;
	int actual_format;
	unsigned long nitems, bytes_after;
	unsigned char *prop = NULL;

	int ret = XGetWindowProperty(dpy, win, type_atom, 0, 16, False, XA_ATOM,
	                              &actual_type, &actual_format, &nitems,
	                              &bytes_after, &prop);
	if (ret != Success || !prop)
		return 0;

	int is_dock = 0;
	Atom *atoms = (Atom *)prop;
	for (unsigned long i = 0; i < nitems; i++) {
		if (atoms[i] == dock_atom) {
			is_dock = 1;
			break;
		}
	}
	XFree(prop);

	if (!is_dock)
		return 0;

	if (!XGetWindowAttributes(dpy, win, out_attrs))
		return 0;
	if (out_attrs->map_state != IsViewable)
		return 0;

	return 1;
}

/* sample a background pixel near the window's edge, away from
 * where text/icons are usually drawn, so we don't accidentally
 * grab a glyph's foreground color */
static unsigned long sample_bg_color(Display *dpy, Window win, int w, int h) {
	int sx = w > 4 ? w - 3 : w / 2;
	int sy = h / 2;
	XImage *img = XGetImage(dpy, win, sx, sy, 1, 1, AllPlanes, ZPixmap);
	if (!img)
		return 0;
	unsigned long pixel = XGetPixel(img, 0, 0);
	XDestroyImage(img);
	return pixel;
}

int bar_detect(TrayState *st) {
	Display *dpy = st->dpy;
	Window root = RootWindow(dpy, st->screen);

	Atom type_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	Atom dock_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);

	Window root_ret, parent_ret, *children = NULL;
	unsigned int nchildren = 0;
	if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren))
		return 0;

	int found = 0;
	for (unsigned int i = 0; i < nchildren && !found; i++) {
		XWindowAttributes attrs;
		if (!window_is_mapped_dock(dpy, children[i], type_atom, dock_atom, &attrs))
			continue;

		/* attrs.x/y are relative to the window's parent - these dock
		 * windows are almost always direct children of root, but
		 * translate just in case a WM reparented it */
		int abs_x, abs_y;
		Window child_ret;
		XTranslateCoordinates(dpy, children[i], root, 0, 0, &abs_x, &abs_y, &child_ret);

		st->dock_x = abs_x;
		st->dock_y = abs_y;
		st->dock_w = attrs.width;
		st->dock_h = attrs.height;
		st->dock_bg = sample_bg_color(dpy, children[i], attrs.width, attrs.height);
		found = 1;

		fprintf(stderr, "[tray] detected dock/bar window 0x%lx at %d,%d %dx%d\n",
		        children[i], abs_x, abs_y, attrs.width, attrs.height);
	}

	if (children) XFree(children);

	st->dock_found = found;
	if (!found)
		fprintf(stderr, "[tray] no EWMH dock window found, using static config.h placement\n");

	return found;
}
