/*
 * gui_daemon.c — zbd-tray: tray icon, monitor threads and config reload.
 *
 * Uses tray_sni.c (GIO-based StatusNotifierItem) + libdbusmenu-glib
 * (com.canonical.dbusmenu) for the context menu.  No GTK dependency.
 *
 * libayatana-appindicator-glib was dropped because it cannot share a D-Bus
 * object path with DbusmenuServer: both try to register vtables at the
 * same path and the second registration silently fails.  The custom SNI
 * implementation registers at /StatusNotifierItem while DbusmenuServer
 * registers at /StatusNotifierItem/Menu — no conflict.
 *
 * Radio-button state
 * ──────────────────
 * Items use toggle-type="radio" and toggle-state=1 for the checked item.
 * g_items_brillo[] and g_items_teclado[] hold borrowed references to
 * the individual level items so we can flip toggle-state on selection.
 *
 * Thread safety
 * ─────────────
 * All DbusmenuMenuitem mutations happen on the GLib main thread via
 * g_idle_add().  Monitor pthreads only call g_idle_add().
 */

#include <libdbusmenu-glib/server.h>
#include <libdbusmenu-glib/menuitem.h>
#include <glib-unix.h>
#include <libudev.h>
#include <pthread.h>
#include <sys/select.h>
#include <time.h>

#include "comun.h"
#include "exec.h"
#include "pantalla.h"
#include "teclado.h"
#include "config.h"
#include "monitor_bluetooth.h"
#include "monitor_orientacion.h"
#include "monitor_teclado_usb.h"
#include "audio.h"
#include "runtime.h"
#include "display.h"
#include "ipc.h"
#include "tray_sni.h"
#include "dash_to_panel.h"

/* ---- module state --------------------------------------------------- */

static GMainLoop          *g_loop;

static DbusmenuServer     *g_dbus_server;
static DbusmenuMenuitem   *g_root;
static DbusmenuMenuitem   *g_items_brillo[10];  /* [0]=10% .. [9]=100% */
static DbusmenuMenuitem   *g_items_teclado[4];  /* [0..3]              */
static DbusmenuMenuitem   *g_submenu_primario;

static pthread_t hilo_orientacion;
static pthread_t hilo_bluetooth;
static pthread_t hilo_usb;
static pthread_t hilo_drm;

static int system_service_available = 0;

/* ---- action callbacks ----------------------------------------------- */

/*
 * dbusmenu item_activated signal: void cb(DbusmenuMenuitem*, guint, gpointer)
 */

static void on_set_pantalla_brillo(DbusmenuMenuitem *item, guint timestamp,
                                   gpointer data)
{
    (void)item; (void)timestamp;
    int nivel = GPOINTER_TO_INT(data);

    for (int i = 0; i < 10; i++) {
        dbusmenu_menuitem_property_set_int(g_items_brillo[i],
            DBUSMENU_MENUITEM_PROP_TOGGLE_STATE,
            (i + 1) * 10 == nivel ? DBUSMENU_MENUITEM_TOGGLE_STATE_CHECKED
                                  : DBUSMENU_MENUITEM_TOGGLE_STATE_UNCHECKED);
    }

    if (system_service_available)
        zbd_ipc_client_set_screen_brightness(nivel);
    else
        set_pantalla_brillo(nivel);
    cfg->pantalla_nivel_brillo = nivel;

    if (monitor_estado("eDP-2")) {
        int sp = nivel * 235 / 100;
        if (sp < 10) sp = 10;
        if (system_service_available)
            zbd_ipc_client_set_screenpad_brightness(nivel);
        else
            set_screenpad_brillo(sp);
    }
}

static void on_set_teclado_brillo(DbusmenuMenuitem *item, guint timestamp,
                                  gpointer data)
{
    (void)item; (void)timestamp;
    int nivel = GPOINTER_TO_INT(data);

    for (int i = 0; i <= 3; i++) {
        dbusmenu_menuitem_property_set_int(g_items_teclado[i],
            DBUSMENU_MENUITEM_PROP_TOGGLE_STATE,
            i == nivel ? DBUSMENU_MENUITEM_TOGGLE_STATE_CHECKED
                       : DBUSMENU_MENUITEM_TOGGLE_STATE_UNCHECKED);
    }

    if (system_service_available)
        zbd_ipc_client_set_keyboard_backlight(nivel);
    else
        set_brillo_teclado(nivel);
    cfg->teclado_nivel_brillo = nivel;
}

