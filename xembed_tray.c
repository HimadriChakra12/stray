/* legacy XEmbed systray protocol, for apps that never adopted SNI
 * (Java Swing/AWT, older GTK2/Qt4 - Xtreme Download Manager is Swing).
 * We claim _NET_SYSTEM_TRAY_S<screen>, apps dock by sending us a
 * ClientMessage, we reparent their real window into ours. */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include "tray.h"
#include "config.h"

#define SYSTEM_TRAY_REQUEST_DOCK 0
#define XEMBED_EMBEDDED_NOTIFY 0

int xembed_init(TrayState *st) {
	char buf[64];
	snprintf(buf, sizeof(buf), "_NET_SYSTEM_TRAY_S%d", st->screen);
	Atom selection = XInternAtom(st->dpy, buf, False);
	st->net_system_tray_opcode = XInternAtom(st->dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
	st->xembed_atom = XInternAtom(st->dpy, "_XEMBED", False);

	Window owner = XGetSelectionOwner(st->dpy, selection);
	if (owner != None) {
		fprintf(stderr, "[tray] another XEmbed tray already running (0x%lx), "
		                "skipping legacy tray support\n", owner);
		return -1;
	}

	XSetSelectionOwner(st->dpy, selection, st->win, CurrentTime);
	if (XGetSelectionOwner(st->dpy, selection) != st->win) {
		fprintf(stderr, "[tray] failed to claim XEmbed tray selection\n");
		return -1;
	}

	XClientMessageEvent ev = {
		.type = ClientMessage,
		.window = RootWindow(st->dpy, st->screen),
		.message_type = XInternAtom(st->dpy, "MANAGER", False),
		.format = 32,
	};
	ev.data.l[0] = CurrentTime;
	ev.data.l[1] = (long)selection;
	ev.data.l[2] = (long)st->win;
	XSendEvent(st->dpy, ev.window, False, StructureNotifyMask, (XEvent *)&ev);

	st->xembed_active = 1;
	fprintf(stderr, "[tray] claimed XEmbed tray selection, legacy icons now supported\n");
	return 0;
}

void xembed_handle_client_message(TrayState *st, XClientMessageEvent *ev) {
	if (ev->message_type != st->net_system_tray_opcode || ev->data.l[1] != SYSTEM_TRAY_REQUEST_DOCK)
		return;

	Window win = (Window)ev->data.l[2];
	EmbedItem *it = calloc(1, sizeof(*it));
	it->win = win;
	it->next = st->embed_items;
	st->embed_items = it;

	XSelectInput(st->dpy, win, StructureNotifyMask | PropertyChangeMask);
	XReparentWindow(st->dpy, win, st->win, 0, 0);
	XResizeWindow(st->dpy, win, icon_size, icon_size);
	XMapWindow(st->dpy, win);

	XClientMessageEvent notify = {
		.type = ClientMessage, .window = win,
		.message_type = st->xembed_atom, .format = 32,
	};
	notify.data.l[0] = CurrentTime;
	notify.data.l[1] = XEMBED_EMBEDDED_NOTIFY;
	notify.data.l[3] = (long)st->win;
	XSendEvent(st->dpy, win, False, NoEventMask, (XEvent *)&notify);

	fprintf(stderr, "[tray] XEmbed icon docked: 0x%lx\n", win);
	item_relayout(st);
}

void xembed_handle_structure_notify(TrayState *st, XEvent *ev) {
	Window w;
	if (ev->type == DestroyNotify) w = ev->xdestroywindow.window;
	else if (ev->type == UnmapNotify) w = ev->xunmap.window;
	else if (ev->type == ReparentNotify && ev->xreparent.parent != st->win) w = ev->xreparent.window;
	else return;

	for (EmbedItem **cur = &st->embed_items; *cur; cur = &(*cur)->next) {
		if ((*cur)->win == w) {
			EmbedItem *dead = *cur;
			*cur = dead->next;
			free(dead);
			fprintf(stderr, "[tray] XEmbed icon removed: 0x%lx\n", w);
			item_relayout(st);
			return;
		}
	}
}
