/* xembed_tray.c
 *
 * StatusNotifierItem (sni_watcher.c) only covers apps that adopted
 * the newer DBus-based tray spec. A lot of things never did -
 * notably Java's java.awt.SystemTray (so Swing apps like XDM), and
 * various older GTK2/Qt4 apps. Those instead speak the original
 * freedesktop "System Tray Protocol", which works over plain X11:
 *
 *   1. We claim the _NET_SYSTEM_TRAY_S<screen> selection on our
 *      window - this makes us "the" system tray for this screen.
 *   2. We broadcast a MANAGER ClientMessage so already-running apps
 *      waiting for a tray notice one just appeared.
 *   3. Apps send us a _NET_SYSTEM_TRAY_OPCODE ClientMessage with
 *      SYSTEM_TRAY_REQUEST_DOCK and their window ID.
 *   4. We XReparentWindow() their actual window into ours and resize
 *      it - unlike SNI this is their real window, not icon bytes, so
 *      we never draw it ourselves, just position it.
 *   5. We tell them they're embedded via an _XEMBED ClientMessage.
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include "tray.h"
#include "config.h"

#define SYSTEM_TRAY_REQUEST_DOCK   0
#define SYSTEM_TRAY_BEGIN_MESSAGE  1
#define SYSTEM_TRAY_CANCEL_MESSAGE 2

#define XEMBED_EMBEDDED_NOTIFY 0

static Atom tray_selection_atom(Display *dpy, int screen) {
	char buf[64];
	snprintf(buf, sizeof(buf), "_NET_SYSTEM_TRAY_S%d", screen);
	return XInternAtom(dpy, buf, False);
}

int xembed_init(TrayState *st) {
	Atom selection_atom = tray_selection_atom(st->dpy, st->screen);
	st->net_system_tray_opcode = XInternAtom(st->dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
	st->xembed_atom = XInternAtom(st->dpy, "_XEMBED", False);

	/* if something else already claims this (another tray, or a DE's
	 * built-in one), don't fight it - fall back to SNI-only */
	Window owner = XGetSelectionOwner(st->dpy, selection_atom);
	if (owner != None) {
		fprintf(stderr, "[tray] another XEmbed tray is already running (window 0x%lx), "
		                "skipping legacy tray support\n", owner);
		return -1;
	}

	XSetSelectionOwner(st->dpy, selection_atom, st->win, CurrentTime);
	if (XGetSelectionOwner(st->dpy, selection_atom) != st->win) {
		fprintf(stderr, "[tray] failed to claim XEmbed tray selection\n");
		return -1;
	}

	XClientMessageEvent ev = {0};
	ev.type = ClientMessage;
	ev.window = RootWindow(st->dpy, st->screen);
	ev.message_type = XInternAtom(st->dpy, "MANAGER", False);
	ev.format = 32;
	ev.data.l[0] = CurrentTime;
	ev.data.l[1] = (long)selection_atom;
	ev.data.l[2] = (long)st->win;
	XSendEvent(st->dpy, RootWindow(st->dpy, st->screen), False,
	           StructureNotifyMask, (XEvent *)&ev);

	st->xembed_active = 1;
	fprintf(stderr, "[tray] claimed XEmbed system tray selection - legacy tray icons (XDM, etc) now supported\n");
	return 0;
}

void xembed_handle_client_message(TrayState *st, XClientMessageEvent *ev) {
	if (ev->message_type != st->net_system_tray_opcode)
		return;
	if (ev->data.l[1] != SYSTEM_TRAY_REQUEST_DOCK)
		return;

	Window embed_win = (Window)ev->data.l[2];

	EmbedItem *it = calloc(1, sizeof(EmbedItem));
	it->win = embed_win;
	it->next = st->embed_items;
	st->embed_items = it;

	/* catch the icon disappearing (app exits or unmaps its tray icon) */
	XSelectInput(st->dpy, embed_win, StructureNotifyMask | PropertyChangeMask);

	XReparentWindow(st->dpy, embed_win, st->win, 0, pad_y);
	XResizeWindow(st->dpy, embed_win, icon_size, icon_size);
	XMapWindow(st->dpy, embed_win);

	XClientMessageEvent notify = {0};
	notify.type = ClientMessage;
	notify.window = embed_win;
	notify.message_type = st->xembed_atom;
	notify.format = 32;
	notify.data.l[0] = CurrentTime;
	notify.data.l[1] = XEMBED_EMBEDDED_NOTIFY;
	notify.data.l[2] = 0;
	notify.data.l[3] = (long)st->win;
	notify.data.l[4] = 0;
	XSendEvent(st->dpy, embed_win, False, NoEventMask, (XEvent *)&notify);

	fprintf(stderr, "[tray] XEmbed icon docked: window 0x%lx\n", embed_win);
	item_relayout(st);
}

void xembed_handle_structure_notify(TrayState *st, XEvent *ev) {
	Window w;
	if (ev->type == DestroyNotify)
		w = ev->xdestroywindow.window;
	else if (ev->type == UnmapNotify)
		w = ev->xunmap.window;
	else if (ev->type == ReparentNotify && ev->xreparent.parent != st->win)
		w = ev->xreparent.window; /* app reparented itself elsewhere/crashed out */
	else
		return;

	EmbedItem **cur = &st->embed_items;
	while (*cur) {
		if ((*cur)->win == w) {
			EmbedItem *dead = *cur;
			*cur = dead->next;
			free(dead);
			fprintf(stderr, "[tray] XEmbed icon removed: window 0x%lx\n", w);
			item_relayout(st);
			return;
		}
		cur = &(*cur)->next;
	}
}
