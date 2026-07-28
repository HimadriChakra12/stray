/* sni_watcher.c
 *
 * Implements enough of the StatusNotifierWatcher / StatusNotifierHost
 * spec to track tray items. If no other watcher is running on the
 * session bus, we become the watcher ourselves. If one already exists
 * (common on KDE/GNOME sessions, or if you run two trays), we just
 * register as an additional host and listen to its signals.
 */

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

/* ---- item list helpers -------------------------------------------- */

TrayItem *item_find_by_service(TrayState *st, const char *service) {
	for (TrayItem *it = st->items; it; it = it->next)
		if (strcmp(it->service, service) == 0)
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
	TrayItem **cur = &st->items;
	while (*cur) {
		if (*cur == it) {
			*cur = it->next;
			item_free(it);
			item_relayout(st);
			return;
		}
		cur = &(*cur)->next;
	}
}

/* best-effort readable name before we know anything else about the
 * item - preferred source is IconName/Title once fetched, but this
 * guarantees SOMETHING renders immediately on registration. */
static char *derive_default_label(const char *bus_name, const char *object_path) {
	/* a custom object path like /org/ayatana/NotificationItem/discord
	 * or /StatusNotifierItem/discord usually carries the app name in
	 * its last segment */
	const char *last_slash = strrchr(object_path, '/');
	if (last_slash && last_slash[1] &&
	    strcmp(last_slash + 1, "StatusNotifierItem") != 0) {
		return strdup(last_slash + 1);
	}

	/* well-known bus names look like org.discordapp.Discord - take
	 * the last dotted segment */
	if (bus_name[0] != ':') {
		const char *last_dot = strrchr(bus_name, '.');
		return strdup(last_dot ? last_dot + 1 : bus_name);
	}

	/* unique name like ":1.234" - not readable, but still visible */
	return strdup(bus_name);
}

static void item_add(TrayState *st, const char *bus_name,
                      const char *service, const char *object_path) {
	if (item_find_by_service(st, service))
		return; /* already tracked */

	TrayItem *it = calloc(1, sizeof(TrayItem));
	it->bus_name = strdup(bus_name);
	it->service = strdup(service);
	it->object_path = strdup(object_path);
	it->label = derive_default_label(bus_name, object_path);
	it->status = 1; /* assume Active until told otherwise */
	it->next = st->items;
	st->items = it;

	watcher_fetch_item_props(st, it);
	item_relayout(st);

	fprintf(stderr, "[tray] item registered: %s (bus=%s path=%s) label=%s\n",
	        service, bus_name, object_path, it->label);
}

/* split a RegisterStatusNotifierItem "service" argument into
 * (bus_name, object_path). Per spec: if it starts with '/', it's an
 * object path on the sender's bus name. Otherwise it's a bus name
 * and the path defaults to /StatusNotifierItem. */
static void split_service_arg(const char *sender, const char *arg,
                               char **out_bus_name, char **out_path) {
	if (arg[0] == '/') {
		*out_bus_name = strdup(sender);
		*out_path = strdup(arg);
	} else {
		*out_bus_name = strdup(arg);
		*out_path = strdup("/StatusNotifierItem");
	}
}

/* ---- property fetching (icon pixmap) -------------------------------
 *
 * IconPixmap is a(iiay): array of (width, height, ARGB32-network-order
 * bytes). We pick the largest entry available. This is a blocking
 * call for simplicity - fine for a one-off fetch on item registration.
 */
int watcher_fetch_item_props(TrayState *st, TrayItem *it) {
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	int ret = sd_bus_call_method(st->bus, it->bus_name, it->object_path,
	                              "org.freedesktop.DBus.Properties", "Get",
	                              &err, &reply, "ss", ITEM_IFACE, "IconPixmap");
	if (ret < 0) {
		fprintf(stderr, "[tray] IconPixmap fetch failed for %s: %s\n",
		        it->service, err.message ? err.message : strerror(-ret));
		sd_bus_error_free(&err);
		return ret;
	}

	/* reply is a variant containing a(iiay) */
	ret = sd_bus_message_enter_container(reply, 'v', "a(iiay)");
	if (ret < 0) goto out;
	ret = sd_bus_message_enter_container(reply, 'a', "(iiay)");
	if (ret < 0) goto out;

	int best_w = 0, best_h = 0;
	unsigned char *best_data = NULL;

	while (sd_bus_message_enter_container(reply, 'r', "iiay") > 0) {
		int32_t w, h;
		sd_bus_message_read(reply, "ii", &w, &h);

		const void *pixels;
		size_t n;
		sd_bus_message_read_array(reply, 'y', &pixels, &n);

		if (w * h > best_w * best_h && n >= (size_t)(w * h * 4)) {
			free(best_data);
			best_data = malloc(n);
			memcpy(best_data, pixels, n);
			best_w = w;
			best_h = h;
		}

		sd_bus_message_exit_container(reply); /* r */
	}
	sd_bus_message_exit_container(reply); /* a */
	sd_bus_message_exit_container(reply); /* v */

	if (best_data) {
		free(it->argb);
		it->argb = best_data;
		it->icon_w = best_w;
		it->icon_h = best_h;
		fprintf(stderr, "[tray] got %dx%d icon for %s\n", best_w, best_h, it->service);
	}

out:
	sd_bus_error_free(&err);
	sd_bus_message_unref(reply);

	if (!it->argb)
		watcher_fetch_icon_by_name(st, it);

	return ret;
}

