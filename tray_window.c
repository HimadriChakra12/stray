/* the strip window: layout, drawing, hit-testing. XEmbed icons are
 * real child windows we just position; SNI items we draw ourselves. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include "tray.h"
#include "config.h"

#define MAX_LABEL_CHARS 10

/* center within whatever height we're using - a detected bar's, or
 * config.h's static tray_h if none was found */
static int vpad(TrayState *st) {
	int h = st->dock_found ? st->dock_h : tray_h;
	int v = (h - icon_size) / 2;
	return v > 0 ? v : 0;
}

static unsigned long effective_bg(TrayState *st) {
	return st->dock_found ? st->dock_bg : bg_color;
}

static int item_width(TrayState *st, TrayItem *it) {
	if (it->argb)
		return icon_size;

	int len = strlen(it->label);
	if (len > MAX_LABEL_CHARS) len = MAX_LABEL_CHARS;
	if (st->font)
		return XTextWidth(st->font, it->label, len) + 8;
	return len * 7 + 8;
}

/* lays out embed icons then SNI items left to right, then docks the
 * whole strip flush against a detected bar (whichever side has
 * room), or falls back to config.h's static gravity/offset */
void item_relayout(TrayState *st) {
	int vp = vpad(st);
	int x = pad_x;

	for (EmbedItem *e = st->embed_items; e; e = e->next) {
		e->x = x;
		if (st->dpy)
			XMoveWindow(st->dpy, e->win, x, vp);
		x += icon_size + icon_gap;
	}
	for (TrayItem *it = st->items; it; it = it->next) {
		it->x = x;
		it->width = item_width(st, it);
		x += it->width + icon_gap;
	}

	int any = st->embed_items || st->items;
	int w = any ? (x - icon_gap + pad_x) : (pad_x * 2 + icon_size);
	int h = st->dock_found ? st->dock_h : tray_h;
	st->width = w;
	st->height = h;

	if (!st->dpy || !st->win)
		return;

	int screen_w = DisplayWidth(st->dpy, st->screen);
	int screen_h = DisplayHeight(st->dpy, st->screen);
	int win_x, win_y;

	if (st->dock_found) {
		int gap_right = screen_w - (st->dock_x + st->dock_w);
		int gap_left = st->dock_x;

		if (gap_right >= w)
			win_x = st->dock_x + st->dock_w;
		else if (gap_left >= w)
			win_x = st->dock_x - w;
		else {
			win_x = st->dock_x + st->dock_w - w;
			fprintf(stderr, "[tray] no gap beside the detected bar (leave some "
			                "padding in its config) - overlapping its edge\n");
		}
		win_y = st->dock_y;
	} else {
		win_x = strstr(tray_gravity, "right") ? screen_w - w - offset_x : offset_x;
		win_y = strstr(tray_gravity, "bottom") ? screen_h - h - offset_y : offset_y;
	}

	XMoveResizeWindow(st->dpy, st->win, win_x, win_y, w, h);
	window_redraw(st);
}

int window_init(TrayState *st) {
	st->dpy = XOpenDisplay(NULL);
	if (!st->dpy) {
		fprintf(stderr, "[tray] cannot open X display\n");
		return -1;
	}
	st->screen = DefaultScreen(st->dpy);
	bar_detect(st); /* best-effort, falls back to config.h if nothing found */

	XSetWindowAttributes attrs = {
		.override_redirect = True,
		.background_pixel = effective_bg(st),
		.event_mask = ExposureMask | ButtonPressMask | StructureNotifyMask | SubstructureNotifyMask,
	};

	st->width = pad_x * 2 + icon_size;
	st->height = st->dock_found ? st->dock_h : tray_h;

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

/* nearest-neighbor scale of an ARGB icon into the icon box, alpha
 * blended against the tray background. bytes are in the network-
 * order layout DBus uses: A,R,G,B. most icon PNGs store RGB=0,0,0
 * for fully-transparent pixels - skipping the alpha blend paints
 * those as solid black instead of the actual background. */
static void blit_icon(TrayState *st, TrayItem *it, int x, int y) {
	unsigned long bg = effective_bg(st);
	int bg_r = (bg >> 16) & 0xff, bg_g = (bg >> 8) & 0xff, bg_b = bg & 0xff;

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

/* no icon loaded - draw the best label we have instead of a blank box */
static void draw_label(TrayState *st, TrayItem *it, int x) {
	GC gc = XCreateGC(st->dpy, st->win, 0, NULL);
	XSetForeground(st->dpy, gc, 0x9aa5ce);
	if (st->font)
		XSetFont(st->dpy, gc, st->font->fid);

	int len = strlen(it->label);
	if (len > MAX_LABEL_CHARS) len = MAX_LABEL_CHARS;
	int text_h = st->font ? st->font->ascent + st->font->descent : 10;
	int baseline = vpad(st) + (icon_size + text_h) / 2 - (st->font ? st->font->descent : 0);

	XDrawString(st->dpy, st->win, gc, x + 4, baseline, it->label, len);
	XFreeGC(st->dpy, gc);
}

void window_redraw(TrayState *st) {
	if (!st->dpy) return;

	GC gc = XCreateGC(st->dpy, st->win, 0, NULL);
	XSetForeground(st->dpy, gc, effective_bg(st));
	XFillRectangle(st->dpy, st->win, gc, 0, 0, st->width, st->height);
	XFreeGC(st->dpy, gc);

	for (TrayItem *it = st->items; it; it = it->next) {
		if (it->argb && it->icon_w > 0 && it->icon_h > 0)
			blit_icon(st, it, it->x, vpad(st));
		else
			draw_label(st, it, it->x);
	}
	XFlush(st->dpy);
}

TrayItem *window_item_at(TrayState *st, int x, int y) {
	int vp = vpad(st);
	if (y < vp || y > vp + icon_size)
		return NULL;
	for (TrayItem *it = st->items; it; it = it->next)
		if (x >= it->x && x < it->x + it->width)
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