/*
 * Walk every child of the "Monitor principal" submenu and set
 * toggle-state CHECKED on the one whose label equals `current`,
 * UNCHECKED on the rest.  Dbusmenu's "radio" toggle-type is purely a
 * rendering hint — the server is still in charge of enforcing the
 * mutually-exclusive state.  Without an explicit update on every
 * click the previous selection stays visually marked even though the
 * compositor has already switched primary.
 *
 * The submenu is rebuilt from scratch on DRM hotplug, so labels are
 * always exact connector names (e.g. "eDP-1", "DP-2").  Comparing on
 * the label keeps the helper independent of how the activated signal
 * was dispatched.
 */
static void primary_submenu_sync_toggle(const char *current)
{
    if (!g_submenu_primario || !current) return;
    GList *children = dbusmenu_menuitem_get_children(g_submenu_primario);
    for (GList *l = children; l; l = l->next) {
        DbusmenuMenuitem *it = DBUSMENU_MENUITEM(l->data);
        const char *label = dbusmenu_menuitem_property_get(
            it, DBUSMENU_MENUITEM_PROP_LABEL);
        gboolean checked = label && !strcmp(label, current);
        dbusmenu_menuitem_property_set_int(it,
            DBUSMENU_MENUITEM_PROP_TOGGLE_STATE,
            checked ? DBUSMENU_MENUITEM_TOGGLE_STATE_CHECKED
                    : DBUSMENU_MENUITEM_TOGGLE_STATE_UNCHECKED);
    }
}

static void on_set_primary_monitor(DbusmenuMenuitem *item, guint timestamp,
                                   gpointer data)
{
    (void)item; (void)timestamp;
    const char *output = (const char *)data;
    fprintf(stderr, "zbd-tray: monitor principal → %s\n", output);

    /*
     * Order matters here.  Dash-to-panel listens on TWO independent
     * signals that both rebuild the panel via PanelManager._reset():
     *
     *   1. SETTINGS 'changed::primary-monitor'  — synchronous
     *      handler, uses the current PanelSettings.monitorIdToIndex
     *      cache and the current Main.layoutManager.monitors array.
     *   2. Utils.DisplayWrapper.getMonitorManager() 'monitors-changed'
     *      — async handler that first awaits PanelSettings.
     *      setMonitorsInfo (a Mutter GetCurrentStateRemote round-trip)
     *      to refresh monitorIdToIndex, THEN calls _reset().
     *
     * If we run gdctl FIRST (Mutter swaps primary, layoutManager
     * monitors are re-ordered synchronously, monitors-changed is
     * queued) and THEN write primary-monitor (changed::primary-monitor
     * is queued), the synchronous handler #1 fires before #2's await
     * completes.  At that moment monitorIdToIndex is stale (still maps
     * the new id to its OLD logical-monitor index), but
     * Main.layoutManager.monitors is already reordered → the cached
     * index points at the WRONG entry of the new array.  The panel
     * ends up on the wrong monitor for one rebuild cycle, producing
     * the visible flicker and the missing-panel symptom on DP-2.
     *
     * Inverting the order eliminates the race: when the synchronous
     * handler runs, BOTH the D2P cache and Main.layoutManager.monitors
     * still reflect the pre-gdctl state — internally consistent —
     * and the second _reset() (after monitors-changed completes its
     * await) sees both views in their post-gdctl state — also
     * internally consistent.  Final placement is correct in both
     * cases regardless of which transition the user triggers.
     */
    if (cfg && cfg->dash_to_panel_gestionar)
        d2p_set_primary_monitor(output);

    display_set_primary(output);
    set_pantalla_brillo(cfg->pantalla_nivel_brillo);

    /* Sync the radio-button group with the new selection so the next
     * time the user opens the tray the marker matches the actual
     * primary.  Otherwise the user-visible state lags behind reality
     * until a DRM hotplug rebuilds the submenu. */
    primary_submenu_sync_toggle(output);
}

