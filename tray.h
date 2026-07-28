#ifndef TRAY_H
#define TRAY_H

#include <systemd/sd-bus.h>
#include <X11/Xlib.h>

/* one registered StatusNotifierItem */
typedef struct TrayItem {
	char *bus_name;     /* unique DBus name that owns the item, e.g. ":1.234" */
	char *service;      /* string passed to RegisterStatusNotifierItem, e.g. "org.discord" or ":1.234/StatusNotifierItem" */
	char *object_path;  /* object path on that bus name, usually /StatusNotifierItem */
	char *label;        /* short human-readable name, always set - used when no icon could be loaded */

	unsigned char *argb; /* decoded 32-bit ARGB pixel data, width*height*4 bytes, NULL until fetched */
	int icon_w, icon_h;

	int status; /* 0=Passive 1=Active 2=NeedsAttention */
	int x, width; /* current on-screen slot, set by item_relayout - used for hit testing */

	struct TrayItem *next;
} TrayItem;

/* a legacy XEmbed-docked tray icon - the app's actual X11 window is
 * reparented into ours, so we don't draw it ourselves, just position it */
typedef struct EmbedItem {
	Window win;
	int x;
	struct EmbedItem *next;
} EmbedItem;

typedef struct {
	sd_bus *bus;
	TrayItem *items;
	int host_registered; /* did we successfully claim the watcher role */

	Display *dpy;
	Window win;
	int screen;
	int width, height; /* current tray window size */
	XFontStruct *font;

	EmbedItem *embed_items;
	Atom net_system_tray_opcode;
	Atom xembed_atom;
	int xembed_active; /* did we successfully claim the tray selection */
} TrayState;

/* sni_watcher.c */
int  watcher_init(TrayState *st);
void watcher_shutdown(TrayState *st);
int  watcher_fetch_item_props(TrayState *st, TrayItem *it);
int  watcher_fetch_icon_by_name(TrayState *st, TrayItem *it);

/* icon_theme.c */
unsigned char *icon_theme_load(const char *icon_name, int pref_size,
                                const char *extra_theme_path, int *out_w, int *out_h);

/* xembed_tray.c - legacy XEmbed systray protocol, for apps (Java
 * Swing/AWT, older GTK2, etc) that never adopted StatusNotifierItem */
int  xembed_init(TrayState *st);
void xembed_handle_client_message(TrayState *st, XClientMessageEvent *ev);
void xembed_handle_structure_notify(TrayState *st, XEvent *ev);

/* tray_window.c */
int  window_init(TrayState *st);
void window_redraw(TrayState *st);
void window_shutdown(TrayState *st);
/* returns the item under (x,y) in the strip, or NULL */
TrayItem *window_item_at(TrayState *st, int x, int y);

/* item list helpers, in sni_watcher.c */
TrayItem *item_find_by_service(TrayState *st, const char *service);
void item_remove(TrayState *st, TrayItem *it);
void item_relayout(TrayState *st);

#endif
