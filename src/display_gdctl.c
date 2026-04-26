/*
 * display_gdctl.c — Wayland / GNOME backend for the display API.
 *
 * Drives the compositor via the gdctl(1) CLI shipped with
 * gnome-control-center >= 47 (default on Ubuntu 26.04). gdctl is the
 * supported way to query and reconfigure displays under Mutter
 * Wayland; xrandr can read the layout (because XWayland exposes it)
 * but cannot change it.
 *
 * For wallpapers we still use feh(1): GNOME's gsettings background
 * key only addresses the primary monitor, while feh on XWayland
 * reaches both displays of the Zenbook Duo correctly even from a
 * Wayland session. Replacing this with gsettings + a per-monitor
 * extension is future work.
 *
 * For is_output_on() we do NOT shell out to `gdctl show`; we still
 * read the DRM sysfs file (/sys/class/drm/card1-<output>/enabled),
 * which the kernel keeps accurate regardless of the compositor.
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

static const char *rotation_to_gdctl(display_rotation r)
{
    switch (r) {
    case DISPLAY_ROTATION_NORMAL:    return "normal";
    case DISPLAY_ROTATION_LEFT_UP:   return "left";
    case DISPLAY_ROTATION_RIGHT_UP:  return "right";
    case DISPLAY_ROTATION_BOTTOM_UP: return "upside-down";
    default:                         return "normal";
    }
}

/* ---- backend operations ------------------------------------------ */

static int gdctl_probe(void)
{
    return access("/usr/bin/gdctl", X_OK) == 0;
}

/*
 * Build a `gdctl set` invocation that materializes the entire
 * desired layout in one atomic call. gdctl needs the *complete*
 * topology each time; partial mutations (just turning eDP-2 off)
 * are not supported.
 *
 * Topology rules implemented here are tailored to the UX8406MA:
 *   - eDP-1 is always on, primary, at the configured mode.
 *   - eDP-2, when on, is placed BELOW eDP-1 (logical-monitor below
 *     in gdctl terms), at the same mode.
 *
 * The caller is expected to pass `mode` (e.g. "2880x1800") and
 * `rate` (e.g. "120") for the relevant output.
 */
static int gdctl_apply_layout(const char *eDP1_mode, const char *eDP1_rate,
                              int eDP2_on, const char *eDP2_mode, const char *eDP2_rate)
{
    /* Construct argv depending on whether eDP-2 is on or off. */
    if (!eDP1_mode || !eDP1_rate) { errno = EINVAL; return -1; }

    char eDP1_spec[64];
    snprintf(eDP1_spec, sizeof(eDP1_spec), "%s@%s", eDP1_mode, eDP1_rate);

    if (!eDP2_on)
    {
        char *const args[] = {
            "gdctl", "set",
            "--logical-monitor", "--primary", "--monitor", "eDP-1", "--mode", eDP1_spec,
            NULL
        };
        return exec_cmd_argv("gdctl", args);
    }

    if (!eDP2_mode || !eDP2_rate) { errno = EINVAL; return -1; }
    char eDP2_spec[64];
    snprintf(eDP2_spec, sizeof(eDP2_spec), "%s@%s", eDP2_mode, eDP2_rate);

    char *const args[] = {
        "gdctl", "set",
        "--logical-monitor", "--primary", "--monitor", "eDP-1", "--mode", eDP1_spec,
        "--logical-monitor", "--monitor", "eDP-2", "--mode", eDP2_spec,
        "--down", "eDP-1",
        NULL
    };
    return exec_cmd_argv("gdctl", args);
}

/*
 * Default eDP-1 mode/rate to use when the caller wants to switch
 * eDP-2 alone. We keep the eDP-1 part of the topology stable using
 * the Zenbook Duo native mode 2880x1800@120, which matches the
 * configuration shipped with this daemon.
 */
#define EDP1_DEFAULT_MODE "2880x1800"
#define EDP1_DEFAULT_RATE "120"

static int gdctl_set_output(const char *output, display_output_state state,
                            const char *mode, const char *rate)
{
    if (!output) { errno = EINVAL; return -1; }

    if (!strcmp(output, "eDP-2"))
    {
        if (state == DISPLAY_OUTPUT_ON)
        {
            return gdctl_apply_layout(EDP1_DEFAULT_MODE, EDP1_DEFAULT_RATE,
                                      1, mode ? mode : EDP1_DEFAULT_MODE,
                                      rate ? rate : EDP1_DEFAULT_RATE);
        }
        return gdctl_apply_layout(EDP1_DEFAULT_MODE, EDP1_DEFAULT_RATE,
                                  0, NULL, NULL);
    }

    /* For any other output (eDP-1 or external) we expect mode + rate
     * to be supplied and we leave eDP-2 as it currently is. */
    if (!mode || !rate) { errno = EINVAL; return -1; }
    char spec[64];
    snprintf(spec, sizeof(spec), "%s@%s", mode, rate);
    if (state == DISPLAY_OUTPUT_OFF)
    {
        char *const args[] = {
            "gdctl", "set",
            "--logical-monitor", "--monitor", (char *)output, "--off",
            NULL
        };
        return exec_cmd_argv("gdctl", args);
    }
    char *const args[] = {
        "gdctl", "set",
        "--logical-monitor", "--monitor", (char *)output, "--mode", spec,
        NULL
    };
    return exec_cmd_argv("gdctl", args);
}

static int gdctl_is_output_on(const char *output)
{
    if (!output) return 0;
    char path[512];
    snprintf(path, sizeof(path), "/sys/class/drm/card1-%s/enabled", output);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char buf[16] = {0};
    int on = 0;
    if (fgets(buf, sizeof(buf), fp))
        on = !strcmp(buf, "enabled\n");
    fclose(fp);
    return on;
}

static int gdctl_set_rotation(const char *output, display_rotation r)
{
    if (!output) { errno = EINVAL; return -1; }
    char *const args[] = {
        "gdctl", "set",
        "--logical-monitor", "--monitor", (char *)output,
        "--rotation", (char *)rotation_to_gdctl(r),
        NULL
    };
    return exec_cmd_argv("gdctl", args);
}

static int gdctl_set_wallpapers(const char *bg1, const char *bg2)
{
    /* feh works through XWayland on GNOME Wayland and addresses both
     * Zenbook Duo panels correctly. Until we wire per-output
     * wallpapers via gsettings + an extension, reuse it. */
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

static const struct display_backend gdctl_backend = {
    .name           = "gdctl",
    .probe          = gdctl_probe,
    .set_output     = gdctl_set_output,
    .is_output_on   = gdctl_is_output_on,
    .set_rotation   = gdctl_set_rotation,
    .set_wallpapers = gdctl_set_wallpapers,
};

__attribute__((constructor))
static void register_gdctl_backend(void)
{
    display_backend_register(&gdctl_backend);
}
