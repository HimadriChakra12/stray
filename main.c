#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <X11/Xlib.h>
#include "tray.h"

static void activate(TrayState *st, TrayItem *it, int x, int y, int ctx) {
	const char *method = ctx ? "ContextMenu" : "Activate";
	sd_bus_error err = SD_BUS_ERROR_NULL;
	int ret = sd_bus_call_method(st->bus, it->bus_name, it->object_path,
	                              "org.kde.StatusNotifierItem", method, &err, NULL, "ii", x, y);
	if (ret < 0)
		fprintf(stderr, "[tray] %s failed for %s: %s\n", method, it->service,
		        err.message ? err.message : strerror(-ret));
	sd_bus_error_free(&err);
}

int main(void) {
	TrayState st = {0};

	if (watcher_init(&st) < 0)
		return 1;
	if (window_init(&st) < 0) {
		watcher_shutdown(&st);
		return 1;
	}
	xembed_init(&st); /* fine if another tray already owns the selection */

	int bus_fd = sd_bus_get_fd(st.bus);
	int x11_fd = ConnectionNumber(st.dpy);
	fprintf(stderr, "[tray] running\n");

	for (;;) {
		while (XPending(st.dpy)) {
			XEvent ev;
			XNextEvent(st.dpy, &ev);
			switch (ev.type) {
			case Expose:
				window_redraw(&st);
				break;
			case ClientMessage:
				xembed_handle_client_message(&st, &ev.xclient);
				break;
			case DestroyNotify:
			case UnmapNotify:
			case ReparentNotify:
				xembed_handle_structure_notify(&st, &ev);
				break;
			case ButtonPress: {
				XButtonEvent *be = &ev.xbutton;
				if (window_is_toggle(&st, be->x, be->y)) {
					st.collapsed = !st.collapsed;
					item_relayout(&st);
					break;
				}
				TrayItem *it = window_item_at(&st, be->x, be->y);
				if (it)
					activate(&st, it, be->x_root, be->y_root, be->button == 3);
				break;
			}
			}
		}

		while (sd_bus_process(st.bus, NULL) > 0)
			;

		struct pollfd fds[2] = {
			{ .fd = bus_fd, .events = POLLIN },
			{ .fd = x11_fd, .events = POLLIN },
		};
		if (poll(fds, 2, 1000) < 0)
			break;
		XRaiseWindow(st.dpy, st.win);
		XFlush(st.dpy);
	}

	window_shutdown(&st);
	watcher_shutdown(&st);
	return 0;
}