/* Fallback path: item didn't send raw pixmap bytes (common for
 * lighter-weight apps), so resolve IconName through the freedesktop
 * icon theme instead, same as any other themed app icon. If that
 * also fails, at least improve the text label so something readable
 * always shows instead of a blank box. */
int watcher_fetch_icon_by_name(TrayState *st, TrayItem *it) {
	sd_bus_error err = SD_BUS_ERROR_NULL;
	char *icon_name = NULL;
	char *theme_path = NULL;

	sd_bus_message *reply = NULL;
	if (sd_bus_call_method(st->bus, it->bus_name, it->object_path,
	                        "org.freedesktop.DBus.Properties", "Get", &err, &reply,
	                        "ss", ITEM_IFACE, "IconName") >= 0) {
		const char *s;
		if (sd_bus_message_read(reply, "v", "s", &s) >= 0 && s && *s)
			icon_name = strdup(s);
	}
	sd_bus_message_unref(reply);
	sd_bus_error_free(&err);
	err = (sd_bus_error)SD_BUS_ERROR_NULL;
	reply = NULL;

	/* Title is often more human-readable than IconName ("Discord" vs
	 * "discord-tray") - prefer it for the visible label even if we
	 * still use IconName for the theme file lookup */
	if (sd_bus_call_method(st->bus, it->bus_name, it->object_path,
	                        "org.freedesktop.DBus.Properties", "Get", &err, &reply,
	                        "ss", ITEM_IFACE, "Title") >= 0) {
		const char *s;
		if (sd_bus_message_read(reply, "v", "s", &s) >= 0 && s && *s) {
			free(it->label);
			it->label = strdup(s);
		}
	}
	sd_bus_message_unref(reply);
	sd_bus_error_free(&err);
	err = (sd_bus_error)SD_BUS_ERROR_NULL;
	reply = NULL;

	if (!icon_name) {
		fprintf(stderr, "[tray] %s has no IconPixmap or IconName, showing label '%s'\n",
		        it->service, it->label);
		return -1;
	}

	/* Title wasn't available - IconName is still a better label than
	 * our best-effort default from the bus name/path */
	if (!it->label || strcmp(it->label, it->bus_name) == 0) {
		free(it->label);
		it->label = strdup(icon_name);
	}

	/* apps may point us at a non-standard theme dir bundled with
	 * the app itself */
	if (sd_bus_call_method(st->bus, it->bus_name, it->object_path,
	                        "org.freedesktop.DBus.Properties", "Get", &err, &reply,
	                        "ss", ITEM_IFACE, "IconThemePath") >= 0) {
		const char *s;
		if (sd_bus_message_read(reply, "v", "s", &s) >= 0 && s && *s)
			theme_path = strdup(s);
	}
	sd_bus_message_unref(reply);
	sd_bus_error_free(&err);

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

/* ---- vtable when WE are the watcher --------------------------------- */

static int method_register_item(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
	(void)ret_error;
	TrayState *st = userdata;
	const char *arg;
	int ret = sd_bus_message_read(m, "s", &arg);
	if (ret < 0) return ret;

	const char *sender = sd_bus_message_get_sender(m);
	char *bus_name, *path;
	split_service_arg(sender, arg, &bus_name, &path);

	/* the "service" key we track by is the sender's unique name + path,
	 * which is stable even if the app passed a well-known name */
	char service_key[512];
	snprintf(service_key, sizeof(service_key), "%s%s", bus_name, path);

	item_add(st, bus_name, service_key, path);
	free(bus_name);
	free(path);

	sd_bus_emit_signal(st->bus, WATCHER_OBJ_PATH, WATCHER_IFACE,
	                    "StatusNotifierItemRegistered", "s", arg);

	return sd_bus_reply_method_return(m, "");
}

static int method_register_host(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
	(void)ret_error;
	TrayState *st = userdata;
	(void)st;
	/* we don't need to track other hosts for a minimal implementation */
	return sd_bus_reply_method_return(m, "");
}

static int prop_get_registered_items(sd_bus *bus, const char *path, const char *interface,
                                      const char *property, sd_bus_message *reply,
                                      void *userdata, sd_bus_error *ret_error) {
	(void)bus; (void)path; (void)interface; (void)property; (void)ret_error;
	TrayState *st = userdata;
	int ret = sd_bus_message_open_container(reply, 'a', "s");
	if (ret < 0) return ret;
	for (TrayItem *it = st->items; it; it = it->next)
		sd_bus_message_append(reply, "s", it->service);
	return sd_bus_message_close_container(reply);
}

static int prop_get_host_registered(sd_bus *bus, const char *path, const char *interface,
                                     const char *property, sd_bus_message *reply,
                                     void *userdata, sd_bus_error *ret_error) {
	(void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;
	return sd_bus_message_append(reply, "b", 1);
}

static int prop_get_protocol_version(sd_bus *bus, const char *path, const char *interface,
                                      const char *property, sd_bus_message *reply,
                                      void *userdata, sd_bus_error *ret_error) {
	(void)bus; (void)path; (void)interface; (void)property; (void)userdata; (void)ret_error;
	return sd_bus_message_append(reply, "i", 0);
}

static const sd_bus_vtable watcher_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", method_register_item, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", method_register_host, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", prop_get_registered_items, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
	SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b", prop_get_host_registered, 0, 0),
	SD_BUS_PROPERTY("ProtocolVersion", "i", prop_get_protocol_version, 0, 0),
	SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
	SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
	SD_BUS_SIGNAL("StatusNotifierHostRegistered", "", 0),
	SD_BUS_SIGNAL("StatusNotifierHostUnregistered", "", 0),
	SD_BUS_VTABLE_END
};

/* ---- client mode: another watcher already exists -------------------- */

static int on_item_registered_signal(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
	(void)ret_error;
	TrayState *st = userdata;
	const char *arg;
	if (sd_bus_message_read(m, "s", &arg) < 0)
		return 0;

	const char *sender = sd_bus_message_get_sender(m);
	char *bus_name, *path;
	split_service_arg(sender, arg, &bus_name, &path);
	char service_key[512];
	snprintf(service_key, sizeof(service_key), "%s%s", bus_name, path);
	item_add(st, bus_name, service_key, path);
	free(bus_name);
	free(path);
	return 0;
}

static int on_item_unregistered_signal(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
	(void)ret_error;
	TrayState *st = userdata;
	const char *arg;
	if (sd_bus_message_read(m, "s", &arg) < 0)
		return 0;
	const char *sender = sd_bus_message_get_sender(m);
	char *bus_name, *path;
	split_service_arg(sender, arg, &bus_name, &path);
	char service_key[512];
	snprintf(service_key, sizeof(service_key), "%s%s", bus_name, path);
	TrayItem *it = item_find_by_service(st, service_key);
	if (it) item_remove(st, it);
	free(bus_name);
	free(path);
	return 0;
}

/* fires when an item's owning bus name drops off the bus without a
 * clean Unregister - the usual case when an app just exits */
static int on_name_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
	(void)ret_error;
	TrayState *st = userdata;
	const char *name, *old_owner, *new_owner;
	if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0)
		return 0;
	if (new_owner[0] != '\0')
		return 0; /* still owned, not a disconnect */

	TrayItem *it = st->items;
	while (it) {
		TrayItem *next = it->next;
		if (strcmp(it->bus_name, name) == 0)
			item_remove(st, it);
		it = next;
	}
	return 0;
}

