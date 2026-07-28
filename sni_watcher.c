/* implements org.freedesktop.StatusNotifierWatcher if nothing else
 * owns it yet; otherwise just registers as another host and listens
 * to the existing watcher's signals. either way, items end up in
 * st->items via item_add(). */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tray.h"
#include "config.h"

#define WATCHER_BUS_NAME "org.freedesktop.StatusNotifierWatcher"
#define WATCHER_OBJ_PATH "/StatusNotifierWatcher"
#define WATCHER_IFACE    "org.freedesktop.StatusNotifierWatcher"
#define ITEM_IFACE       "org.kde.StatusNotifierItem"

TrayItem *item_find_by_service(TrayState *st, const char *service) {
	for (TrayItem *it = st->items; it; it = it->next)
		if (!strcmp(it->service, service))
			return it;
	return NULL;
}

static void item_free(TrayItem *it) {
	free(it->bus_name);
	free(it->service);
	free(it->object_path);
	free(it->label);
	free(it->argb);
	free(it);
}

void item_remove(TrayState *st, TrayItem *it) {
	for (TrayItem **cur = &st->items; *cur; cur = &(*cur)->next) {
		if (*cur == it) {
			*cur = it->next;
			item_free(it);
			item_relayout(st);
			return;
		}
	}
}

/* best-effort readable name before we know anything else - IconName
 * or Title (fetched later) usually replace this */
static char *derive_default_label(const char *bus_name, const char *object_path) {
	const char *seg = strrchr(object_path, '/');
	if (seg && seg[1] && strcmp(seg + 1, "StatusNotifierItem"))
		return strdup(seg + 1);

	if (bus_name[0] != ':') {
		const char *dot = strrchr(bus_name, '.');
		return strdup(dot ? dot + 1 : bus_name);
	}
	return strdup(bus_name); /* unique name, ugly but visible */
}

static void item_add(TrayState *st, const char *bus_name,
                      const char *service, const char *object_path) {
	if (item_find_by_service(st, service))
		return;

	TrayItem *it = calloc(1, sizeof(*it));
	it->bus_name = strdup(bus_name);
	it->service = strdup(service);
	it->object_path = strdup(object_path);
	it->label = derive_default_label(bus_name, object_path);
	it->status = 1;
	it->next = st->items;
	st->items = it;

	watcher_fetch_item_props(st, it);
	item_relayout(st);

	fprintf(stderr, "[tray] registered: %s label=%s\n", service, it->label);
}

/* "service" arg is either an object path on the sender's bus name,
 * or a bus name whose item lives at /StatusNotifierItem */
static void split_service_arg(const char *sender, const char *arg,
                               char **bus_name, char **path) {
	if (arg[0] == '/') {
		*bus_name = strdup(sender);
		*path = strdup(arg);
	} else {
		*bus_name = strdup(arg);
		*path = strdup("/StatusNotifierItem");
	}
}

/* IconPixmap is a(iiay): (w, h, ARGB32 bytes), picks the largest.
 * blocking call, fine for a one-off fetch on registration. */
int watcher_fetch_item_props(TrayState *st, TrayItem *it) {
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	int ret = sd_bus_call_method(st->bus, it->bus_name, it->object_path,
	                              "org.freedesktop.DBus.Properties", "Get",
	                              &err, &reply, "ss", ITEM_IFACE, "IconPixmap");
	if (ret < 0) {
		sd_bus_error_free(&err);
		return watcher_fetch_icon_by_name(st, it);
	}

	sd_bus_message_enter_container(reply, 'v', "a(iiay)");
	sd_bus_message_enter_container(reply, 'a', "(iiay)");

	int best_w = 0, best_h = 0;
	unsigned char *best = NULL;

	while (sd_bus_message_enter_container(reply, 'r', "iiay") > 0) {
		int32_t w, h;
		sd_bus_message_read(reply, "ii", &w, &h);
		const void *pixels;
		size_t n;
		sd_bus_message_read_array(reply, 'y', &pixels, &n);

		if (w * h > best_w * best_h && n >= (size_t)(w * h * 4)) {
			free(best);
			best = malloc(n);
			memcpy(best, pixels, n);
			best_w = w;
			best_h = h;
		}
		sd_bus_message_exit_container(reply);
	}
	sd_bus_message_exit_container(reply);
	sd_bus_message_exit_container(reply);
	sd_bus_message_unref(reply);
	sd_bus_error_free(&err);

	if (best) {
		free(it->argb);
		it->argb = best;
		it->icon_w = best_w;
		it->icon_h = best_h;
		fprintf(stderr, "[tray] %s: got %dx%d pixmap icon\n", it->service, best_w, best_h);
		return 0;
	}

	return watcher_fetch_icon_by_name(st, it);
}

