/*
 * display_xrandr.c — X11 backend for the display API.
 *
 * Drives the compositor through the xrandr(1) CLI. Works on any X11
 * session (the historical environment for this daemon) and, in
 * principle, also on Wayland with XWayland — but Mutter ignores
 * xrandr reconfiguration on a real Wayland session, so the
 * autodetector in display.c only picks this backend for X11.
 *
 * is_output_on() reads /sys/class/drm/card1-<output>/enabled directly
 * instead of parsing xrandr output. The DRM card index is hard-coded
 * to card1 because that is what the UX8406MA exposes; a future commit
 * may make this configurable.
 *
 * Wallpapers are set via feh(1).
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
    /* Accept whenever the binary exists. The autodetector in
     * display.c handles the X11-vs-Wayland distinction. */
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
    snprintf(path, sizeof(path), "/sys/class/drm/card1-%s/enabled", output);

    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    char buf[16] = {0};
    int on = 0;
    if (fgets(buf, sizeof(buf), fp))
    {
        on = !strcmp(buf, "enabled\n");
    }
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
    if (!bg1) { errno = EINVAL; return -1; }
    if (bg2)
    {
        char *const args[] = {
            "feh", "--bg-scale", (char *)bg1, "--bg-scale", (char *)bg2, NULL
        };
        return exec_cmd_argv("feh", args);
    }
    char *const args[] = {
        "feh", "--bg-scale", (char *)bg1, NULL
    };
    return exec_cmd_argv("feh", args);
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
