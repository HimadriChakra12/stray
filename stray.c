#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef STRAY_VERSION
#define STRAY_VERSION "dev"
#endif

#define STB_DS_IMPLEMENTATION
#include "vendor/stb_ds.h"

#define SYSTEM_TRAY_REQUEST_DOCK 0
#define XEMBED_EMBEDDED_NOTIFY   0

#include "config.h"

static Display      *dpy;
static int           screen, haverandr;
static int           mx, my, mw, mh;
static Window        root, barwin, selwin;
static GC            gc;
static unsigned long bgpx, borderpx;
static Atom          xa_xembed, xa_manager, xa_trayatom, xa_opcode, xa_orient;
static Window       *icons;
static int           barw;
static volatile sig_atomic_t togglereq;

static void die(const char *msg) { fputs(msg, stderr); exit(1); }

static void envuint(const char *name, unsigned int *dst)
{
    const char *s = getenv(name);
    char *end; long v;
    if (!s || !*s) return;
    v = strtol(s, &end, 10);
    if (!*end && v >= 0) *dst = (unsigned int)v;
}

static void envint(const char *name, int *dst)
{
    const char *s = getenv(name);
    char *end; long v;
    if (!s || !*s) return;
    v = strtol(s, &end, 10);
    if (!*end) *dst = (int)v;
}

static void envstr(const char *name, const char **dst)
{
    const char *s = getenv(name);
    if (s && *s) *dst = s;
}

static void loadconfig(void)
{
    envint ("STRAY_BOTTOM",    &bottom);
    envstr ("STRAY_ALIGN",     &align);
    envuint("STRAY_HEIGHT",    &height);
    envuint("STRAY_ICONSIZE",  &icon_size);
    envuint("STRAY_PADDINGH",  &padding_h);
    envuint("STRAY_MARGINV",   &margin_v);
    envuint("STRAY_MARGINH",   &margin_h);
    envuint("STRAY_ICONPAD",   &icon_pad);
    envint ("STRAY_BORDER",    &border);
    envuint("STRAY_BORDERW",   &border_w);
    envstr ("STRAY_BG",        &bg_color);
    envstr ("STRAY_FG",        &fg_color);
}

static int overridden(Window w)
{
    XWindowAttributes wa;
    return XGetWindowAttributes(dpy, w, &wa) && wa.override_redirect;
}

