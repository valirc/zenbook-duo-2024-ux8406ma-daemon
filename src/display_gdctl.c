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
 *
 * Rotation state
 * ──────────────
 * gdctl requires the *complete* display topology in every `gdctl set`
 * call — partial mutations are not supported. The rotation applied by
 * the accelerometer monitor must therefore be threaded through every
 * topology rebuild (keyboard attach/detach, output on/off). We keep
 * g_edp_rotation as module-level state so that gdctl_apply_layout()
 * always emits the correct --transform for both eDP panels without any
 * caller needing to pass it explicitly.
 *
 * Both eDP panels rotate together: the Zenbook Duo has two stacked
 * displays that form a single physical unit, so they share one
 * rotation value.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "comun.h"
#include "display.h"
#include "display_backend.h"
#include "exec.h"

/* ---- module-level state ------------------------------------------ */

/* Current rotation applied to the eDP panels. */
static display_rotation g_edp_rotation = DISPLAY_ROTATION_NORMAL;

/* Desired primary output.  Empty string = automatic: HDMI-1 when a cable
 * is physically present, eDP-1 otherwise.  Set by gdctl_set_primary(). */
static char g_primary_output[64] = "";

/* ---- DRM card detection ------------------------------------------ */

/*
 * Return the DRM card name (e.g. "card0") that owns the eDP-1 connector.
 * Result is cached on first call. Falls back to "card0" if detection
 * fails — correct for xe driver (Meteor Lake Arc, UX8406MA).
 */
static const char *get_drm_card(void)
{
    static char card[16] = {0};
    if (card[0]) return card;

    DIR *d = opendir("/sys/class/drm");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, "card", 4) != 0) continue;
            const char *dash = strchr(e->d_name + 4, '-');
            if (!dash) continue;
            if (strcmp(dash + 1, "eDP-1") == 0) {
                size_t len = (size_t)(dash - e->d_name);
                if (len < sizeof(card)) {
                    memcpy(card, e->d_name, len);
                    card[len] = '\0';
                }
                break;
            }
        }
        closedir(d);
    }
    if (!card[0])
        strncpy(card, "card0", sizeof(card) - 1);
    return card;
}

/* ---- helpers ----------------------------------------------------- */

static const char *rotation_to_gdctl(display_rotation r)
{
    switch (r) {
    case DISPLAY_ROTATION_NORMAL:    return "normal";
    case DISPLAY_ROTATION_LEFT_UP:   return "90";
    case DISPLAY_ROTATION_RIGHT_UP:  return "270";
    case DISPLAY_ROTATION_BOTTOM_UP: return "180";
    default:                         return "normal";
    }
}

/*
 * Map a gdctl connector name to its DRM sysfs counterpart.
 * gdctl uses "HDMI-N" but the kernel exposes "HDMI-A-N" in
 * /sys/class/drm/. eDP and DP connectors pass through unchanged.
 */
static const char *gdctl_name_to_drm(const char *name)
{
    if (!name) return NULL;
    if (!strcmp(name, "HDMI-1")) return "HDMI-A-1";
    if (!strcmp(name, "HDMI-2")) return "HDMI-A-2";
    return name;
}

/*
 * Returns 1 if the DRM connector `drm_name` (e.g. "HDMI-A-1", "DP-1")
 * has a cable physically plugged in — i.e. sysfs status == "connected".
 */
static int drm_connector_connected(const char *drm_name)
{
    if (!drm_name) return 0;
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/drm/%s-%s/status", get_drm_card(), drm_name);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char buf[16] = {0};
    int connected = 0;
    if (fgets(buf, sizeof(buf), fp))
        connected = !strcmp(buf, "connected\n");
    fclose(fp);
    return connected;
}

/* Mapping table: gdctl connector name → DRM sysfs name.
 * Covers HDMI, DisplayPort-over-Thunderbolt, and USB-C DP-alt. */
static const struct { const char *gdctl; const char *drm; } g_ext_candidates[] = {
    { "HDMI-1", "HDMI-A-1" },
    { "HDMI-2", "HDMI-A-2" },
    { "DP-1",   "DP-1"     },
    { "DP-2",   "DP-2"     },
    { "DP-3",   "DP-3"     },
    { NULL,     NULL       }
};

/*
 * Returns the gdctl name of the first physically-connected external output
 * (HDMI, Thunderbolt, USB-C DP-alt), or NULL when nothing is plugged in.
 * eDP-1 and eDP-2 are internal panels and are not considered external.
 */
static const char *detect_external_output(void)
{
    for (int i = 0; g_ext_candidates[i].gdctl; i++) {
        if (drm_connector_connected(g_ext_candidates[i].drm))
            return g_ext_candidates[i].gdctl;
    }
    return NULL;
}

/* ---- backend operations ------------------------------------------ */

