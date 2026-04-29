/*
 * display_xrandr.c — X11 backend for the display API.
 *
 * Drives the compositor through the xrandr(1) CLI. Only selected when
 * XDG_SESSION_TYPE=x11; on Wayland/GNOME the gdctl backend is chosen
 * instead (Mutter silently ignores xrandr reconfiguration from XWayland).
 *
 * is_output_on() reads /sys/class/drm/<card>-<output>/enabled directly
 * rather than parsing xrandr output. The DRM card name is auto-detected
 * via display_get_drm_card() — no hardcoded card index.
 *
 * Wallpapers are set via gsettings (org.gnome.desktop.background), the
 * same mechanism used by the gdctl backend.  This works for GNOME on
 * X11 and removes the feh/XWayland dependency entirely.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "display.h"
#include "display_backend.h"
#include "exec.h"

/* ---- helpers ----------------------------------------------------- */

static const char *rotation_to_xrandr(display_rotation r)
{
    switch (r) {
    case DISPLAY_ROTATION_NORMAL:    return "normal";
    case DISPLAY_ROTATION_LEFT_UP:   return "left";
    case DISPLAY_ROTATION_RIGHT_UP:  return "right";
    case DISPLAY_ROTATION_BOTTOM_UP: return "inverted";
    default:                         return "normal";
    }
}

/* ---- backend operations ------------------------------------------ */

static int xrandr_probe(void)
{
    return access("/usr/bin/xrandr", X_OK) == 0;
}

static int xrandr_set_output(const char *output, display_output_state state,
                             const char *mode, const char *rate)
{
    if (!output) { errno = EINVAL; return -1; }

    if (state == DISPLAY_OUTPUT_OFF)
    {
        char *const args[] = { "xrandr", "--output", (char *)output, "--off", NULL };
        return exec_cmd_argv("xrandr", args);
    }

    if (!mode || !rate) { errno = EINVAL; return -1; }

    /* Two-step: --auto first to pick up a sensible mode list,
     * then --mode/--rate to lock in the requested combination. */
    {
        char *const args_auto[] = {
            "xrandr", "--output", (char *)output, "--auto", NULL
        };
        if (exec_cmd_argv("xrandr", args_auto) != 0) return -1;
    }
    {
        char *const args_mode[] = {
            "xrandr",
            "--output", (char *)output,
            "--mode",   (char *)mode,
            "--rate",   (char *)rate,
            NULL
        };
        return exec_cmd_argv("xrandr", args_mode);
    }
}

static int xrandr_is_output_on(const char *output)
{
    if (!output) return 0;

    char path[512];
    snprintf(path, sizeof(path), "/sys/class/drm/%s-%s/enabled",
             display_get_drm_card(), output);

    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    char buf[16] = {0};
    int on = 0;
    if (fgets(buf, sizeof(buf), fp))
        on = !strcmp(buf, "enabled\n");
    fclose(fp);
    return on;
}

static int xrandr_set_rotation(const char *output, display_rotation r)
{
    if (!output) { errno = EINVAL; return -1; }
    char *const args[] = {
        "xrandr",
        "--output", (char *)output,
        "--rotate", (char *)rotation_to_xrandr(r),
        NULL
    };
    return exec_cmd_argv("xrandr", args);
}

static int xrandr_set_wallpapers(const char *bg1, const char *bg2)
{
    /* Set via gsettings — works for GNOME on X11, no XWayland or feh needed.
     * bg2 is ignored: GNOME has no native per-output wallpaper API. */
    if (!bg1) { errno = EINVAL; return -1; }
    (void)bg2;

    if (bg1[0] != '/') { errno = EINVAL; return -1; }
    char uri[4096];
    int n = snprintf(uri, sizeof(uri), "file://%s", bg1);
    if (n <= 0 || (size_t)n >= sizeof(uri)) { errno = ENAMETOOLONG; return -1; }

    char *const args_light[] = {
        "gsettings", "set",
        "org.gnome.desktop.background", "picture-uri",
        uri, NULL
    };
    int r = exec_cmd_argv("gsettings", args_light);
    if (r != 0) return r;

    char *const args_dark[] = {
        "gsettings", "set",
        "org.gnome.desktop.background", "picture-uri-dark",
        uri, NULL
    };
    return exec_cmd_argv("gsettings", args_dark);
}

/* ---- registration ------------------------------------------------ */

static const struct display_backend xrandr_backend = {
    .name           = "xrandr",
    .probe          = xrandr_probe,
    .set_output     = xrandr_set_output,
    .is_output_on   = xrandr_is_output_on,
    .set_rotation   = xrandr_set_rotation,
    .set_wallpapers = xrandr_set_wallpapers,
};

__attribute__((constructor))
static void register_xrandr_backend(void)
{
    display_backend_register(&xrandr_backend);
}