static void on_quit_item(DbusmenuMenuitem *item, guint timestamp, gpointer data)
{
    (void)item; (void)timestamp; (void)data;
    g_main_loop_quit(g_loop);
}

/* ---- primary-monitor submenu (rebuilt on DRM hotplug) --------------- */

static void rebuild_primary_submenu(void)
{
    static const char *candidates[] = {
        "HDMI-1", "HDMI-2", "DP-1", "DP-2", "DP-3", NULL
    };

    /* Remove all existing children (copy list first — deletion invalidates it). */
    GList *snap = g_list_copy(dbusmenu_menuitem_get_children(g_submenu_primario));
    for (GList *l = snap; l; l = l->next)
        dbusmenu_menuitem_child_delete(g_submenu_primario,
                                      DBUSMENU_MENUITEM(l->data));
    g_list_free(snap);

    const char *cur = display_get_primary();
    if (!cur) cur = "eDP-1";

    for (int i = 0; candidates[i]; i++) {
        if (!display_is_output_connected(candidates[i])) continue;
        DbusmenuMenuitem *it = dbusmenu_menuitem_new();
        dbusmenu_menuitem_property_set(it, DBUSMENU_MENUITEM_PROP_LABEL,
                                       candidates[i]);
        dbusmenu_menuitem_property_set(it, DBUSMENU_MENUITEM_PROP_TOGGLE_TYPE,
                                       DBUSMENU_MENUITEM_TOGGLE_RADIO);
        dbusmenu_menuitem_property_set_int(it, DBUSMENU_MENUITEM_PROP_TOGGLE_STATE,
            !strcmp(candidates[i], cur) ? DBUSMENU_MENUITEM_TOGGLE_STATE_CHECKED
                                        : DBUSMENU_MENUITEM_TOGGLE_STATE_UNCHECKED);
        g_signal_connect(it, DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                         G_CALLBACK(on_set_primary_monitor),
                         (gpointer)candidates[i]);
        dbusmenu_menuitem_child_append(g_submenu_primario, it);
        g_object_unref(it);
    }

    /* eDP-1 always present. */
    DbusmenuMenuitem *edp1 = dbusmenu_menuitem_new();
    dbusmenu_menuitem_property_set(edp1, DBUSMENU_MENUITEM_PROP_LABEL, "eDP-1");
    dbusmenu_menuitem_property_set(edp1, DBUSMENU_MENUITEM_PROP_TOGGLE_TYPE,
                                   DBUSMENU_MENUITEM_TOGGLE_RADIO);
    dbusmenu_menuitem_property_set_int(edp1, DBUSMENU_MENUITEM_PROP_TOGGLE_STATE,
        !strcmp("eDP-1", cur) ? DBUSMENU_MENUITEM_TOGGLE_STATE_CHECKED
                              : DBUSMENU_MENUITEM_TOGGLE_STATE_UNCHECKED);
    g_signal_connect(edp1, DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                     G_CALLBACK(on_set_primary_monitor), (gpointer)"eDP-1");
    dbusmenu_menuitem_child_append(g_submenu_primario, edp1);
    g_object_unref(edp1);
}

/* ---- signal / reload callbacks -------------------------------------- */

static gboolean on_shutdown_signal(gpointer data)
{
    (void)data;
    g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
}

