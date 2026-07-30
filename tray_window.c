/* the tray window: a small corner button when collapsed, a vertical
 * dropdown of icons when expanded. top corners drop down, bottom
 * corners pop up - the button always stays put at its corner, the
 * panel grows toward the center of the screen. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include "tray.h"
#include "config.h"

#define MAX_LABEL_CHARS 10

static int is_top(void)   { return strstr(tray_gravity, "top") != NULL; }
static int is_right(void) { return strstr(tray_gravity, "right") != NULL; }

static int item_width(TrayState *st, TrayItem *it) {
	if (it->argb)
		return icon_size;
	int len = strlen(it->label);
	if (len > MAX_LABEL_CHARS) len = MAX_LABEL_CHARS;
	if (st->font)
		return XTextWidth(st->font, it->label, len) + 8;
	return len * 7 + 8;
}

/* stacks the toggle button plus every icon vertically. the button
 * always stays fixed at the actual screen corner: top-corner trays
 * put it first (icons stack downward below it), bottom-corner trays
 * put it last (icons stack upward above it) - either way the button
 * never moves on screen, only the icon list grows away from it. */
void item_relayout(TrayState *st) {
	int n = 0;
	int col_w = icon_size;
	for (EmbedItem *e = st->embed_items; e; e = e->next) n++;
	for (TrayItem *it = st->items; it; it = it->next) {
		it->width = item_width(st, it);
		if (it->width > col_w) col_w = it->width;
		n++;
	}

	int w = pad_x * 2 + (st->collapsed ? icon_size : col_w);
	int h = pad_x * 2 + icon_size;

	if (!st->collapsed && n > 0)
		h += icon_gap + n * icon_size + (n - 1) * icon_gap;

	st->width = w;
	st->height = h;

	if (!st->dpy || !st->win)
		return;

	int top_first = is_top();
	int y = top_first ? pad_x + icon_size + icon_gap : pad_x;

	if (!st->collapsed && n > 0) {
		for (EmbedItem *e = st->embed_items; e; e = e->next) {
			e->y = y;
			y += icon_size + icon_gap;
		}
		for (TrayItem *it = st->items; it; it = it->next) {
			it->y = y;
			y += icon_size + icon_gap;
		}
	}

	/* XEmbed windows stay mapped always - moving them off past the
	 * collapsed parent's tiny bounds hides them via X clipping.
	 * unmapping/remapping to hide them instead can leave Swing/AWT
	 * tray icons (XDM) blank, since some never repaint after that
	 * cycle. */
	for (EmbedItem *e = st->embed_items; e; e = e->next) {
		if (st->collapsed)
			XMoveWindow(st->dpy, e->win, pad_x, st->height + icon_size);
		else
			XMoveWindow(st->dpy, e->win, pad_x, e->y);
	}

	int screen_w = DisplayWidth(st->dpy, st->screen);
	int screen_h = DisplayHeight(st->dpy, st->screen);
	int win_x = is_right() ? screen_w - w - offset_x : offset_x;
	int win_y = top_first ? offset_y : screen_h - h - offset_y;

	XMoveResizeWindow(st->dpy, st->win, win_x, win_y, w, h);
	window_redraw(st);
}

/* y-range of the toggle button row, given the current corner */
static void toggle_row(TrayState *st, int *y0, int *y1) {
	if (is_top()) {
		*y0 = pad_x;
		*y1 = pad_x + icon_size;
	} else {
		*y0 = st->height - pad_x - icon_size;
		*y1 = st->height - pad_x;
	}
}