static char *get_string_prop(TrayState *st, TrayItem *it, const char *prop) {
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	char *out = NULL;

	if (sd_bus_call_method(st->bus, it->bus_name, it->object_path,
	                        "org.freedesktop.DBus.Properties", "Get", &err, &reply,
	                        "ss", ITEM_IFACE, prop) >= 0) {
		const char *s;
		if (sd_bus_message_read(reply, "v", "s", &s) >= 0 && s && *s)
			out = strdup(s);
	}
	sd_bus_message_unref(reply);
	sd_bus_error_free(&err);
	return out;
}

/* no raw pixmap bytes - resolve IconName through the icon theme
 * instead. Title becomes the label either way since it's usually
 * more readable ("Discord" vs "discord-tray"). */
int watcher_fetch_icon_by_name(TrayState *st, TrayItem *it) {
	char *icon_name = get_string_prop(st, it, "IconName");
	char *title = get_string_prop(st, it, "Title");

	if (title) {
		free(it->label);
		it->label = title;
	} else if (icon_name && (!it->label || !strcmp(it->label, it->bus_name))) {
		free(it->label);
		it->label = strdup(icon_name);
	}

	if (!icon_name) {
		fprintf(stderr, "[tray] %s: no icon data, showing label '%s'\n", it->service, it->label);
		return -1;
	}

	char *theme_path = get_string_prop(st, it, "IconThemePath");
	int w, h;
	unsigned char *buf = icon_theme_load(icon_name, icon_size, theme_path, &w, &h);
	if (buf) {
		free(it->argb);
		it->argb = buf;
		it->icon_w = w;
		it->icon_h = h;
	} else {
		fprintf(stderr, "[tray] %s: no themed icon for '%s', showing label '%s'\n",
		        it->service, icon_name, it->label);
	}

	free(icon_name);
	free(theme_path);
	return buf ? 0 : -1;
}

/* --- vtable when we own the watcher name --- */

static int method_register_item(sd_bus_message *m, void *userdata, sd_bus_error *e) {
	(void)e;
	TrayState *st = userdata;
	const char *arg;
	if (sd_bus_message_read(m, "s", &arg) < 0)
		return -1;

	char *bus_name, *path;
	split_service_arg(sd_bus_message_get_sender(m), arg, &bus_name, &path);
	char key[512];
	snprintf(key, sizeof(key), "%s%s", bus_name, path);
	item_add(st, bus_name, key, path);
	free(bus_name);
	free(path);

	sd_bus_emit_signal(st->bus, WATCHER_OBJ_PATH, WATCHER_IFACE,
	                    "StatusNotifierItemRegistered", "s", arg);
	return sd_bus_reply_method_return(m, "");
}

static int method_register_host(sd_bus_message *m, void *userdata, sd_bus_error *e) {
	(void)userdata; (void)e;
	return sd_bus_reply_method_return(m, "");
}

static int prop_items(sd_bus *bus, const char *path, const char *iface,
                       const char *prop, sd_bus_message *reply, void *userdata, sd_bus_error *e) {
	(void)bus; (void)path; (void)iface; (void)prop; (void)e;
	TrayState *st = userdata;
	sd_bus_message_open_container(reply, 'a', "s");
	for (TrayItem *it = st->items; it; it = it->next)
		sd_bus_message_append(reply, "s", it->service);
	return sd_bus_message_close_container(reply);
}

static int prop_host_registered(sd_bus *b, const char *p, const char *i,
                                 const char *pr, sd_bus_message *r, void *u, sd_bus_error *e) {
	(void)b; (void)p; (void)i; (void)pr; (void)u; (void)e;
	return sd_bus_message_append(r, "b", 1);
}

static int prop_protocol_version(sd_bus *b, const char *p, const char *i,
                                  const char *pr, sd_bus_message *r, void *u, sd_bus_error *e) {
	(void)b; (void)p; (void)i; (void)pr; (void)u; (void)e;
	return sd_bus_message_append(r, "i", 0);
}

static const sd_bus_vtable watcher_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", method_register_item, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", method_register_host, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", prop_items, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b", prop_host_registered, 0, 0),
	SD_BUS_PROPERTY("ProtocolVersion", "i", prop_protocol_version, 0, 0),
	SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
	SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
	SD_BUS_SIGNAL("StatusNotifierHostRegistered", "", 0),
	SD_BUS_SIGNAL("StatusNotifierHostUnregistered", "", 0),
	SD_BUS_VTABLE_END
};

/* --- client mode: another watcher already exists --- */