static gboolean on_reload_config(gpointer data)
{
    (void)data;
    fprintf(stderr, "zbd-tray: SIGHUP — recargando configuracion\n");

    if (cargar_configuracion() < 0) {
        fprintf(stderr, "zbd-tray: reload: cargar_configuracion fallo; "
                "se mantiene la config anterior\n");
        return G_SOURCE_CONTINUE;
    }

    fprintf(stderr, "zbd-tray: reload: brillo=%d teclado=%d bateria=%d "
            "mic=%d%% altavoces=%d%%\n",
            cfg->pantalla_nivel_brillo, cfg->teclado_nivel_brillo,
            cfg->bateria_carga_maxima,
            cfg->audio_volumen_microfono, cfg->audio_volumen_altavoces);

    configurar_dmic_raw();

    if (system_service_available)
        zbd_ipc_client_set_screen_brightness(cfg->pantalla_nivel_brillo);
    else
        set_pantalla_brillo(cfg->pantalla_nivel_brillo);

    if (monitor_estado("eDP-2")) {
        int sp = cfg->pantalla_nivel_brillo * 235 / 100;
        if (sp < 10) sp = 10;
        if (system_service_available)
            zbd_ipc_client_set_screenpad_brightness(cfg->pantalla_nivel_brillo);
        else
            set_screenpad_brillo(sp);
    }

    if (system_service_available)
        zbd_ipc_client_set_keyboard_backlight(cfg->teclado_nivel_brillo);
    else
        set_brillo_teclado(cfg->teclado_nivel_brillo);

    if (system_service_available && cfg->bateria_carga_maxima > 0)
        zbd_ipc_client_set_battery_threshold(cfg->bateria_carga_maxima);

    /* Sync radio-button checked states to reloaded config values. */
    for (int i = 0; i < 10; i++) {
        int nivel = (i + 1) * 10;
        dbusmenu_menuitem_property_set_int(g_items_brillo[i],
            DBUSMENU_MENUITEM_PROP_TOGGLE_STATE,
            nivel == cfg->pantalla_nivel_brillo
                ? DBUSMENU_MENUITEM_TOGGLE_STATE_CHECKED
                : DBUSMENU_MENUITEM_TOGGLE_STATE_UNCHECKED);
    }
    for (int i = 0; i <= 3; i++) {
        dbusmenu_menuitem_property_set_int(g_items_teclado[i],
            DBUSMENU_MENUITEM_PROP_TOGGLE_STATE,
            i == cfg->teclado_nivel_brillo
                ? DBUSMENU_MENUITEM_TOGGLE_STATE_CHECKED
                : DBUSMENU_MENUITEM_TOGGLE_STATE_UNCHECKED);
    }

    fprintf(stderr, "zbd-tray: reload: completado\n");
    return G_SOURCE_CONTINUE;
}

/* ---- DRM hotplug monitor -------------------------------------------- */

static guint g_drm_rebuild_source = 0;

static gboolean on_drm_hotplug(gpointer data)
{
    (void)data;
    g_drm_rebuild_source = 0;
    rebuild_primary_submenu();
    return G_SOURCE_REMOVE;
}

static gboolean drm_schedule_rebuild(gpointer data)
{
    (void)data;
    if (g_drm_rebuild_source == 0)
        g_drm_rebuild_source = g_timeout_add(1000, on_drm_hotplug, NULL);
    return G_SOURCE_REMOVE;
}

static void *monitorizar_drm_hotplug(void *arg)
{
    (void)arg;
    struct udev *udev = udev_new();
    if (!udev) return NULL;

    struct udev_monitor *mon = udev_monitor_new_from_netlink(udev, "udev");
    if (!mon) { udev_unref(udev); return NULL; }

    udev_monitor_filter_add_match_subsystem_devtype(mon, "drm", NULL);
    udev_monitor_enable_receiving(mon);
    int fd = udev_monitor_get_fd(mon);

    struct timespec last_queued = {0, 0};

    while (1) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = { 5, 0 };
        if (select(fd + 1, &fds, NULL, NULL, &tv) > 0 && FD_ISSET(fd, &fds)) {
            struct udev_device *dev = udev_monitor_receive_device(mon);
            if (dev) {
                const char *action = udev_device_get_action(dev);
                if (action && !strcmp(action, "change")) {
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    long ms = (now.tv_sec  - last_queued.tv_sec)  * 1000
                            + (now.tv_nsec - last_queued.tv_nsec) / 1000000;
                    if (ms >= 500) {
                        last_queued = now;
                        g_idle_add(drm_schedule_rebuild, NULL);
                    }
                }
                udev_device_unref(dev);
            }
        }
    }
    udev_monitor_unref(mon);
    udev_unref(udev);
    return NULL;
}

/* ---- dbusmenu tree setup -------------------------------------------- */

