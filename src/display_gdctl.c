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
 * read the DRM sysfs file (/sys/class/drm/card<N>-<output>/enabled),
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
 *
 * Centering
 * ─────────
 * When an external monitor is connected, gdctl_apply_layout() centres
 * the internal eDP stack horizontally under it (or the external under
 * the internal stack if the internal panels are wider). Absolute --x/--y
 * coordinates are derived from:
 *   1. The external monitor's native physical mode read from DRM sysfs
 *      (/sys/class/drm/<card>-<conn>/modes, first line = preferred mode).
 *   2. The external monitor's current logical scale obtained via the
 *      Mutter D-Bus interface (org.gnome.Mutter.DisplayConfig
 *      GetCurrentState). This is the actual scale Mutter is applying,
 *      not an estimate.
 *   logical_width  = physical_width  / mutter_scale
 *   logical_height = physical_height / mutter_scale
 *
 * If the Mutter query fails (e.g. session bus not up yet) or the sysfs
 * read fails, the code falls back to --above eDP-1 (prior behaviour,
 * left-aligned).
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib-2.0/gio/gio.h>

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

/* ---- external monitor dimension helpers ----------------------------- */

/*
 * Read the preferred (first) mode from the DRM connector's sysfs modes file.
 * Returns 1 on success with *w and *h set, 0 if the file cannot be read.
 */
static int drm_preferred_mode(const char *gdctl_name, int *w, int *h)
{
    const char *drm_name = gdctl_name_to_drm(gdctl_name);
    char path[256];
    snprintf(path, sizeof(path), "/sys/class/drm/%s-%s/modes",
             get_drm_card(), drm_name);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[32] = {0};
    int ok = (fgets(line, sizeof(line), fp) != NULL);
    fclose(fp);
    if (!ok || !*line) return 0;
    return sscanf(line, "%dx%d", w, h) == 2;
}

/*
 * Query the current Mutter scale for the logical monitor containing
 * `connector` via org.gnome.Mutter.DisplayConfig GetCurrentState.
 * Returns the scale (e.g. 1.25) or 0.0 on failure.
 *
 * Calling this before a gdctl set gives us the scale Mutter is CURRENTLY
 * applying to the external — which is the scale gdctl will preserve for it
 * when we call `gdctl set` without an explicit --scale for that connector.
 */
static double mutter_connector_scale(const char *connector)
{
    GError *err = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SESSION,
        G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.gnome.Mutter.DisplayConfig",
        "/org/gnome/Mutter/DisplayConfig",
        "org.gnome.Mutter.DisplayConfig",
        NULL, &err);
    if (!proxy) {
        if (err) g_error_free(err);
        return 0.0;
    }

    GVariant *result = g_dbus_proxy_call_sync(proxy, "GetCurrentState",
        NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
    g_object_unref(proxy);
    if (!result) {
        if (err) g_error_free(err);
        return 0.0;
    }

    /*
     * Result type: (u, a((ssss)a(siiddada{sv})a{sv}), a(iiduba(ssss)a{sv}), a{sv})
     * We only care about the logical_monitors (3rd element).
     * Logical monitor tuple: (i x, i y, d scale, u transform, b primary,
     *                          a(ssss) monitors, a{sv} properties)
     */
    guint32 serial;
    GVariant *mons_v, *log_v, *props_v;
    g_variant_get(result,
                  "(u"
                  "@a((ssss)a(siiddada{sv})a{sv})"
                  "@a(iiduba(ssss)a{sv})"
                  "@a{sv})",
                  &serial, &mons_v, &log_v, &props_v);

    double found_scale = 0.0;
    int found = 0;

    GVariantIter lm_iter;
    g_variant_iter_init(&lm_iter, log_v);
    GVariant *lm;
    while (!found && (lm = g_variant_iter_next_value(&lm_iter)) != NULL) {
        gint32 lm_x, lm_y;
        gdouble lm_scale;
        guint32 lm_tr;
        gboolean lm_primary;
        GVariant *lm_mons, *lm_pr;
        g_variant_get(lm, "(iidub@a(ssss)@a{sv})",
                      &lm_x, &lm_y, &lm_scale, &lm_tr, &lm_primary,
                      &lm_mons, &lm_pr);

        GVariantIter m_iter;
        g_variant_iter_init(&m_iter, lm_mons);
        GVariant *mon;
        while (!found && (mon = g_variant_iter_next_value(&m_iter)) != NULL) {
            const char *c, *v, *p, *s;
            g_variant_get(mon, "(ssss)", &c, &v, &p, &s);
            if (!strcmp(c, connector)) {
                found_scale = lm_scale;
                found = 1;
            }
            g_variant_unref(mon);
        }

        g_variant_unref(lm_mons);
        g_variant_unref(lm_pr);
        g_variant_unref(lm);
    }

    g_variant_unref(mons_v);
    g_variant_unref(log_v);
    g_variant_unref(props_v);
    g_variant_unref(result);
    return found_scale;
}