static void handle_registered(TrayState *st, const char *sender, const char *arg) {
	char *bus_name, *path;
	split_service_arg(sender, arg, &bus_name, &path);
	char key[512];
	snprintf(key, sizeof(key), "%s%s", bus_name, path);
	item_add(st, bus_name, key, path);
	free(bus_name);
	free(path);
}

static int on_item_registered(sd_bus_message *m, void *userdata, sd_bus_error *e) {
	(void)e;
	const char *arg;
	if (sd_bus_message_read(m, "s", &arg) >= 0)
		handle_registered(userdata, sd_bus_message_get_sender(m), arg);
	return 0;
}

static int on_item_unregistered(sd_bus_message *m, void *userdata, sd_bus_error *e) {
	(void)e;
	TrayState *st = userdata;
	const char *arg;
	if (sd_bus_message_read(m, "s", &arg) < 0)
		return 0;

	char *bus_name, *path;
	split_service_arg(sd_bus_message_get_sender(m), arg, &bus_name, &path);
	char key[512];
	snprintf(key, sizeof(key), "%s%s", bus_name, path);
	TrayItem *it = item_find_by_service(st, key);
	if (it) item_remove(st, it);
	free(bus_name);
	free(path);
	return 0;
}

/* app dropped off the bus without unregistering - the usual case */
static int on_name_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *e) {
	(void)e;
	TrayState *st = userdata;
	const char *name, *old_owner, *new_owner;
	if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0 || *new_owner)
		return 0;

	TrayItem *it = st->items;
	while (it) {
		TrayItem *next = it->next;
		if (!strcmp(it->bus_name, name))
			item_remove(st, it);
		it = next;
	}
	return 0;
}

static void become_host_client(TrayState *st) {
	sd_bus_call_method(st->bus, WATCHER_BUS_NAME, WATCHER_OBJ_PATH, WATCHER_IFACE,
	                    "RegisterStatusNotifierHost", NULL, NULL, "s", "systray-host");

	sd_bus_match_signal(st->bus, NULL, WATCHER_BUS_NAME, WATCHER_OBJ_PATH, WATCHER_IFACE,
	                     "StatusNotifierItemRegistered", on_item_registered, st);
	sd_bus_match_signal(st->bus, NULL, WATCHER_BUS_NAME, WATCHER_OBJ_PATH, WATCHER_IFACE,
	                     "StatusNotifierItemUnregistered", on_item_unregistered, st);

	sd_bus_message *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	int ret = sd_bus_call_method(st->bus, WATCHER_BUS_NAME, WATCHER_OBJ_PATH,
	                              "org.freedesktop.DBus.Properties", "Get", &err, &reply,
	                              "ss", WATCHER_IFACE, "RegisteredStatusNotifierItems");
	if (ret >= 0) {
		sd_bus_message_enter_container(reply, 'v', "as");
		sd_bus_message_enter_container(reply, 'a', "s");
		const char *svc;
		while (sd_bus_message_read(reply, "s", &svc) > 0) {
			char *bus_name, *path;
			split_service_arg(svc, svc, &bus_name, &path);
			item_add(st, bus_name, svc, path);
			free(bus_name);
			free(path);
		}
		sd_bus_message_exit_container(reply);
		sd_bus_message_exit_container(reply);
	}
	sd_bus_message_unref(reply);
	sd_bus_error_free(&err);

	fprintf(stderr, "[tray] existing watcher found, running as host client\n");
}

int watcher_init(TrayState *st) {
	int ret = sd_bus_open_user(&st->bus);
	if (ret < 0) {
		fprintf(stderr, "[tray] can't connect to session bus: %s\n", strerror(-ret));
		return ret;
	}

	sd_bus_match_signal(st->bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
	                     "org.freedesktop.DBus", "NameOwnerChanged", on_name_owner_changed, st);
	sd_bus_call_method(st->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
	                    "org.freedesktop.DBus", "AddMatch", NULL, NULL,
	                    "s", "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'");

	if (sd_bus_request_name(st->bus, WATCHER_BUS_NAME, 0) >= 0) {
		ret = sd_bus_add_object_vtable(st->bus, NULL, WATCHER_OBJ_PATH, WATCHER_IFACE, watcher_vtable, st);
		if (ret < 0) {
			fprintf(stderr, "[tray] failed to add watcher vtable: %s\n", strerror(-ret));
			return ret;
		}
		st->host_registered = 1;
		fprintf(stderr, "[tray] no existing watcher, running as StatusNotifierWatcher\n");
	} else {
		become_host_client(st);
	}

	return 0;
}

void watcher_shutdown(TrayState *st) {
	while (st->items)
		item_remove(st, st->items);
	if (st->bus)
		sd_bus_unref(st->bus);
}
