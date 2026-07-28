#ifndef TRAY_H
#define TRAY_H

#include <systemd/sd-bus.h>
#include <X11/Xlib.h>

/* a StatusNotifierItem, registered over DBus */
typedef struct TrayItem {
	char *bus_name, *service, *object_path;
	char *label;             /* always set, used when no icon loads */
	unsigned char *argb;     /* w*h*4 bytes, NULL until fetched */
	int icon_w, icon_h;
	int status;              /* 0 passive, 1 active, 2 needs-attention */
	int x, width;             /* current slot, set by item_relayout */
	struct TrayItem *next;
} TrayItem;

/* a legacy XEmbed icon - a real window reparented into ours */
typedef struct EmbedItem {
	Window win;
	int x;
	struct EmbedItem *next;
} EmbedItem;

typedef struct {
	sd_bus *bus;
	TrayItem *items;
	int host_registered;

	Display *dpy;
	Window win;
	int screen;
	int width, height;
	XFontStruct *font;

	EmbedItem *embed_items;
	Atom net_system_tray_opcode, xembed_atom;
	int xembed_active;
} TrayState;

/* sni_watcher.c */
int  watcher_init(TrayState *st);
void watcher_shutdown(TrayState *st);
int  watcher_fetch_item_props(TrayState *st, TrayItem *it);
int  watcher_fetch_icon_by_name(TrayState *st, TrayItem *it);
TrayItem *item_find_by_service(TrayState *st, const char *service);
void item_remove(TrayState *st, TrayItem *it);
void item_relayout(TrayState *st);

/* icon_theme.c */
unsigned char *icon_theme_load(const char *icon_name, int pref_size,
                                const char *extra_theme_path, int *out_w, int *out_h);

/* xembed_tray.c */
int  xembed_init(TrayState *st);
void xembed_handle_client_message(TrayState *st, XClientMessageEvent *ev);
void xembed_handle_structure_notify(TrayState *st, XEvent *ev);

/* tray_window.c */
int  window_init(TrayState *st);
void window_redraw(TrayState *st);
void window_shutdown(TrayState *st);
TrayItem *window_item_at(TrayState *st, int x, int y);

#endif