static int xerror(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

static void sighandler(int sig) { (void)sig; togglereq = 1; }

static void updatemon(void)
{
    mx = 0; my = 0;
    mw = DisplayWidth(dpy, screen);
    mh = DisplayHeight(dpy, screen);
    if (!haverandr) return;

    int n; XRRMonitorInfo *info = XRRGetMonitors(dpy, root, True, &n);
    if (!info) return;
    int i = (n > 1) ? 1 : 0;
    mx = info[i].x; my = info[i].y;
    mw = info[i].width; mh = info[i].height;
    XRRFreeMonitors(info);
}

static void layout(void)
{
    int n  = (int)arrlen(icons);
    int bw = border ? (int)border_w : 0;
    updatemon();

    barw = (int)(2 * padding_h)
           + n * (int)icon_size
           + (n > 1 ? (n - 1) * (int)icon_pad : 0);
    if (barw < (int)height) barw = (int)height;

    int bx;
    if (strcmp(align, "left") == 0)
        bx = mx + (int)margin_h;
    else if (strcmp(align, "center") == 0)
        bx = mx + (mw - barw - 2 * bw) / 2;
    else
        bx = mx + mw - barw - 2 * bw - (int)margin_h;

    int by = bottom
        ? my + mh - (int)height - 2 * bw - (int)margin_v
        : my + (int)margin_v;

    XMoveResizeWindow(dpy, barwin, bx, by, (unsigned)barw, height);

    int x = (int)padding_h;
    int y = ((int)height - (int)icon_size) / 2;
    for (int i = 0; i < n; i++) {
        XMoveResizeWindow(dpy, icons[i], x, y, icon_size, icon_size);
        x += (int)(icon_size + icon_pad);
    }
}

static void dock(Window w)
{
    if (w == None) return;

    XAddToSaveSet(dpy, w);
    XSelectInput(dpy, w, StructureNotifyMask);
    XReparentWindow(dpy, w, barwin, 0, 0);

    XEvent ev = {0};
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = w;
    ev.xclient.message_type = xa_xembed;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = CurrentTime;
    ev.xclient.data.l[1]    = XEMBED_EMBEDDED_NOTIFY;
    ev.xclient.data.l[3]    = (long)barwin;
    XSendEvent(dpy, w, False, NoEventMask, &ev);

    XMapWindow(dpy, w);
    arrput(icons, w);
    layout();
    XClearWindow(dpy, barwin);
}

static int iconindex(Window w)
{
    for (int i = 0; i < (int)arrlen(icons); i++)
        if (icons[i] == w) return i;
    return -1;
}

static void undock(Window w)
{
    int i = iconindex(w);
    if (i < 0) return;
    arrdel(icons, i);
    layout();
    XClearWindow(dpy, barwin);
}

static void handle(XEvent *ev)
{
    switch (ev->type) {
    case ClientMessage:
        if (ev->xclient.message_type == xa_opcode &&
            ev->xclient.data.l[1] == SYSTEM_TRAY_REQUEST_DOCK)
            dock((Window)ev->xclient.data.l[2]);
        break;

    case Expose:
        if (ev->xexpose.window == barwin && ev->xexpose.count == 0)
            XClearWindow(dpy, barwin);
        break;

    case DestroyNotify:
        undock(ev->xdestroywindow.window);
        break;

    case ReparentNotify:
        if (ev->xreparent.parent != barwin)
            undock(ev->xreparent.window);
        break;

    case ConfigureNotify:
        if (iconindex(ev->xconfigure.window) >= 0 &&
            ((unsigned)ev->xconfigure.width  != icon_size ||
             (unsigned)ev->xconfigure.height != icon_size))
            layout();
        else if (ev->xconfigure.event == root &&
                 ev->xconfigure.window != barwin &&
                 !overridden(ev->xconfigure.window))
            XRaiseWindow(dpy, barwin);
        break;

    case MapNotify:
        if (ev->xmap.event == root &&
            ev->xmap.window != barwin &&
            !ev->xmap.override_redirect)
            XRaiseWindow(dpy, barwin);
        break;

    case SelectionClear:
        if (ev->xselectionclear.window == selwin)
            die("stray: lost tray selection, exiting\n");
        break;
    }
}

static unsigned long alloccolor(const char *name)
{
    XColor c, dummy;
    if (!XAllocNamedColor(dpy, DefaultColormap(dpy, screen), name, &c, &dummy))
        die("stray: cannot allocate colour\n");
    return c.pixel;
}

static void setup(void)
{
    loadconfig();

    if (!(dpy = XOpenDisplay(NULL)))
        die("stray: cannot open display\n");
    XSetErrorHandler(xerror);
    screen = DefaultScreen(dpy);
    root   = RootWindow(dpy, screen);
    int di; haverandr = XRRQueryExtension(dpy, &di, &di);

    bgpx     = alloccolor(bg_color);
    borderpx = alloccolor(fg_color);

    xa_xembed  = XInternAtom(dpy, "_XEMBED",                      False);
    xa_manager = XInternAtom(dpy, "MANAGER",                      False);
    xa_opcode  = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE",      False);
    xa_orient  = XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);

    char selname[32];
    snprintf(selname, sizeof selname, "_NET_SYSTEM_TRAY_S%d", screen);
    xa_trayatom = XInternAtom(dpy, selname, False);

    if (XGetSelectionOwner(dpy, xa_trayatom) != None)
        die("stray: another system tray is already running\n");

    long orient = 0;
    selwin = XCreateSimpleWindow(dpy, root, -1, -1, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, selwin, xa_orient, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&orient, 1);
    XSetSelectionOwner(dpy, xa_trayatom, selwin, CurrentTime);
    if (XGetSelectionOwner(dpy, xa_trayatom) != selwin)
        die("stray: unable to acquire tray selection\n");

    XSetWindowAttributes swa = {0};
    swa.override_redirect = True;
    swa.background_pixel  = bgpx;
    swa.border_pixel      = borderpx;
    swa.event_mask        = ExposureMask | SubstructureNotifyMask;

    unsigned long mask = CWOverrideRedirect | CWBackPixel | CWEventMask;
    int bw = border ? (int)border_w : 0;
    if (border) mask |= CWBorderPixel;

    barwin = XCreateWindow(dpy, root, 0, 0, 1, 1, bw,
                           CopyFromParent, CopyFromParent, CopyFromParent,
                           mask, &swa);

    XClassHint ch; ch.res_name = ch.res_class = "stray";
    XSetClassHint(dpy, barwin, &ch);

    gc = XCreateGC(dpy, barwin, 0, NULL);
    XSetForeground(dpy, gc, borderpx);

    XEvent ev = {0};
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = root;
    ev.xclient.message_type = xa_manager;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = CurrentTime;
    ev.xclient.data.l[1]    = (long)xa_trayatom;
    ev.xclient.data.l[2]    = (long)selwin;
    XSendEvent(dpy, root, False, StructureNotifyMask, &ev);

    XSelectInput(dpy, root, SubstructureNotifyMask);

    layout();
    XMapRaised(dpy, barwin);

    struct sigaction sa = {0};
    sa.sa_handler = sighandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    XSync(dpy, False);
}

static void run(void)
{
    XEvent ev;
    for (;;) {
        if (togglereq) {
            togglereq = 0;
            XRaiseWindow(dpy, barwin);
            XSync(dpy, False);
        }
        XNextEvent(dpy, &ev);
        handle(&ev);
    }
}

int main(int argc, char *argv[])
{
    if (argc == 2 && !strcmp(argv[1], "-v")) {
        printf("stray %s\n", STRAY_VERSION);
        return 0;
    }
    if (argc > 1) die("usage: stray [-v]\n");
    setup();
    run();
    return 0;
}
