/*
 * display.h — public API for display, wallpaper and rotation control.
 *
 * The daemon has historically driven xrandr from inline shell
 * invocations in pantalla.c. That works on X11 but is silently
 * useless on Wayland / GNOME, which is the default on Ubuntu 26.04.
 *
 * This module hides the compositor behind a small swappable backend
 * (xrandr | gdctl | mutter_dbus) so callers only deal with semantic
 * operations: "turn eDP-2 on at 2880x1800@120, then set the
 * wallpapers". The dispatcher picks the right backend at startup.
 *
 * Selection precedence:
 *   1. The configuration key cfg->pantalla_backend if set to
 *      something other than "auto".
 *   2. XDG_SESSION_TYPE = "wayland" + XDG_CURRENT_DESKTOP containing
 *      "GNOME" => gdctl.
 *   3. XDG_SESSION_TYPE = "x11"  => xrandr.
 *   4. Fall back to xrandr (and warn).
 */

#ifndef ZBD_DISPLAY_H
#define ZBD_DISPLAY_H

typedef enum {
    DISPLAY_OUTPUT_OFF = 0,
    DISPLAY_OUTPUT_ON  = 1,
} display_output_state;

typedef enum {
    DISPLAY_ROTATION_NORMAL    = 0,
    DISPLAY_ROTATION_LEFT_UP   = 1,
    DISPLAY_ROTATION_RIGHT_UP  = 2,
    DISPLAY_ROTATION_BOTTOM_UP = 3,
} display_rotation;

/*
 * Initialize the display subsystem: walk the registered backends,
 * pick one based on the configuration / environment, and remember it
 * for the rest of the process. Returns 0 on success, -1 if no
 * backend can be loaded.
 *
 * MUST be called once from each binary's main() before any of the
 * display_* operations below.
 */
int display_init(void);

/*
 * Name of the active backend. Useful for diagnostics and tests.
 * Returns NULL before display_init() succeeds.
 */
const char *display_active_backend(void);

/*
 * Turn an output on (with the given mode + refresh rate, in xrandr
 * notation, e.g. "2880x1800" / "120") or off. `mode` and `rate` are
 * ignored when state == DISPLAY_OUTPUT_OFF.
 */
int display_set_output(const char *output, display_output_state state,
                       const char *mode, const char *rate);

/*
 * Return 1 if the output is currently enabled, 0 otherwise.
 * Errors are logged and reported as 0.
 */
int display_is_output_on(const char *output);

/*
 * Set the rotation of an output. Currently used only by the
 * orientation monitor.
 */
int display_set_rotation(const char *output, display_rotation r);

/*
 * Apply wallpapers. If bg2 is NULL, only one wallpaper is set
 * (single-display mode). Backends are free to ignore bg2 if the
 * compositor cannot address per-output wallpapers.
 */
int display_set_wallpapers(const char *bg1, const char *bg2);

#endif /* ZBD_DISPLAY_H */
