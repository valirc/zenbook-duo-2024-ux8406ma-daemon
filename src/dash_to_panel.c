/*
 * dash_to_panel.c — dash-to-panel and hidetopbar integration.
 *
 * See dash_to_panel.h for the rationale.  This file uses libgio
 * directly (no `gsettings` subprocess) so it can load schemas from
 * a user-local extension install path that is not part of
 * $XDG_DATA_DIRS — which is the case on Ubuntu 26.04 when the
 * extensions live under ~/.local/share/gnome-shell/extensions/.
 *
 * Schema discovery order:
 *   1. Default schema source (system-wide install, e.g. the deb).
 *   2. $XDG_DATA_HOME (typically ~/.local/share).
 *   3. Every directory in g_get_system_data_dirs().
 *
 * The first directory that contains the schema wins; subsequent ones
 * are not consulted.  All callers that interact with the schema go
 * through `open_settings_for_uuid()` so the discovery is uniform.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>
#include <glib.h>

#include "dash_to_panel.h"

#define D2P_UUID         "dash-to-panel@jderose9.github.com"
#define D2P_SCHEMA       "org.gnome.shell.extensions.dash-to-panel"

#define HIDETOPBAR_UUID    "hidetopbar@mathieu.bidon.ca"
#define HIDETOPBAR_SCHEMA  "org.gnome.shell.extensions.hidetopbar"

#define SHELL_SCHEMA       "org.gnome.shell"
#define SHELL_KEY_ENABLED  "enabled-extensions"

/* ----- schema discovery --------------------------------------------- */

static GSettings *open_schema_in_dir(const char *schema_dir, const char *schema_id)
{
    GError *err = NULL;
    GSettingsSchemaSource *src = g_settings_schema_source_new_from_directory(
        schema_dir, NULL, FALSE, &err);
    if (!src) { g_clear_error(&err); return NULL; }

    GSettingsSchema *schema = g_settings_schema_source_lookup(src, schema_id, FALSE);
    GSettings *result = NULL;
    if (schema) {
        result = g_settings_new_full(schema, NULL, NULL);
        g_settings_schema_unref(schema);
    }
    g_settings_schema_source_unref(src);
    return result;
}

/*
 * Try to open the GSettings for `schema_id`, looking first in the
 * default source and then in each well-known location for an
 * extension UUID `extension_uuid`.  Returns NULL if nothing matches.
 */
static GSettings *open_settings_for_uuid(const char *schema_id, const char *extension_uuid)
{
    /* 1. Default source. */
    GSettingsSchemaSource *def = g_settings_schema_source_get_default();
    if (def) {
        GSettingsSchema *schema = g_settings_schema_source_lookup(def, schema_id, TRUE);
        if (schema) {
            g_settings_schema_unref(schema);
            return g_settings_new(schema_id);
        }
    }

    /* 2. User data dir. */
    {
        char *p = g_build_filename(g_get_user_data_dir(),
            "gnome-shell", "extensions", extension_uuid, "schemas", NULL);
        GSettings *s = open_schema_in_dir(p, schema_id);
        g_free(p);
        if (s) return s;
    }

    /* 3. System data dirs. */
    const char * const *sys = g_get_system_data_dirs();
    for (int i = 0; sys && sys[i]; i++) {
        char *p = g_build_filename(sys[i],
            "gnome-shell", "extensions", extension_uuid, "schemas", NULL);
        GSettings *s = open_schema_in_dir(p, schema_id);
        g_free(p);
        if (s) return s;
    }
    return NULL;
}

/* ----- public availability ------------------------------------------ */

int d2p_is_available(void)
{
    GSettings *s = open_settings_for_uuid(D2P_SCHEMA, D2P_UUID);
    if (!s) return 0;
    g_object_unref(s);
    return 1;
}

/* ----- shell extension toggle -------------------------------------- */

/*
 * Remove `uuid` from `org.gnome.shell.enabled-extensions` if present.
 * Returns 1 on removal, 0 on no-op, -1 on schema/IO error.
 */