static void setup_dbusmenu(void)
{
    /*
     * Register com.canonical.dbusmenu at the SNI menu sub-path.
     * This is a different path from the SNI object itself, so there is
     * no vtable conflict with the StatusNotifierItem registration.
     */
    g_dbus_server = dbusmenu_server_new(tray_sni_menu_path());
    g_root        = dbusmenu_menuitem_new();

    /* ---- Brillo pantalla (10%..100% radio group) ---- */
    DbusmenuMenuitem *sub_p = dbusmenu_menuitem_new();
    dbusmenu_menuitem_property_set(sub_p, DBUSMENU_MENUITEM_PROP_LABEL,
                                   "Brillo pantalla");
    dbusmenu_menuitem_property_set(sub_p, DBUSMENU_MENUITEM_PROP_CHILD_DISPLAY,
                                   DBUSMENU_MENUITEM_CHILD_DISPLAY_SUBMENU);

    int brillo_act = cfg ? cfg->pantalla_nivel_brillo : 20;
    for (int i = 0; i < 10; i++) {
        int nivel = (i + 1) * 10;
        char label[8];
        snprintf(label, sizeof(label), "%d%%", nivel);
        DbusmenuMenuitem *it = dbusmenu_menuitem_new();
        dbusmenu_menuitem_property_set(it, DBUSMENU_MENUITEM_PROP_LABEL, label);
        dbusmenu_menuitem_property_set(it, DBUSMENU_MENUITEM_PROP_TOGGLE_TYPE,
                                       DBUSMENU_MENUITEM_TOGGLE_RADIO);
        dbusmenu_menuitem_property_set_int(it, DBUSMENU_MENUITEM_PROP_TOGGLE_STATE,
            nivel == brillo_act ? DBUSMENU_MENUITEM_TOGGLE_STATE_CHECKED
                                : DBUSMENU_MENUITEM_TOGGLE_STATE_UNCHECKED);
        g_signal_connect(it, DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                         G_CALLBACK(on_set_pantalla_brillo), GINT_TO_POINTER(nivel));
        dbusmenu_menuitem_child_append(sub_p, it);
        g_items_brillo[i] = it;  /* borrowed ref — parent owns it */
        g_object_unref(it);
    }
    dbusmenu_menuitem_child_append(g_root, sub_p);
    g_object_unref(sub_p);

    /* ---- Brillo teclado (0..3 radio group) ---- */
    DbusmenuMenuitem *sub_t = dbusmenu_menuitem_new();
    dbusmenu_menuitem_property_set(sub_t, DBUSMENU_MENUITEM_PROP_LABEL,
                                   "Brillo teclado");
    dbusmenu_menuitem_property_set(sub_t, DBUSMENU_MENUITEM_PROP_CHILD_DISPLAY,
                                   DBUSMENU_MENUITEM_CHILD_DISPLAY_SUBMENU);

    int teclado_act = cfg ? cfg->teclado_nivel_brillo : 1;
    for (int i = 0; i <= 3; i++) {
        char label[4];
        snprintf(label, sizeof(label), "%d", i);
        DbusmenuMenuitem *it = dbusmenu_menuitem_new();
        dbusmenu_menuitem_property_set(it, DBUSMENU_MENUITEM_PROP_LABEL, label);
        dbusmenu_menuitem_property_set(it, DBUSMENU_MENUITEM_PROP_TOGGLE_TYPE,
                                       DBUSMENU_MENUITEM_TOGGLE_RADIO);
        dbusmenu_menuitem_property_set_int(it, DBUSMENU_MENUITEM_PROP_TOGGLE_STATE,
            i == teclado_act ? DBUSMENU_MENUITEM_TOGGLE_STATE_CHECKED
                             : DBUSMENU_MENUITEM_TOGGLE_STATE_UNCHECKED);
        g_signal_connect(it, DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                         G_CALLBACK(on_set_teclado_brillo), GINT_TO_POINTER(i));
        dbusmenu_menuitem_child_append(sub_t, it);
        g_items_teclado[i] = it;  /* borrowed ref — parent owns it */
        g_object_unref(it);
    }
    dbusmenu_menuitem_child_append(g_root, sub_t);
    g_object_unref(sub_t);

    /* ---- Monitor principal (dynamic, rebuilt on hotplug) ---- */
    g_submenu_primario = dbusmenu_menuitem_new();
    dbusmenu_menuitem_property_set(g_submenu_primario,
                                   DBUSMENU_MENUITEM_PROP_LABEL,
                                   "Monitor principal");
    dbusmenu_menuitem_property_set(g_submenu_primario,
                                   DBUSMENU_MENUITEM_PROP_CHILD_DISPLAY,
                                   DBUSMENU_MENUITEM_CHILD_DISPLAY_SUBMENU);
    rebuild_primary_submenu();
    dbusmenu_menuitem_child_append(g_root, g_submenu_primario);

    /* ---- Separator ---- */
    DbusmenuMenuitem *sep = dbusmenu_menuitem_new();
    dbusmenu_menuitem_property_set(sep, DBUSMENU_MENUITEM_PROP_TYPE, "separator");
    dbusmenu_menuitem_child_append(g_root, sep);
    g_object_unref(sep);

    /* ---- Salir ---- */
    DbusmenuMenuitem *quit_it = dbusmenu_menuitem_new();
    dbusmenu_menuitem_property_set(quit_it, DBUSMENU_MENUITEM_PROP_LABEL, "Salir");
    g_signal_connect(quit_it, DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
                     G_CALLBACK(on_quit_item), NULL);
    dbusmenu_menuitem_child_append(g_root, quit_it);
    g_object_unref(quit_it);

    dbusmenu_server_set_root(g_dbus_server, g_root);
}

