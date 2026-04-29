/*
 * gui_daemon.c — zbd-tray: tray icon, monitor threads and config reload.
 *
 * Uses libayatana-appindicator-glib (GLib-only reimplementation of the
 * StatusNotifierItem/AppIndicator protocol) and GIO GMenu/GSimpleAction
 * instead of GTK3 GtkMenu widgets.  No GTK dependency — pure GLib/GIO.
 *
 * Menu model
 * ──────────
 * The tray menu is a static GMenu tree wired to a GSimpleActionGroup
 * (prefix "ind").  Radio-button behaviour is achieved via stateful
 * GSimpleActions with G_VARIANT_TYPE_STRING: each menu item carries an
 * action-detail like "ind.brillo-pantalla::20"; when activated the action
 * receives the target string as its parameter and sets its state to that
 * value — the tray renderer marks the matching item as checked.
 *
 * Thread safety
 * ─────────────
 * All GMenu/GAction mutations happen on the GLib main thread via
 * g_idle_add() / g_timeout_add() callbacks.  The monitor pthreads only
 * call g_idle_add() to schedule work; they never touch GMenu directly.
 */

#include <libayatana-appindicator-glib/ayatana-appindicator.h>
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

/* ---- module state --------------------------------------------------- */

static AppIndicator       *indicator;
static GMainLoop          *g_loop;
static GSimpleActionGroup *g_actions;
static GMenu              *g_menu;
static GMenu              *g_submenu_primario;
static GSimpleAction      *g_act_pantalla;
static GSimpleAction      *g_act_teclado;
static GSimpleAction      *g_act_primary;

static pthread_t hilo_orientacion;
static pthread_t hilo_bluetooth;
static pthread_t hilo_usb;
static pthread_t hilo_drm;

static int system_service_available = 0;

/* ---- action callbacks ----------------------------------------------- */