static int become_host_client(TrayState *st) {
	sd_bus_call_method(st->bus, WATCHER_BUS_NAME, WATCHER_OBJ_PATH,
	                    WATCHER_IFACE, "RegisterStatusNotifierHost",
	                    NULL, NULL, "s", "systray-host");

	sd_bus_match_signal(st->bus, NULL, WATCHER_BUS_NAME, WATCHER_OBJ_PATH,
	                     WATCHER_IFACE, "StatusNotifierItemRegistered",
	                     on_item_registered_signal, st);
	sd_bus_match_signal(st->bus, NULL, WATCHER_BUS_NAME, WATCHER_OBJ_PATH,
	                     WATCHER_IFACE, "StatusNotifierItemUnregistered",
	                     on_item_unregistered_signal, st);

	/* pull in whatever is already registered */
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
			/* we don't know the true sender here, so best effort:
			 * treat as bus-name form */
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

	fprintf(stderr, "[tray] existing StatusNotifierWatcher found, running as host client\n");
	return 0;
}

/* ---- init / shutdown ------------------------------------------------- */

int watcher_init(TrayState *st) {
	int ret = sd_bus_open_user(&st->bus);
	if (ret < 0) {
		fprintf(stderr, "[tray] failed to connect to session bus: %s\n", strerror(-ret));
		return ret;
	}

	/* always watch for items disappearing */
	sd_bus_match_signal(st->bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
	                     "org.freedesktop.DBus", "NameOwnerChanged",
	                     on_name_owner_changed, st);
	sd_bus_call_method(st->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
	                    "org.freedesktop.DBus", "AddMatch", NULL, NULL,
	                    "s", "type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'");

	ret = sd_bus_request_name(st->bus, WATCHER_BUS_NAME, 0);
	if (ret >= 0) {
		/* we own it: become the watcher */
		ret = sd_bus_add_object_vtable(st->bus, NULL, WATCHER_OBJ_PATH,
		                                WATCHER_IFACE, watcher_vtable, st);
		if (ret < 0) {
			fprintf(stderr, "[tray] failed to add watcher vtable: %s\n", strerror(-ret));
			return ret;
		}
		st->host_registered = 1;
		fprintf(stderr, "[tray] no existing watcher, running as StatusNotifierWatcher\n");
	} else {
		/* someone else owns it: just be a host */
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
