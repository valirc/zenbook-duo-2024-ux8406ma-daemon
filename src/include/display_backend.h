/*
 * display_backend.h — internal backend contract for display drivers.
 *
 * A display backend is a struct of function pointers that knows how
 * to:
 *   - turn an output (eDP-1, eDP-2, ...) on or off,
 *   - report whether an output is currently on,
 *   - apply a rotation to an output,
 *   - set wallpapers on the active outputs.
 *
 * The dispatcher (display.c) holds a registry of backends and chooses
 * one at startup based on the user's session (XDG_SESSION_TYPE,
 * XDG_CURRENT_DESKTOP) or an explicit override in the configuration
 * (cfg->pantalla_backend, future).
 *
 * Registration is done in each backend's translation unit via a
 * GCC/Clang `__attribute__((constructor))` function that calls
 * display_backend_register(); this keeps the backend list closed
 * over the linker so removing display_xrandr.c / display_gdctl.c
 * from the link cleanly removes that backend at compile time.
 */

#ifndef ZBD_DISPLAY_BACKEND_H
#define ZBD_DISPLAY_BACKEND_H

#include "display.h"

struct display_backend
{
    /* Stable, lower-case identifier used in the config (cfg->
     * pantalla_backend) and in env-var-driven auto-detection. */
    const char *name;

    /* Probe that returns 1 if this backend can run in the current
     * environment, 0 otherwise. The dispatcher consults this only
     * when "auto" is requested. */
    int (*probe)(void);

    /* Mandatory operations. All return 0 on success, non-zero on
     * failure. NULL is treated as "not implemented" and the call
     * fails with -ENOSYS. */
    int (*set_output)(const char *output, display_output_state state,
                      const char *mode, const char *rate);
    int (*is_output_on)(const char *output);
    int (*set_rotation)(const char *output, display_rotation r);
    int (*set_wallpapers)(const char *bg1, const char *bg2);

    /* Primary monitor management.
     * set_primary: persist `output` as the desired primary and rebuild the
     *              full layout immediately.  Passing NULL resets to auto.
     * get_primary: returns the name of the currently effective primary
     *              (never NULL — falls back to "eDP-1" if undetermined).
     * is_output_connected: 1 if a cable is physically present on `output`
     *              (DRM sysfs status == "connected"), 0 otherwise. */
    int         (*set_primary)(const char *output);
    const char *(*get_primary)(void);
    int         (*is_output_connected)(const char *output);
};

/*
 * Register a backend. Called from constructor functions in each
 * backend's translation unit. Backends with the same name are
 * rejected (the second registration is a no-op and emits a warning).
 */
void display_backend_register(const struct display_backend *backend);

#endif /* ZBD_DISPLAY_BACKEND_H */
