/*
 * tray_sni.c — minimal org.kde.StatusNotifierItem implementation using GIO.
 *
 * Replaces libayatana-appindicator-glib.  The library cannot coexist with
 * libdbusmenu-glib at the same D-Bus object path (each tries to register
 * object vtables and the second one fails silently).  Implementing the SNI
 * spec directly with GIO avoids the conflict: the SNI object lives at
 * /StatusNotifierItem and the DbusmenuServer lives at a separate sub-path.
 *
 * Protocol
 * ────────
 * 1. Own bus name "org.kde.StatusNotifierItem-<pid>-1".
 * 2. Register /StatusNotifierItem implementing org.kde.StatusNotifierItem.
 * 3. Call org.kde.StatusNotifierWatcher.RegisterStatusNotifierItem with the
 *    SNI object path so the shell extension picks up the indicator.
 * 4. The Menu property points to SNI_MENU_PATH where the caller
 *    (gui_daemon.c) will create its DbusmenuServer.
 */

#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "tray_sni.h"

#define SNI_OBJECT_PATH   "/StatusNotifierItem"
#define SNI_MENU_PATH     "/StatusNotifierItem/Menu"

/* SNI D-Bus interface XML. */
static const char SNI_XML[] =
    "<node>"
    "  <interface name='org.kde.StatusNotifierItem'>"
    "    <property name='Id'               type='s' access='read'/>"
    "    <property name='Category'         type='s' access='read'/>"
    "    <property name='Status'           type='s' access='read'/>"
    "    <property name='Title'            type='s' access='read'/>"
    "    <property name='IconName'         type='s' access='read'/>"
    "    <property name='AttentionIconName' type='s' access='read'/>"
    "    <property name='OverlayIconName'  type='s' access='read'/>"
    "    <property name='Menu'             type='o' access='read'/>"
    "    <property name='ItemIsMenu'       type='b' access='read'/>"
    "    <method name='Activate'>"
    "      <arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "    </method>"
    "    <method name='SecondaryActivate'>"
    "      <arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "    </method>"
    "    <method name='ContextMenu'>"
    "      <arg type='i' direction='in'/><arg type='i' direction='in'/>"
    "    </method>"
    "    <method name='Scroll'>"
    "      <arg type='i' direction='in'/><arg type='s' direction='in'/>"
    "    </method>"
    "    <signal name='NewStatus'><arg type='s'/></signal>"
    "    <signal name='NewIcon'/>"
    "    <signal name='NewTitle'/>"
    "    <signal name='NewAttentionIcon'/>"
    "  </interface>"
    "</node>";

static const char *g_icon_name = "dialog-information";
static const char *g_title     = "";
static guint       g_name_owner_id = 0;

static GVariant *sni_get_property(GDBusConnection *conn, const char *sender,
    const char *path, const char *iface, const char *prop,
    GError **err, gpointer data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)err; (void)data;

    if (!strcmp(prop, "Id"))               return g_variant_new_string("zbd-indicator");
    if (!strcmp(prop, "Category"))         return g_variant_new_string("Hardware");
    if (!strcmp(prop, "Status"))           return g_variant_new_string("Active");
    if (!strcmp(prop, "Title"))            return g_variant_new_string(g_title);
    if (!strcmp(prop, "IconName"))         return g_variant_new_string(g_icon_name);
    if (!strcmp(prop, "AttentionIconName")) return g_variant_new_string("");
    if (!strcmp(prop, "OverlayIconName")) return g_variant_new_string("");
    if (!strcmp(prop, "Menu"))             return g_variant_new_object_path(SNI_MENU_PATH);
    if (!strcmp(prop, "ItemIsMenu"))       return g_variant_new_boolean(TRUE);
    return NULL;
}

static void sni_method_call(GDBusConnection *conn, const char *sender,
    const char *path, const char *iface, const char *method,
    GVariant *params, GDBusMethodInvocation *invoc, gpointer data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)method;
    (void)params; (void)data;
    g_dbus_method_invocation_return_value(invoc, NULL);
}

static const GDBusInterfaceVTable sni_vtable = {
    .method_call  = sni_method_call,
    .get_property = sni_get_property,
    .set_property = NULL,
};

static void on_name_acquired(GDBusConnection *conn, const char *name, gpointer data)
{
    (void)data;

    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(SNI_XML, NULL);
    if (!node) {
        fprintf(stderr, "tray_sni: XML parse failed\n");
        return;
    }

    GError *err = NULL;
    guint reg = g_dbus_connection_register_object(conn, SNI_OBJECT_PATH,
        node->interfaces[0], &sni_vtable, NULL, NULL, &err);
    g_dbus_node_info_unref(node);

    if (!reg) {
        fprintf(stderr, "tray_sni: register_object: %s\n",
                err ? err->message : "unknown error");
        g_clear_error(&err);
        return;
    }

    /* Tell the StatusNotifierWatcher about us.  Passing our object path;
     * the watcher pairs it with the sender's bus name automatically. */
    g_dbus_connection_call(conn,
        "org.kde.StatusNotifierWatcher",
        "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher",
        "RegisterStatusNotifierItem",
        g_variant_new("(s)", SNI_OBJECT_PATH),
        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);

    fprintf(stderr, "tray_sni: registered as %s%s\n", name, SNI_OBJECT_PATH);
}

static void on_name_lost(GDBusConnection *conn, const char *name, gpointer data)
{
    (void)conn; (void)data;
    fprintf(stderr, "tray_sni: lost bus name %s\n", name);
}

/* ---- public API ---------------------------------------------------- */

void tray_sni_init(const char *icon_name, const char *title)
{
    g_icon_name = icon_name ? icon_name : "dialog-information";
    g_title     = title     ? title     : "";

    char bus_name[64];
    snprintf(bus_name, sizeof(bus_name),
             "org.kde.StatusNotifierItem-%d-1", getpid());

    g_name_owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, bus_name,
        G_BUS_NAME_OWNER_FLAGS_NONE,
        NULL,             /* bus_acquired_handler — not needed */
        on_name_acquired,
        on_name_lost,
        NULL, NULL);
}

const char *tray_sni_menu_path(void)
{
    return SNI_MENU_PATH;
}
