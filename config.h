/* systray config.h - edit and recompile */

/* icon box size in px (icons are scaled/cropped to this) */
static const int icon_size   = 20;
/* gap between icons */
static const int icon_gap    = 4;
/* padding around the whole strip */
static const int pad_x       = 4;
static const int pad_y       = 2;
/* tray window height (usually icon_size + 2*pad_y) */
static const int tray_h      = 24;

/* screen position: "top-right", "top-left", "bottom-right", "bottom-left" */
static const char *tray_gravity = "top-right";

/* offset from the chosen corner, e.g. leave room for a clock widget */
static const int offset_x = 0;
static const int offset_y = 0;

/* background color 0xRRGGBB (used if not compositing) */
static const unsigned long bg_color = 0x1a1b26;

/* set to 1 to request an ARGB visual (transparent bg) if a compositor
 * is running; falls back to bg_color if not available */
static const int want_transparency = 1;