/* ---- entry point ---------------------------------------------------- */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (zbd_install_signal_handlers() != 0)
        return 1;

    if (cargar_configuracion() < 0)
        return 1;

    if (display_init() < 0) {
        fprintf(stderr, "display: ningun backend disponible; abortando.\n");
        return 1;
    }

    system_service_available = zbd_ipc_client_is_service_available();
    fprintf(stderr, "ipc: zbd-system %s en el bus\n",
            system_service_available ? "presente"
                                     : "ausente; usando llamadas directas");

    g_loop = g_main_loop_new(NULL, FALSE);

    /*
     * Register the StatusNotifierItem and the dbusmenu context menu.
     * Both calls schedule async D-Bus work; the registrations complete
     * once g_main_loop_run() processes the first GLib iteration.
     */
    tray_sni_init("zbd-tray", "ZBD Tray");
    setup_dbusmenu();

    /* dash-to-panel always-visible policy.  Forces D2P intellihide off,
     * disables the conflicting `hidetopbar` extension, and pins the panel
     * to the current primary monitor.  No-op when D2P is not installed
     * or when the user opted out via /etc/zbd/zbd.conf. */
    if (cfg && cfg->dash_to_panel_gestionar) {
        d2p_apply_visibility_settings();
        const char *cur_primary = display_get_primary();
        if (cur_primary && *cur_primary)
            d2p_set_primary_monitor(cur_primary);
    }

    /* DMIC — always direct from the tray (PipeWire is a session service). */
    configurar_dmic_raw();

    /* Only one keyboard monitor at a time to avoid state conflicts. */
    if (cfg->modo_deteccion && !strcmp(cfg->modo_deteccion, "bluetooth"))
        pthread_create(&hilo_bluetooth, NULL, monitorizar_cambios_bluetooth, NULL);
    else if (cfg->modo_deteccion && !strcmp(cfg->modo_deteccion, "udev"))
        pthread_create(&hilo_usb, NULL, monitorizar_cambios_teclado_usb, NULL);
    else
        g_warning("modo_deteccion invalido o ausente ('%s'); monitor del "
                  "teclado no iniciado.",
                  cfg->modo_deteccion ? cfg->modo_deteccion : "");

    pthread_create(&hilo_orientacion, NULL, monitorizar_cambios_orientacion, NULL);
    pthread_create(&hilo_drm,         NULL, monitorizar_drm_hotplug,        NULL);

    g_unix_signal_add(SIGTERM, on_shutdown_signal, NULL);
    g_unix_signal_add(SIGINT,  on_shutdown_signal, NULL);
    g_unix_signal_add(SIGHUP,  on_reload_config,   NULL);

    g_main_loop_run(g_loop);

    g_main_loop_unref(g_loop);
    return 0;
}