static int gdctl_probe(void)
{
    return access("/usr/bin/gdctl", X_OK) == 0;
}

/*
 * Build and execute a `gdctl set` call that materialises the entire
 * desired layout in one atomic operation. gdctl requires the complete
 * topology on every call; partial mutations are not supported.
 *
 * Topology rules for the UX8406MA:
 *   eDP-1  — always on, primary, at the configured mode.
 *   eDP-2  — when on, placed BELOW eDP-1 at the same mode.
 *   HDMI-1 — included whenever a cable is physically present
 *             (HDMI-A-1 DRM connector), placed ABOVE eDP-1.
 *             This prevents keyboard attach/detach from stomping an
 *             active external monitor.
 *
 * Both eDP panels receive --transform from g_edp_rotation so that
 * layout rebuilds triggered by keyboard events preserve any rotation
 * previously set by the accelerometer monitor.
 *
 * Max argv slots used (scale+rotation+eDP2+HDMI): 2+10+11+5+1 = 29  (array sized at 32).
 */
static int gdctl_apply_layout(const char *eDP1_mode, const char *eDP1_rate,
                              int eDP2_on, const char *eDP2_mode, const char *eDP2_rate)
{
    if (!eDP1_mode || !eDP1_rate) { errno = EINVAL; return -1; }

    /* gdctl requires decimal rate notation ("120.000").
     * atol() is locale-independent; %.3f would produce "120,000" in
     * Spanish locales and be rejected by gdctl. */
    char eDP1_spec[64];
    snprintf(eDP1_spec, sizeof(eDP1_spec), "%s@%ld.000", eDP1_mode, atol(eDP1_rate));

    char eDP2_spec[64];
    if (eDP2_on) {
        if (!eDP2_mode || !eDP2_rate) { errno = EINVAL; return -1; }
        snprintf(eDP2_spec, sizeof(eDP2_spec), "%s@%ld.000", eDP2_mode, atol(eDP2_rate));
    }

    /* Scale factor from config — use string directly to avoid float↔locale issues. */
    const char *scale_str = (cfg && cfg->pantalla_escala && cfg->pantalla_escala[0])
                            ? cfg->pantalla_escala : "1.2";

    /* Detect any physically-connected external output (HDMI, TB, USB-C DP). */
    const char *ext_output = detect_external_output();
    int ext_on = (ext_output != NULL);

    int rotated  = (g_edp_rotation != DISPLAY_ROTATION_NORMAL);
    const char *transform = rotation_to_gdctl(g_edp_rotation);

    /* Determine effective primary.
     * Manual (g_primary_output set): honour unless that output is gone,
     * then fall back to auto.
     * Auto: first connected external output; eDP-1 if nothing external. */
    const char *primary;
    if (g_primary_output[0]) {
        /* Check physical presence of the manually-requested primary. */
        int req_connected = !strcmp(g_primary_output, "eDP-1") ? 1
                          : (ext_output && !strcmp(g_primary_output, ext_output));
        primary = req_connected ? g_primary_output
                                : (ext_on ? ext_output : "eDP-1");
    } else {
        primary = ext_on ? ext_output : "eDP-1";
    }

    char *args[32];
    int n = 0;

    args[n++] = "gdctl";
    args[n++] = "set";

    /* eDP-1 */
    args[n++] = "--logical-monitor";
    if (!strcmp(primary, "eDP-1")) args[n++] = "--primary";
    args[n++] = "--monitor"; args[n++] = "eDP-1";
    args[n++] = "--mode";    args[n++] = eDP1_spec;
    args[n++] = "--scale";   args[n++] = (char *)scale_str;
    if (rotated) {
        args[n++] = "--transform"; args[n++] = (char *)transform;
    }

    /* eDP-2: below eDP-1 when on */
    if (eDP2_on) {
        args[n++] = "--logical-monitor";
        args[n++] = "--monitor"; args[n++] = "eDP-2";
        args[n++] = "--mode";    args[n++] = eDP2_spec;
        args[n++] = "--below";   args[n++] = "eDP-1";
        args[n++] = "--scale";   args[n++] = (char *)scale_str;
        if (rotated) {
            args[n++] = "--transform"; args[n++] = (char *)transform;
        }
    }

    /* External output: above eDP-1 when connected (HDMI / TB / USB-C DP) */
    if (ext_on) {
        args[n++] = "--logical-monitor";
        if (!strcmp(primary, ext_output)) args[n++] = "--primary";
        args[n++] = "--monitor"; args[n++] = (char *)ext_output;
        args[n++] = "--above";   args[n++] = "eDP-1";
    }

    args[n] = NULL;
    return exec_cmd_argv("gdctl", args);
}