static int gnome_shell_disable_extension(const char *uuid)
{
    GSettingsSchemaSource *def = g_settings_schema_source_get_default();
    if (!def) return -1;
    GSettingsSchema *schema = g_settings_schema_source_lookup(def, SHELL_SCHEMA, TRUE);
    if (!schema) return -1;
    g_settings_schema_unref(schema);

    GSettings *s = g_settings_new(SHELL_SCHEMA);
    char **arr = g_settings_get_strv(s, SHELL_KEY_ENABLED);
    int n = arr ? g_strv_length(arr) : 0;
    int present = 0;
    for (int i = 0; i < n; i++) if (!strcmp(arr[i], uuid)) { present = 1; break; }

    if (!present) {
        g_strfreev(arr);
        g_object_unref(s);
        return 0;
    }

    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    for (int i = 0; i < n; i++)
        if (strcmp(arr[i], uuid) != 0)
            g_ptr_array_add(out, g_strdup(arr[i]));
    g_ptr_array_add(out, NULL); /* strv NULL terminator */

    gboolean ok = g_settings_set_strv(s, SHELL_KEY_ENABLED,
        (const char * const *)out->pdata);

    g_ptr_array_free(out, TRUE);
    g_strfreev(arr);
    g_object_unref(s);

    if (ok) g_settings_sync();
    return ok ? 1 : -1;
}

/* ----- visibility lock --------------------------------------------- */

static void d2p_force_intellihide_off(void)
{
    GSettings *s = open_settings_for_uuid(D2P_SCHEMA, D2P_UUID);
    if (!s) {
        fprintf(stderr, "d2p: schema dash-to-panel ausente; saltando lock de intellihide\n");
        return;
    }

    GSettingsSchema *schema = NULL;
    g_object_get(s, "settings-schema", &schema, NULL);

    /* Three boolean keys, all on if the user enabled intellihide via
     * D2P prefs.  Set every one that exists in this version of the
     * schema (the schema has evolved across versions). */
    static const char *keys[] = {
        "intellihide",
        "intellihide-hide-from-windows",
        "intellihide-hide-from-monitor-windows",
        NULL
    };
    for (int i = 0; keys[i]; i++) {
        if (schema && g_settings_schema_has_key(schema, keys[i]))
            g_settings_set_boolean(s, keys[i], FALSE);
    }
    if (schema) g_settings_schema_unref(schema);

    g_settings_sync();
    g_object_unref(s);
    fprintf(stderr, "d2p: intellihide forzado a off\n");
}

/*
 * Force hidetopbar.enable-intellihide = false even if the user
 * re-enables the extension manually later.  Schema lives in the
 * extension dir, same discovery pattern as D2P.
 */
static void hidetopbar_force_intellihide_off(void)
{
    GSettings *s = open_settings_for_uuid(HIDETOPBAR_SCHEMA, HIDETOPBAR_UUID);
    if (!s) return;  /* hidetopbar not installed → nothing to do */

    GSettingsSchema *schema = NULL;
    g_object_get(s, "settings-schema", &schema, NULL);
    if (schema && g_settings_schema_has_key(schema, "enable-intellihide"))
        g_settings_set_boolean(s, "enable-intellihide", FALSE);
    if (schema) g_settings_schema_unref(schema);

    g_settings_sync();
    g_object_unref(s);
    fprintf(stderr, "d2p: hidetopbar.enable-intellihide forzado a false\n");
}

int d2p_apply_visibility_settings(void)
{
    int actions = 0;

    d2p_force_intellihide_off();
    actions++;

    hidetopbar_force_intellihide_off();
    actions++;

    int htb = gnome_shell_disable_extension(HIDETOPBAR_UUID);
    if (htb == 1)
        fprintf(stderr, "d2p: hidetopbar deshabilitado en enabled-extensions "
                "(impedia panel siempre visible al maximizar)\n");
    else if (htb == 0)
        fprintf(stderr, "d2p: hidetopbar no estaba en enabled-extensions; ok\n");
    /* htb == -1 → schema org.gnome.shell missing: rare, we just log */

    return actions ? 0 : -1;
}

/* ----- primary-monitor translation --------------------------------- */

/*
 * Reproduce dash-to-panel/panelSettings.js `_saveMonitors`:
 *
 *   id = `${vendor}-${serial}`
 *   if id collides:
 *       id = connector if connector does not collide
 *       else id = index_as_string
 *
 * Returns the resolved id for `target_connector` (g_free()'d by
 * caller), or NULL if the connector is not present in Mutter's
 * current state.
 */