static void on_set_pantalla_brillo(GSimpleAction *act, GVariant *param,
                                   gpointer data)
{
    (void)data;
    g_simple_action_set_state(act, param);
    int nivel = atoi(g_variant_get_string(param, NULL));
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

static void on_set_teclado_brillo(GSimpleAction *act, GVariant *param,
                                  gpointer data)
{
    (void)data;
    g_simple_action_set_state(act, param);
    int nivel = atoi(g_variant_get_string(param, NULL));
    if (system_service_available)
        zbd_ipc_client_set_keyboard_backlight(nivel);
    else
        set_brillo_teclado(nivel);
    cfg->teclado_nivel_brillo = nivel;
}

static void on_set_primary_monitor(GSimpleAction *act, GVariant *param,
                                   gpointer data)
{
    (void)data;
    g_simple_action_set_state(act, param);
    const char *output = g_variant_get_string(param, NULL);
    fprintf(stderr, "zbd-tray: monitor principal → %s\n", output);
    display_set_primary(output);
    set_pantalla_brillo(cfg->pantalla_nivel_brillo);
}

static void on_quit(GSimpleAction *act, GVariant *param, gpointer data)
{
    (void)act; (void)param; (void)data;
    g_main_loop_quit(g_loop);
}

/* ---- primary-monitor submenu (rebuilt on DRM hotplug) --------------- */

/*
 * Rebuild g_submenu_primario to show only physically-connected outputs
 * (eDP-1 always visible) and update the checked radio state.
 * Must be called from the main thread.
 */
static void rebuild_primary_submenu(void)
{
    static const char *candidates[] = {
        "HDMI-1", "HDMI-2", "DP-1", "DP-2", "DP-3", NULL
    };

    g_menu_remove_all(g_submenu_primario);

    for (int i = 0; candidates[i]; i++) {
        if (display_is_output_connected(candidates[i])) {
            char action[64];
            snprintf(action, sizeof(action), "ind.set-primary::%s", candidates[i]);
            g_menu_append(g_submenu_primario, candidates[i], action);
        }
    }
    g_menu_append(g_submenu_primario, "eDP-1", "ind.set-primary::eDP-1");

    if (g_act_primary) {
        const char *cur = display_get_primary();
        if (!cur) cur = "eDP-1";
        g_simple_action_set_state(g_act_primary, g_variant_new_string(cur));
    }
}

/* ---- signal / reload callbacks -------------------------------------- */

static gboolean on_shutdown_signal(gpointer data)
{
    (void)data;
    g_main_loop_quit(g_loop);
    return G_SOURCE_REMOVE;
}

/*
 * SIGHUP: re-read /etc/zbd/zbd.conf and re-apply all hardware settings.
 * Monitor threads keep running across the reload; they re-read cfg on
 * their next event.
 */
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

    /* Sync radio-button checked state to the newly loaded values. */
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", cfg->pantalla_nivel_brillo);
    g_simple_action_set_state(g_act_pantalla, g_variant_new_string(buf));
    snprintf(buf, sizeof(buf), "%d", cfg->teclado_nivel_brillo);
    g_simple_action_set_state(g_act_teclado, g_variant_new_string(buf));

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

/*
 * Watches udev for DRM connector hotplug events and schedules a debounced
 * menu rebuild on the GLib main thread.
 */
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

/* ---- GAction setup -------------------------------------------------- */

static void setup_actions(void)
{
    g_actions = g_simple_action_group_new();

    /* Brillo pantalla: stateful string action (values "10".."100"). */
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", cfg ? cfg->pantalla_nivel_brillo : 20);
    g_act_pantalla = g_simple_action_new_stateful(
        "brillo-pantalla", G_VARIANT_TYPE_STRING, g_variant_new_string(buf));
    g_signal_connect(g_act_pantalla, "activate",
                     G_CALLBACK(on_set_pantalla_brillo), NULL);
    g_simple_action_group_insert(g_actions, G_ACTION(g_act_pantalla));

    /* Brillo teclado: stateful string action (values "0".."3"). */
    snprintf(buf, sizeof(buf), "%d", cfg ? cfg->teclado_nivel_brillo : 1);
    g_act_teclado = g_simple_action_new_stateful(
        "brillo-teclado", G_VARIANT_TYPE_STRING, g_variant_new_string(buf));
    g_signal_connect(g_act_teclado, "activate",
                     G_CALLBACK(on_set_teclado_brillo), NULL);
    g_simple_action_group_insert(g_actions, G_ACTION(g_act_teclado));

    /* Monitor principal: stateful string action (connector names). */
    const char *prim = display_get_primary();
    if (!prim) prim = "eDP-1";
    g_act_primary = g_simple_action_new_stateful(
        "set-primary", G_VARIANT_TYPE_STRING, g_variant_new_string(prim));
    g_signal_connect(g_act_primary, "activate",
                     G_CALLBACK(on_set_primary_monitor), NULL);
    g_simple_action_group_insert(g_actions, G_ACTION(g_act_primary));

    /* Salir: simple non-stateful action. */
    GSimpleAction *quit = g_simple_action_new("quit", NULL);
    g_signal_connect(quit, "activate", G_CALLBACK(on_quit), NULL);
    g_simple_action_group_insert(g_actions, G_ACTION(quit));
    g_object_unref(quit);
}

/* ---- GMenu construction --------------------------------------------- */

static void build_menu(void)
{
    g_menu = g_menu_new();

    /* Brillo pantalla: 10%..100% radio group. */
    GMenu *sub_p = g_menu_new();
    for (int i = 10; i <= 100; i += 10) {
        char label[8], action[48];
        snprintf(label,  sizeof(label),  "%d%%", i);
        snprintf(action, sizeof(action), "ind.brillo-pantalla::%d", i);
        g_menu_append(sub_p, label, action);
    }
    g_menu_append_submenu(g_menu, "Brillo pantalla", G_MENU_MODEL(sub_p));
    g_object_unref(sub_p);

    /* Brillo teclado: 0..3 radio group. */
    GMenu *sub_t = g_menu_new();
    for (int i = 0; i <= 3; i++) {
        char label[4], action[40];
        snprintf(label,  sizeof(label),  "%d", i);
        snprintf(action, sizeof(action), "ind.brillo-teclado::%d", i);
        g_menu_append(sub_t, label, action);
    }
    g_menu_append_submenu(g_menu, "Brillo teclado", G_MENU_MODEL(sub_t));
    g_object_unref(sub_t);

    /* Monitor principal: dynamic items populated by rebuild_primary_submenu(). */
    g_submenu_primario = g_menu_new();
    rebuild_primary_submenu();
    g_menu_append_submenu(g_menu, "Monitor principal",
                          G_MENU_MODEL(g_submenu_primario));

    /* Separator + Salir in their own section. */
    GMenu *sec = g_menu_new();
    g_menu_append(sec, "Salir", "ind.quit");
    g_menu_append_section(g_menu, NULL, G_MENU_MODEL(sec));
    g_object_unref(sec);
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

    setup_actions();
    build_menu();

    /* Icon registered in /usr/share/icons/hicolor/scalable/apps/ by make
     * install. The tray extension resolves it by name from the hicolor theme. */
    indicator = app_indicator_new("zbd-indicator", "zbd-tray",
                                  APP_INDICATOR_CATEGORY_HARDWARE);
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_icon(indicator, "zbd-tray", "ZBD Tray");
    app_indicator_set_menu(indicator, g_menu);
    app_indicator_set_actions(indicator, g_actions);

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