/*
 * Default eDP mode/rate — used when switching eDP-2 alone or when
 * applying rotation without an explicit mode from the caller.
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
    snprintf(spec, sizeof(spec), "%s@%ld.000", mode, atol(rate));
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
    /* Translate gdctl connector name to DRM sysfs name (HDMI-1 → HDMI-A-1). */
    const char *drm = gdctl_name_to_drm(output);
    char path[512];
    snprintf(path, sizeof(path), "/sys/class/drm/%s-%s/enabled", get_drm_card(), drm);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char buf[16] = {0};
    int on = 0;
    if (fgets(buf, sizeof(buf), fp))
        on = !strcmp(buf, "enabled\n");
    fclose(fp);
    return on;
}

/*
 * Apply rotation to the eDP panels.
 *
 * gdctl requires the complete layout topology on every set call, so we
 * cannot just call `gdctl set --logical-monitor --monitor eDP-1
 * --transform 90` in isolation — that fails with "Config is missing
 * primary logical". Instead we:
 *
 *   1. Persist the new rotation in g_edp_rotation.
 *   2. Delegate to gdctl_apply_layout(), which always builds the full
 *      topology and now includes --transform from g_edp_rotation.
 *
 * Both eDP panels share one rotation value and are handled in a single
 * gdctl call regardless of how many times apply_orientation() calls
 * this function (once for eDP-1, once for eDP-2 if active). The
 * second call is a harmless no-op that rebuilds the same topology.
 *
 * HDMI-1 is intentionally excluded from rotation — external monitors
 * are never rotated by the accelerometer.
 */
static int gdctl_set_rotation(const char *output, display_rotation r)
{
    if (!output) { errno = EINVAL; return -1; }

    /* Both eDP panels rotate together; ignore rotation requests for
     * anything that is not an internal panel. */
    if (strcmp(output, "eDP-1") != 0 && strcmp(output, "eDP-2") != 0)
        return 0;

    /* Skip gdctl if rotation hasn't changed — avoids redundant monitors-changed
     * events (and D2P resets) when orientation monitor or eDP-2 call duplicates
     * an already-applied rotation. */
    if (g_edp_rotation == r)
        return 0;

    g_edp_rotation = r;

    int edp2_on = gdctl_is_output_on("eDP-2");
    return gdctl_apply_layout(EDP1_DEFAULT_MODE, EDP1_DEFAULT_RATE,
                              edp2_on,
                              edp2_on ? EDP1_DEFAULT_MODE : NULL,
                              edp2_on ? EDP1_DEFAULT_RATE : NULL);
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

/* ---- primary management ------------------------------------------ */

static int gdctl_set_primary(const char *output)
{
    /* NULL or empty string resets to automatic selection. */
    if (!output || !output[0]) {
        g_primary_output[0] = '\0';
    } else {
        if (strlen(output) >= sizeof(g_primary_output)) { errno = EINVAL; return -1; }
        strncpy(g_primary_output, output, sizeof(g_primary_output) - 1);
        g_primary_output[sizeof(g_primary_output) - 1] = '\0';
    }
    /* Rebuild layout immediately so the change is visible at once. */
    int edp2_on = gdctl_is_output_on("eDP-2");
    return gdctl_apply_layout(EDP1_DEFAULT_MODE, EDP1_DEFAULT_RATE,
                              edp2_on,
                              edp2_on ? EDP1_DEFAULT_MODE : NULL,
                              edp2_on ? EDP1_DEFAULT_RATE : NULL);
}

static const char *gdctl_get_primary(void)
{
    /* If the user forced a specific output, return it (even if currently
     * disconnected — the tray uses this to show the manual selection). */
    if (g_primary_output[0])
        return g_primary_output;
    /* Auto: first connected external, else eDP-1. */
    const char *ext = detect_external_output();
    return ext ? ext : "eDP-1";
}

static int gdctl_is_output_connected(const char *output)
{
    if (!output) return 0;
    /* eDP panels are always physically present. */
    if (!strcmp(output, "eDP-1") || !strcmp(output, "eDP-2"))
        return 1;
    /* For external outputs, map gdctl name → DRM sysfs name. */
    const char *drm = gdctl_name_to_drm(output);
    return drm_connector_connected(drm);
}

/* ---- registration ------------------------------------------------ */

static const struct display_backend gdctl_backend = {
    .name                = "gdctl",
    .probe               = gdctl_probe,
    .set_output          = gdctl_set_output,
    .is_output_on        = gdctl_is_output_on,
    .set_rotation        = gdctl_set_rotation,
    .set_wallpapers      = gdctl_set_wallpapers,
    .set_primary         = gdctl_set_primary,
    .get_primary         = gdctl_get_primary,
    .is_output_connected = gdctl_is_output_connected,
};

__attribute__((constructor))
static void register_gdctl_backend(void)
{
    display_backend_register(&gdctl_backend);
}