static char *resolve_d2p_monitor_id(const char *target_connector)
{
    if (!target_connector) return NULL;

    GError *err = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
        G_DBUS_PROXY_FLAGS_NONE, NULL,
        "org.gnome.Mutter.DisplayConfig",
        "/org/gnome/Mutter/DisplayConfig",
        "org.gnome.Mutter.DisplayConfig",
        NULL, &err);
    if (!proxy) {
        if (err) { fprintf(stderr, "d2p: dbus proxy: %s\n", err->message); g_error_free(err); }
        return NULL;
    }

    GVariant *result = g_dbus_proxy_call_sync(proxy, "GetCurrentState",
        NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &err);
    g_object_unref(proxy);
    if (!result) {
        if (err) { fprintf(stderr, "d2p: GetCurrentState: %s\n", err->message); g_error_free(err); }
        return NULL;
    }

    /*
     * (u, a((ssss)a(siiddada{sv})a{sv}), a(iiduba(ssss)a{sv}), a{sv})
     *
     * D2P walks displayInfo[2] (logical_monitors).  Each element:
     *   (i x, i y, d scale, u transform, b primary,
     *    a(ssss) monitors, a{sv} props)
     * and reads logicalMonitor[5][0] = (s connector, s vendor, s product, s serial).
     */
    guint32 dserial;
    GVariant *mons_v, *log_v, *props_v;
    g_variant_get(result,
        "(u@a((ssss)a(siiddada{sv})a{sv})@a(iiduba(ssss)a{sv})@a{sv})",
        &dserial, &mons_v, &log_v, &props_v);

    /* Hash table that records ids already taken so we can detect
     * collisions in the same order as D2P does. */
    GHashTable *taken = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *found = NULL;

    GVariantIter lm_iter;
    g_variant_iter_init(&lm_iter, log_v);
    GVariant *lm;
    int idx = 0;
    while ((lm = g_variant_iter_next_value(&lm_iter)) != NULL) {
        gint32 lm_x, lm_y;
        gdouble lm_scale;
        guint32 lm_tr;
        gboolean lm_primary;
        GVariant *lm_mons, *lm_pr;
        g_variant_get(lm, "(iidub@a(ssss)@a{sv})",
            &lm_x, &lm_y, &lm_scale, &lm_tr, &lm_primary, &lm_mons, &lm_pr);

        if (g_variant_n_children(lm_mons) > 0) {
            GVariant *mon0 = g_variant_get_child_value(lm_mons, 0);
            const char *connector = NULL, *vendor = NULL,
                       *product   = NULL, *mser   = NULL;
            g_variant_get(mon0, "(&s&s&s&s)", &connector, &vendor, &product, &mser);

            char *id;
            if (vendor && *vendor && mser && *mser)
                id = g_strdup_printf("%s-%s", vendor, mser);
            else
                id = g_strdup_printf("%d", idx);

            if (g_hash_table_contains(taken, id)) {
                g_free(id);
                if (connector && *connector && !g_hash_table_contains(taken, connector))
                    id = g_strdup(connector);
                else
                    id = g_strdup_printf("%d", idx);
            }

            if (!found && connector && !strcmp(connector, target_connector))
                found = g_strdup(id);

            g_hash_table_add(taken, id);  /* takes ownership of id */
            g_variant_unref(mon0);
        }
        g_variant_unref(lm_mons);
        g_variant_unref(lm_pr);
        g_variant_unref(lm);
        idx++;
    }

    g_hash_table_destroy(taken);
    g_variant_unref(mons_v);
    g_variant_unref(log_v);
    g_variant_unref(props_v);
    g_variant_unref(result);
    return found;
}

int d2p_set_primary_monitor(const char *connector)
{
    if (!connector || !*connector) { errno = EINVAL; return -1; }

    GSettings *s = open_settings_for_uuid(D2P_SCHEMA, D2P_UUID);
    if (!s) return 0;  /* D2P not installed → silent no-op */

    char *id = resolve_d2p_monitor_id(connector);
    if (!id) {
        fprintf(stderr, "d2p: no se pudo resolver monitor-id para conector '%s' "
                "(¿desconectado?)\n", connector);
        g_object_unref(s);
        return -1;
    }

    g_settings_set_string(s, "primary-monitor", id);
    g_settings_set_boolean(s, "multi-monitors", FALSE);
    g_settings_sync();

    fprintf(stderr,
            "d2p: primary-monitor=%s (conector=%s) multi-monitors=false\n",
            id, connector);

    g_free(id);
    g_object_unref(s);
    return 0;
}
