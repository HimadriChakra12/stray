/* tray_window.c
 *
 * A thin override-redirect X11 window docked to a screen corner,
 * drawing each tracked item's ARGB icon in a horizontal strip.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include "tray.h"
#include "config.h"

#define MAX_LABEL_CHARS 10

/* width this item occupies in the strip: icon box if we have pixel
 * data, otherwise enough room for its (possibly truncated) label */
static int item_width(TrayState *st, TrayItem *it) {
	if (it->argb)
		return icon_size;

	int len = strlen(it->label);
	if (len > MAX_LABEL_CHARS) len = MAX_LABEL_CHARS;

	if (st->font)
		return XTextWidth(st->font, it->label, len) + 8;

	return len * 7 + 8; /* rough estimate if no font loaded yet */
}

/* recompute width/height and x/y position from item count + gravity.
 * XEmbed-docked windows are laid out first (they're real child
 * windows we just position), then SNI items after. */
void item_relayout(TrayState *st) {
	int x = pad_x;

	for (EmbedItem *e = st->embed_items; e; e = e->next) {
		e->x = x;
		if (st->dpy)
			XMoveWindow(st->dpy, e->win, x, pad_y);
		x += icon_size + icon_gap;
	}

	for (TrayItem *it = st->items; it; it = it->next) {
		it->x = x;
		it->width = item_width(st, it);
		x += it->width + icon_gap;
	}

	int any = st->embed_items || st->items;
	int w = any ? (x - icon_gap + pad_x) : (pad_x * 2 + icon_size);
	int h = tray_h;

	st->width = w;
	st->height = h;

	if (!st->dpy || st->win == 0)
		return; /* window not created yet */

	int screen_w = DisplayWidth(st->dpy, st->screen);
	int screen_h = DisplayHeight(st->dpy, st->screen);

	int win_x = offset_x, win_y = offset_y;
	if (strstr(tray_gravity, "right"))
		win_x = screen_w - w - offset_x;
	if (strstr(tray_gravity, "bottom"))
		win_y = screen_h - h - offset_y;

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

	int screen_w = DisplayWidth(st->dpy, st->screen);
	(void)screen_w;

	XSetWindowAttributes attrs;
	attrs.override_redirect = True;
	attrs.background_pixel = bg_color;
	attrs.event_mask = ExposureMask | ButtonPressMask | StructureNotifyMask | SubstructureNotifyMask;

	st->width = pad_x * 2 + icon_size;
	st->height = tray_h;

	st->win = XCreateWindow(st->dpy, RootWindow(st->dpy, st->screen),
	                         0, 0, st->width, st->height, 0,
	                         CopyFromParent, InputOutput, CopyFromParent,
	                         CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);

	/* mark as a dock/panel-type window for WMs that respect EWMH even
	 * with override-redirect (some do, for stacking order) */
	Atom window_type = XInternAtom(st->dpy, "_NET_WM_WINDOW_TYPE", False);
	Atom dock = XInternAtom(st->dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
	XChangeProperty(st->dpy, st->win, window_type, XA_ATOM, 32,
	                 PropModeReplace, (unsigned char *)&dock, 1);

	XMapWindow(st->dpy, st->win);

	st->font = XLoadQueryFont(st->dpy, "-*-fixed-medium-r-*--12-*-*-*-*-*-*-*");
	if (!st->font)
		st->font = XLoadQueryFont(st->dpy, "fixed");
	if (!st->font)
		fprintf(stderr, "[tray] warning: no font found, labels will use fallback sizing\n");

	item_relayout(st);
	XFlush(st->dpy);
	return 0;
}

/* draws an already-decoded ARGB icon (from DBus IconPixmap or the
 * themed icon-file loader) scaled into the icon box at dst_x */
static void blit_icon(TrayState *st, TrayItem *it, int dst_x, int dst_y) {
	XImage *img = XCreateImage(st->dpy, DefaultVisual(st->dpy, st->screen), 24, ZPixmap,
	                            0, malloc(icon_size * icon_size * 4), icon_size, icon_size, 32, 0);

	for (int y = 0; y < icon_size; y++) {
		int sy = y * it->icon_h / icon_size;
		for (int x = 0; x < icon_size; x++) {
			int sx = x * it->icon_w / icon_size;
			const unsigned char *px = it->argb + (sy * it->icon_w + sx) * 4;
			/* network order: byte0=A byte1=R byte2=G byte3=B */
			unsigned long pixel = (px[1] << 16) | (px[2] << 8) | px[3];
			XPutPixel(img, x, y, pixel);
		}
	}

	XPutImage(st->dpy, st->win, DefaultGC(st->dpy, st->screen), img,
	          0, 0, dst_x, dst_y, icon_size, icon_size);
	XDestroyImage(img); /* also frees the malloc'd data buffer */
}

/* no icon could be loaded (DBus fetch failed AND theme lookup found
 * nothing) - draw the best label we have so the item is still
 * readable and you can tell registration is working */
static void draw_label(TrayState *st, TrayItem *it, int dst_x, int width) {
	(void)width;
	GC gc = XCreateGC(st->dpy, st->win, 0, NULL);
	XSetForeground(st->dpy, gc, 0x9aa5ce); /* light gray-blue, readable on dark bg */
	if (st->font)
		XSetFont(st->dpy, gc, st->font->fid);

	int len = strlen(it->label);
	if (len > MAX_LABEL_CHARS) len = MAX_LABEL_CHARS;

	int text_h = st->font ? (st->font->ascent + st->font->descent) : 10;
	int baseline_y = pad_y + (icon_size + text_h) / 2 - (st->font ? st->font->descent : 0);

	XDrawString(st->dpy, st->win, gc, dst_x + 4, baseline_y, it->label, len);
	XFreeGC(st->dpy, gc);
}

static void render_item(TrayState *st, TrayItem *it) {
	if (it->argb && it->icon_w > 0 && it->icon_h > 0)
		blit_icon(st, it, it->x, pad_y);
	else
		draw_label(st, it, it->x, it->width);
}

void window_redraw(TrayState *st) {
	if (!st->dpy) return;

	GC gc = XCreateGC(st->dpy, st->win, 0, NULL);
	XSetForeground(st->dpy, gc, bg_color);
	XFillRectangle(st->dpy, st->win, gc, 0, 0, st->width, st->height);
	XFreeGC(st->dpy, gc);

	for (TrayItem *it = st->items; it; it = it->next)
		render_item(st, it);

	XFlush(st->dpy);
}

TrayItem *window_item_at(TrayState *st, int x, int y) {
	if (y < pad_y || y > pad_y + icon_size)
		return NULL;

	for (TrayItem *it = st->items; it; it = it->next)
		if (x >= it->x && x < it->x + it->width)
			return it;
	return NULL;
}

void window_shutdown(TrayState *st) {
	if (st->dpy) {
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
}
