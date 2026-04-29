/*
 * dash_to_panel.h — integration with the dash-to-panel GNOME Shell
 * extension and removal of conflicts with `hidetopbar`.
 *
 * Two responsibilities, both opt-out via the
 * `dash_to_panel_gestionar` config key in /etc/zbd/zbd.conf:
 *
 *   1. Always-visible panel
 *      ───────────────────
 *      Two independent mechanisms can hide the panel on app
 *      maximisation:
 *        (a) D2P's own `intellihide` family.
 *        (b) The `hidetopbar@mathieu.bidon.ca` extension, which
 *            hides the GNOME top bar (which D2P substitutes) when an
 *            app is maximised, provided its `enable-intellihide` is
 *            true (its default).
 *      `d2p_apply_visibility_settings()` forces (a) off via the
 *      D2P GSettings schema, removes `hidetopbar` from
 *      `org.gnome.shell.enabled-extensions`, AND additionally sets
 *      `hidetopbar.enable-intellihide=false` so that even if the user
 *      re-enables the extension manually it does not regress the bug.
 *
 *   2. Primary-monitor sync
 *      ────────────────────
 *      D2P does not key its monitors by DRM connector but by a
 *      synthetic id `${vendor}-${serial}`, with fall-back to the
 *      connector name on collision and to the index on a second
 *      collision (see panelSettings.js `_saveMonitors`).
 *      `d2p_set_primary_monitor()` reproduces that exact algorithm
 *      against `org.gnome.Mutter.DisplayConfig.GetCurrentState`,
 *      writes the resolved id into D2P's `primary-monitor` setting,
 *      and forces `multi-monitors=false` so the explicit choice is
 *      honoured.
 *
 * All entry points are no-ops (returning 0) when the corresponding
 * GSettings schema is not installed, so callers do not have to guard.
 */

#ifndef ZBD_DASH_TO_PANEL_H
#define ZBD_DASH_TO_PANEL_H

/*
 * Returns 1 if the dash-to-panel GSettings schema can be loaded
 * (system-wide or from a user-local extension install), 0 otherwise.
 */
int d2p_is_available(void);

/*
 * Apply the always-visible policy described above.
 *
 * Returns 0 on success or partial success (every step that could be
 * done was done; a missing schema is not an error). Returns -1 only
 * when both D2P and hidetopbar handling fail in unexpected ways.
 */
int d2p_apply_visibility_settings(void);

/*
 * Pin the D2P panel to the monitor identified by DRM connector
 * (e.g. "eDP-1", "DP-2"). Translates the connector to D2P's
 * `vendor-serial` id (with the documented fall-backs) and writes:
 *   primary-monitor = <translated id>
 *   multi-monitors  = false
 *
 * Returns 0 on success or no-op (D2P not installed), -1 on resolution
 * or D-Bus failure.
 */
int d2p_set_primary_monitor(const char *connector);

#endif /* ZBD_DASH_TO_PANEL_H */