/*
 * Compute the logical dimensions of an external output.
 * Sets *lw and *lh to (physical_w / scale, physical_h / scale).
 * Returns 1 on success, 0 if dimensions cannot be determined.
 */
static int ext_logical_dims(const char *output_name, int *lw, int *lh)
{
    int pw = 0, ph = 0;
    if (!drm_preferred_mode(output_name, &pw, &ph) || pw == 0 || ph == 0)
        return 0;
    double scale = mutter_connector_scale(output_name);
    if (scale <= 0.0) return 0;
    *lw = (int)(pw / scale + 0.5);
    *lh = (int)(ph / scale + 0.5);
    return 1;
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
 *   eDP-1  — always on, primary (or below external primary), at the
 *             configured mode.  Horizontally centred relative to the
 *             external monitor when one is present.
 *   eDP-2  — when on, placed directly below eDP-1 (same x, same width).
 *   External (HDMI/DP/Thunderbolt) — primary, placed above eDP-1.
 *             Horizontally centred relative to eDP-1 if it is narrower.
 *
 * Positioning strategy:
 *   When an external monitor is present and its logical dimensions can
 *   be determined (DRM sysfs + Mutter scale query), explicit --x/--y
 *   absolute coordinates are used for all logical monitors so that the
 *   narrower stack is centred under the wider one.  If dimension
 *   detection fails, the code falls back to --above/--below relative
 *   placement (prior behaviour, left-aligned).
 *
 * Both eDP panels receive --transform from g_edp_rotation so that
 * layout rebuilds triggered by keyboard events preserve any rotation
 * previously set by the accelerometer monitor.
 *
 * Argv slots budget (worst case — ext + eDP2 + rotation + abs pos):
 *   2 (gdctl set)
 *  +14 (eDP-1: --lm [--primary] --monitor --mode --scale --x --y [--transform])
 *  +13 (eDP-2: --lm --monitor --mode --x --y --scale [--transform])
 *  + 8 (ext:   --lm [--primary] --monitor --x --y)
 *  + 1 (NULL)
 *  = 38  → array sized at 48.
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
        int req_connected = !strcmp(g_primary_output, "eDP-1") ? 1
                          : (ext_output && !strcmp(g_primary_output, ext_output));
        primary = req_connected ? g_primary_output
                                : (ext_on ? ext_output : "eDP-1");
    } else {
        primary = ext_on ? ext_output : "eDP-1";
    }

    /* ------------------------------------------------------------------ *
     * Centering geometry                                                  *
     *                                                                     *
     * eDP logical size adjusted for the current rotation:                *
     *   normal / 180°  → (phys_w / scale,  phys_h / scale)              *
     *   90°   / 270°   → (phys_h / scale,  phys_w / scale)              *
     *                                                                     *
     * When an external monitor is present and its logical dimensions are  *
     * determinable, we use absolute --x/--y so that the narrower stack   *
     * is horizontally centred under the wider one.  On failure we fall    *
     * back to --above/--below (left-aligned, previous behaviour).        *
     *                                                                     *
     * We parse scale_str ourselves — atof()/strtod() are locale-dependent *
     * and misparse "1.2" as 1.0 under Spanish locale (where '.' is not   *
     * the decimal separator).  We must use the configured scale (what we  *
     * are about to pass to gdctl --scale) rather than the current Mutter  *
     * scale for eDP-1, since gdctl will change eDP-1's scale to this     *
     * value during the same call.                                         *
     * ------------------------------------------------------------------ */
    /* Query the eDP-1 scale Mutter is CURRENTLY applying.  This is the
     * value that determines the actual logical width of eDP-1 after our
     * gdctl call (Mutter snaps --scale to its fractional-scaling grid and
     * may not honour "1.2" if "1.25" is the nearest supported step).
     * Using the Mutter-reported value avoids centering errors when the
     * effective scale differs from the config string, and sidesteps the
     * locale-dependent atof/strtod issue entirely.
     *
     * Fallback: if the Mutter query fails (session bus not up yet, or
     * eDP-1 not in any logical monitor), parse scale_str ourselves in a
     * locale-independent way (both '.' and ',' accepted as decimal mark). */
    double scale_val = mutter_connector_scale("eDP-1");
    if (scale_val <= 0.0) {
        const char *p = scale_str;
        long ipart = 0, fpart = 0, fdiv = 1;
        while (*p >= '0' && *p <= '9') ipart = ipart * 10 + (*p++ - '0');
        if (*p == '.' || *p == ',') {
            p++;
            while (*p >= '0' && *p <= '9') { fpart = fpart * 10 + (*p++ - '0'); fdiv *= 10; }
        }
        scale_val = (double)ipart + (double)fpart / (double)fdiv;
    }
    if (scale_val <= 0.0) scale_val = 1.25;

    int edp_phys_w = 2880, edp_phys_h = 1800;
    sscanf(eDP1_mode, "%dx%d", &edp_phys_w, &edp_phys_h); /* parse from caller */

    int edp_lw, edp_lh;
    if (g_edp_rotation == DISPLAY_ROTATION_LEFT_UP ||
        g_edp_rotation == DISPLAY_ROTATION_RIGHT_UP) {
        edp_lw = (int)(edp_phys_h / scale_val + 0.5);
        edp_lh = (int)(edp_phys_w / scale_val + 0.5);
    } else {
        edp_lw = (int)(edp_phys_w / scale_val + 0.5);
        edp_lh = (int)(edp_phys_h / scale_val + 0.5);
    }

    /* Absolute positions (logical pixels). Computed when ext present. */
    int ext_x = 0,  ext_y = 0;
    int edp1_x = 0, edp1_y = 0;
    int have_abs_pos = 0; /* 1 = use --x/--y; 0 = fallback --above/--below */

    if (ext_on) {
        int ext_lw = 0, ext_lh = 0;
        if (ext_logical_dims(ext_output, &ext_lw, &ext_lh) && ext_lw > 0 && ext_lh > 0) {
            have_abs_pos = 1;
            /* Centre the narrower stack under the wider. */
            if (ext_lw >= edp_lw) {
                ext_x  = 0;
                edp1_x = (ext_lw - edp_lw) / 2;
            } else {
                edp1_x = 0;
                ext_x  = (edp_lw - ext_lw) / 2;
            }
            ext_y  = 0;
            edp1_y = ext_lh; /* eDP-1 directly below external */
            fprintf(stderr,
                    "gdctl: centering — ext=%dx%d edp=%dx%d "
                    "ext_x=%d edp1_x=%d edp1_y=%d\n",
                    ext_lw, ext_lh, edp_lw, edp_lh,
                    ext_x, edp1_x, edp1_y);
        } else {
            fprintf(stderr,
                    "gdctl: cannot determine external dimensions for %s; "
                    "falling back to --above eDP-1\n", ext_output);
        }
    }

    /* ---- Build argv -------------------------------------------------- */
    char *args[48];
    int n = 0;

    /* Stack-allocated position string buffers (alive until exec_cmd_argv). */
    char ext_x_s[16],  ext_y_s[16];
    char edp1_x_s[16], edp1_y_s[16];
    char edp2_x_s[16], edp2_y_s[16];

    args[n++] = "gdctl";
    args[n++] = "set";

    /* eDP-1 */
    args[n++] = "--logical-monitor";
    if (!strcmp(primary, "eDP-1")) args[n++] = "--primary";
    args[n++] = "--monitor"; args[n++] = "eDP-1";
    args[n++] = "--mode";    args[n++] = eDP1_spec;
    args[n++] = "--scale";   args[n++] = (char *)scale_str;
    if (have_abs_pos) {
        snprintf(edp1_x_s, sizeof(edp1_x_s), "%d", edp1_x);
        snprintf(edp1_y_s, sizeof(edp1_y_s), "%d", edp1_y);
        args[n++] = "--x"; args[n++] = edp1_x_s;
        args[n++] = "--y"; args[n++] = edp1_y_s;
    }
    if (rotated) {
        args[n++] = "--transform"; args[n++] = (char *)transform;
    }

    /* eDP-2: directly below eDP-1 (same x) */
    if (eDP2_on) {
        args[n++] = "--logical-monitor";
        args[n++] = "--monitor"; args[n++] = "eDP-2";
        args[n++] = "--mode";    args[n++] = eDP2_spec;
        if (have_abs_pos) {
            snprintf(edp2_x_s, sizeof(edp2_x_s), "%d", edp1_x);
            snprintf(edp2_y_s, sizeof(edp2_y_s), "%d", edp1_y + edp_lh);
            args[n++] = "--x"; args[n++] = edp2_x_s;
            args[n++] = "--y"; args[n++] = edp2_y_s;
        } else {
            args[n++] = "--below"; args[n++] = "eDP-1";
        }
        args[n++] = "--scale"; args[n++] = (char *)scale_str;
        if (rotated) {
            args[n++] = "--transform"; args[n++] = (char *)transform;
        }
    }

    /* External output: above the eDP stack */
    if (ext_on) {
        args[n++] = "--logical-monitor";
        if (!strcmp(primary, ext_output)) args[n++] = "--primary";
        args[n++] = "--monitor"; args[n++] = (char *)ext_output;
        if (have_abs_pos) {
            snprintf(ext_x_s, sizeof(ext_x_s), "%d", ext_x);
            snprintf(ext_y_s, sizeof(ext_y_s), "%d", ext_y);
            args[n++] = "--x"; args[n++] = ext_x_s;
            args[n++] = "--y"; args[n++] = ext_y_s;
        } else {
            args[n++] = "--above"; args[n++] = "eDP-1";
        }
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