int window_init(TrayState *st) {
	st->dpy = XOpenDisplay(NULL);
	if (!st->dpy) {
		fprintf(stderr, "[tray] cannot open X display\n");
		return -1;
	}
	st->screen = DefaultScreen(st->dpy);
	st->collapsed = start_collapsed;

	XSetWindowAttributes attrs = {
		.override_redirect = True,
		.background_pixel = bg_color,
		.event_mask = ExposureMask | ButtonPressMask | StructureNotifyMask | SubstructureNotifyMask,
	};

	st->width = st->height = pad_x * 2 + icon_size;

	st->win = XCreateWindow(st->dpy, RootWindow(st->dpy, st->screen), 0, 0,
	                         st->width, st->height, 0, CopyFromParent, InputOutput, CopyFromParent,
	                         CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);

	Atom type = XInternAtom(st->dpy, "_NET_WM_WINDOW_TYPE", False);
	Atom dock = XInternAtom(st->dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
	XChangeProperty(st->dpy, st->win, type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&dock, 1);

	XMapWindow(st->dpy, st->win);

	st->font = XLoadQueryFont(st->dpy, "-*-fixed-medium-r-*--12-*-*-*-*-*-*-*");
	if (!st->font)
		st->font = XLoadQueryFont(st->dpy, "fixed");
	if (!st->font)
		fprintf(stderr, "[tray] no font found, label sizing will be approximate\n");

	item_relayout(st);
	XFlush(st->dpy);
	return 0;
}

/* nearest-neighbor scale of an ARGB icon, alpha blended against the
 * tray background - most icon PNGs store RGB=0,0,0 for transparent
 * pixels, so skipping the blend paints them solid black */
static void blit_icon(TrayState *st, TrayItem *it, int x, int y) {
	int bg_r = (bg_color >> 16) & 0xff, bg_g = (bg_color >> 8) & 0xff, bg_b = bg_color & 0xff;

	XImage *img = XCreateImage(st->dpy, DefaultVisual(st->dpy, st->screen), 24, ZPixmap, 0,
	                            malloc(icon_size * icon_size * 4), icon_size, icon_size, 32, 0);

	for (int dy = 0; dy < icon_size; dy++) {
		int sy = dy * it->icon_h / icon_size;
		for (int dx = 0; dx < icon_size; dx++) {
			int sx = dx * it->icon_w / icon_size;
			const unsigned char *px = it->argb + (sy * it->icon_w + sx) * 4;
			int a = px[0];
			int r = (px[1] * a + bg_r * (255 - a)) / 255;
			int g = (px[2] * a + bg_g * (255 - a)) / 255;
			int b = (px[3] * a + bg_b * (255 - a)) / 255;
			XPutPixel(img, dx, dy, (r << 16) | (g << 8) | b);
		}
	}

	XPutImage(st->dpy, st->win, DefaultGC(st->dpy, st->screen), img, 0, 0, x, y, icon_size, icon_size);
	XDestroyImage(img);
}

static void draw_label(TrayState *st, TrayItem *it, int y) {
	GC gc = XCreateGC(st->dpy, st->win, 0, NULL);
	XSetForeground(st->dpy, gc, toggle_color);
	if (st->font)
		XSetFont(st->dpy, gc, st->font->fid);

	int len = strlen(it->label);
	if (len > MAX_LABEL_CHARS) len = MAX_LABEL_CHARS;
	int text_h = st->font ? st->font->ascent + st->font->descent : 10;
	int baseline = y + (icon_size + text_h) / 2 - (st->font ? st->font->descent : 0);

	XDrawString(st->dpy, st->win, gc, pad_x, baseline, it->label, len);
	XFreeGC(st->dpy, gc);
}

/* chevron pointing the direction the panel will grow when clicked:
 * collapsed at a top corner -> down, collapsed at a bottom corner ->
 * up, and the reverse once expanded (click again to close) */
static void draw_toggle(TrayState *st) {
	int y0, y1;
	toggle_row(st, &y0, &y1);
	int cx = st->width / 2, cy = (y0 + y1) / 2;
	int r = icon_size / 4;
	int down = st->collapsed ? is_top() : !is_top();

	GC gc = XCreateGC(st->dpy, st->win, 0, NULL);
	XSetForeground(st->dpy, gc, toggle_color);
	XPoint pts[3];
	if (down) {
		pts[0] = (XPoint){cx - r, cy - r / 2};
		pts[1] = (XPoint){cx + r, cy - r / 2};
		pts[2] = (XPoint){cx, cy + r / 2};
	} else {
		pts[0] = (XPoint){cx - r, cy + r / 2};
		pts[1] = (XPoint){cx + r, cy + r / 2};
		pts[2] = (XPoint){cx, cy - r / 2};
	}
	XFillPolygon(st->dpy, st->win, gc, pts, 3, Convex, CoordModeOrigin);
	XFreeGC(st->dpy, gc);
}

void window_redraw(TrayState *st) {
	if (!st->dpy) return;

	GC gc = XCreateGC(st->dpy, st->win, 0, NULL);
	XSetForeground(st->dpy, gc, bg_color);
	XFillRectangle(st->dpy, st->win, gc, 0, 0, st->width, st->height);
	XFreeGC(st->dpy, gc);

	draw_toggle(st);

	if (!st->collapsed)
		for (TrayItem *it = st->items; it; it = it->next) {
			if (it->argb && it->icon_w > 0 && it->icon_h > 0)
				blit_icon(st, it, pad_x, it->y);
			else
				draw_label(st, it, it->y);
		}

	XFlush(st->dpy);
}

int window_is_toggle(TrayState *st, int x, int y) {
	if (x < 0 || x >= st->width)
		return 0;
	int y0, y1;
	toggle_row(st, &y0, &y1);
	return y >= y0 && y < y1;
}

TrayItem *window_item_at(TrayState *st, int x, int y) {
	if (st->collapsed || x < pad_x || x >= st->width - pad_x)
		return NULL;
	for (TrayItem *it = st->items; it; it = it->next)
		if (y >= it->y && y < it->y + icon_size)
			return it;
	return NULL;
}

void window_shutdown(TrayState *st) {
	if (!st->dpy)
		return;

	Window root = RootWindow(st->dpy, st->screen);
	while (st->embed_items) {
		EmbedItem *e = st->embed_items;
		XUnmapWindow(st->dpy, e->win);
		XReparentWindow(st->dpy, e->win, root, 0, 0);
		st->embed_items = e->next;
		free(e);
	}
	if (st->font) XFreeFont(st->dpy, st->font);
	if (st->win) XDestroyWindow(st->dpy, st->win);
	XCloseDisplay(st->dpy);
}
