#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
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

static AppIndicator *indicator;
static pthread_t hilo_orientacion;
static pthread_t hilo_bluetooth;
static pthread_t hilo_usb;
static pthread_t hilo_drm;

/* Fixed radio items for "Monitor principal" — created once at startup,
 * never destroyed.  The hotplug callback only shows/hides them and updates
 * the active state, so dbusmenu always holds valid widget references. */
static struct {
    const char *name;
    GtkWidget  *item;
} g_primario_items[] = {
    { "HDMI-1", NULL }, { "HDMI-2", NULL },
    { "DP-1",   NULL }, { "DP-2",   NULL }, { "DP-3", NULL },
    { "eDP-1",  NULL },
    { NULL,     NULL }
};
/* Cached at startup: 1 = the system service is running and we
 * should route privileged calls through D-Bus; 0 = no service, fall
 * back to direct calls (only useful when running zbd-tray as root). */
static int system_service_available = 0;

static void on_set_pantalla_brillo(GtkMenuItem *item, gpointer user_data)
{
    if (!gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item)))
        return; /* fired on deselect — ignore */
    int nivel = GPOINTER_TO_INT(user_data);
    if (system_service_available)
    {
        zbd_ipc_client_set_screen_brightness(nivel);
    }
    else
    {
        set_pantalla_brillo(nivel);
    }
    /* Keep cfg in sync so keyboard-attach/detach events restore the right
     * level, and propagate the new brightness to the ScreenPad if active. */
    cfg->pantalla_nivel_brillo = nivel;
    if (monitor_estado("eDP-2"))
    {
        int sp = nivel * 235 / 100;
        if (sp < 10) sp = 10;
        if (system_service_available)
            zbd_ipc_client_set_screenpad_brightness(nivel);
        else
            set_screenpad_brillo(sp);
    }
}


static void on_set_teclado_brillo(GtkMenuItem *item, gpointer user_data)
{
    if (!gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item)))
        return; /* fired on deselect — ignore */
    int nivel = GPOINTER_TO_INT(user_data);
    if (system_service_available)
    {
        zbd_ipc_client_set_keyboard_backlight(nivel);
    }
    else
    {
        set_brillo_teclado(nivel);
    }
    cfg->teclado_nivel_brillo = nivel;
}

static void on_set_primary_monitor(GtkMenuItem *item, gpointer user_data)
{
    if (!gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item)))
        return; /* fired on deselect — ignore */
    const char *output = (const char *)user_data;
    fprintf(stderr, "zbd-tray: monitor principal → %s\n", output ? output : "(auto)");
    display_set_primary(output);
    /* Restore brightness after layout rebuild. */
    set_pantalla_brillo(cfg->pantalla_nivel_brillo);
}

/* Rebuilds screen-brightness radio items on open so the checked entry
 * always reflects the current cfg->pantalla_nivel_brillo. */
static void on_submenu_pantalla_map(GtkWidget *submenu, gpointer user_data)
{
    (void)user_data;

    GList *old = gtk_container_get_children(GTK_CONTAINER(submenu));
    g_list_foreach(old, (GFunc)gtk_widget_destroy, NULL);
    g_list_free(old);

    int current = cfg ? cfg->pantalla_nivel_brillo : 0;
    GSList *group = NULL;

    for (int i = 10; i <= 100; i += 10) {
        char label[8];
        snprintf(label, sizeof(label), "%d%%", i);
        GtkWidget *item = gtk_radio_menu_item_new_with_label(group, label);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
        gtk_widget_show(item);
        if (i == current)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
        g_signal_connect(item, "activate",
                         G_CALLBACK(on_set_pantalla_brillo), GINT_TO_POINTER(i));
    }
}

/* Rebuilds keyboard-brightness radio items on open so the checked entry
 * always reflects the current cfg->teclado_nivel_brillo. */
static void on_submenu_teclado_map(GtkWidget *submenu, gpointer user_data)
{
    (void)user_data;

    GList *old = gtk_container_get_children(GTK_CONTAINER(submenu));
    g_list_foreach(old, (GFunc)gtk_widget_destroy, NULL);
    g_list_free(old);

    int current = cfg ? cfg->teclado_nivel_brillo : 0;
    GSList *group = NULL;

    for (int i = 0; i <= 3; i++) {
        char label[4];
        snprintf(label, sizeof(label), "%d", i);
        GtkWidget *item = gtk_radio_menu_item_new_with_label(group, label);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
        gtk_widget_show(item);
        if (i == current)
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
        g_signal_connect(item, "activate",
                         G_CALLBACK(on_set_teclado_brillo), GINT_TO_POINTER(i));
    }
}

/* Updates the "Monitor principal" submenu in place: shows only connected
 * outputs and marks the current primary as active.  Items are never
 * destroyed — dbusmenu always holds valid widget references. */
static void on_submenu_primario_map(GtkWidget *submenu, gpointer user_data)
{
    (void)submenu;
    (void)user_data;
    const char *current = display_get_primary();
    for (int i = 0; g_primario_items[i].name; i++) {
        const char *name = g_primario_items[i].name;
        GtkWidget  *item = g_primario_items[i].item;
        if (!item) continue;
        /* Show connected outputs; eDP-1 always visible. */
        gboolean visible = (!strcmp(name, "eDP-1")
                         || display_is_output_connected(name));
        if (visible) gtk_widget_show(item); else gtk_widget_hide(item);
        /* Activate the current primary without triggering on_set_primary_monitor. */
        if (current && !strcmp(current, name)
                && !gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item))) {
            g_signal_handlers_block_by_func(
                item, on_set_primary_monitor, (gpointer)name);
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
            g_signal_handlers_unblock_by_func(
                item, on_set_primary_monitor, (gpointer)name);
        }
    }
}

static void on_start_orientacion(void)
{
    pthread_create(&hilo_orientacion, NULL, monitorizar_cambios_orientacion, NULL);
}

static void on_start_bluetooth(void)
{
    pthread_create(&hilo_bluetooth, NULL, monitorizar_cambios_bluetooth, NULL);
}

static void on_start_usb(void)
{
    pthread_create(&hilo_usb, NULL, monitorizar_cambios_teclado_usb, NULL);
}

static void on_quit(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    (void)user_data;
    gtk_main_quit();
}

static gboolean on_shutdown_signal(gpointer user_data)
{
    (void)user_data;
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

/* SIGHUP handler: re-read /etc/zbd/zbd.conf and re-apply all hardware
 * settings. Monitor threads (orientation, USB, bluetooth) keep running
 * across the reload — they re-read cfg fields on their next event.
 *
 * Note: cargar_configuracion() replaces the global cfg pointer. Monitor
 * threads may briefly see a stale pointer; this is benign in practice
 * (single-user device, infrequent reloads, threads poll on long timeouts).
 */
static gboolean on_reload_config(gpointer user_data)
{
    (void)user_data;
    fprintf(stderr, "zbd-tray: SIGHUP — recargando configuracion\n");

    if (cargar_configuracion() < 0)
    {
        fprintf(stderr, "zbd-tray: reload: cargar_configuracion fallo; se mantiene la config anterior\n");
        return G_SOURCE_CONTINUE;
    }

    fprintf(stderr, "zbd-tray: reload: brillo=%d teclado=%d bateria=%d mic=%d%% altavoces=%d%%\n",
            cfg->pantalla_nivel_brillo, cfg->teclado_nivel_brillo,
            cfg->bateria_carga_maxima,
            cfg->audio_volumen_microfono, cfg->audio_volumen_altavoces);

    /* Audio — siempre directo desde la tray (PipeWire es de sesion). */
    configurar_dmic_raw();

    /* Brillo de pantalla. */
    if (system_service_available)
        zbd_ipc_client_set_screen_brightness(cfg->pantalla_nivel_brillo);
    else
        set_pantalla_brillo(cfg->pantalla_nivel_brillo);

    /* ScreenPad: sincronizar si eDP-2 esta activo. */
    if (monitor_estado("eDP-2"))
    {
        int sp = cfg->pantalla_nivel_brillo * 235 / 100;
        if (sp < 10) sp = 10;
        if (system_service_available)
            zbd_ipc_client_set_screenpad_brightness(cfg->pantalla_nivel_brillo);
        else
            set_screenpad_brillo(sp);
    }

    /* Teclado. */
    if (system_service_available)
        zbd_ipc_client_set_keyboard_backlight(cfg->teclado_nivel_brillo);
    else
        set_brillo_teclado(cfg->teclado_nivel_brillo);

    /* Bateria — solo via D-Bus (escribe sysfs privilegiado). */
    if (system_service_available && cfg->bateria_carga_maxima > 0)
        zbd_ipc_client_set_battery_threshold(cfg->bateria_carga_maxima);

    fprintf(stderr, "zbd-tray: reload: completado\n");
    return G_SOURCE_CONTINUE;
}

static guint g_drm_rebuild_source = 0;

static gboolean on_drm_hotplug(gpointer user_data)
{
    (void)user_data;
    g_drm_rebuild_source = 0;
    on_submenu_primario_map(NULL, NULL);
    return G_SOURCE_REMOVE;
}

/* Debounce trampoline — runs in the GLib main loop via g_idle_add.
 * Arms a one-shot 1-second timer; a second call within that window
 * is a no-op so that rapid-fire events collapse into a single rebuild. */
static gboolean drm_schedule_rebuild(gpointer user_data)
{
    (void)user_data;
    if (g_drm_rebuild_source == 0)
        g_drm_rebuild_source = g_timeout_add(1000, on_drm_hotplug, NULL);
    return G_SOURCE_REMOVE;
}

/* Watches udev for DRM connector hotplug events and schedules a debounced
 * menu rebuild on the GTK main thread. */
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
                    /* Rate-limit: the Xe/DRM driver can burst dozens of
                     * "change" events on a single connect/disconnect.
                     * Cap g_idle_add calls to one per 500 ms to avoid
                     * flooding the GLib main loop with idle sources. */
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

static GtkWidget *create_menu(void)
{
    GtkWidget *menu, *item;
    menu = gtk_menu_new();

    /* DMIC: siempre directamente desde la tray. pactl habla con el
     * servidor PipeWire del usuario via XDG_RUNTIME_DIR; el servicio
     * system corre en un mount namespace distinto y no ve /run/user/0.
     * Si PipeWire aun no esta listo, configurar_dmic_raw() falla con
     * un aviso pero no aborta. */
    configurar_dmic_raw();
    on_start_orientacion();

    /* Solo un monitor del teclado a la vez: Bluetooth o USB, segun la
     * configuracion. Lanzar ambos provoca que se pisen al cambiar el
     * estado de eDP-2. */
    if (cfg->modo_deteccion && !strcmp(cfg->modo_deteccion, "bluetooth"))
    {
        on_start_bluetooth();
    }
    else if (cfg->modo_deteccion && !strcmp(cfg->modo_deteccion, "udev"))
    {
        on_start_usb();
    }
    else
    {
        g_warning("modo_deteccion invalido o ausente ('%s'); el monitor "
                  "del teclado no se inicia.",
                  cfg->modo_deteccion ? cfg->modo_deteccion : "");
    }

    // Submenú brillo de pantalla (eDP-1): rango válido 10-100
    // Pre-populated at creation so GTK sees items at show_all time (empty
    // submenus are treated as leaf nodes and close the menu on click).
    // map signal refreshes the checked entry each time the submenu opens.
    GtkWidget *submenu_pantalla = gtk_menu_new();
    on_submenu_pantalla_map(submenu_pantalla, NULL);
    g_signal_connect(submenu_pantalla, "map",
                     G_CALLBACK(on_submenu_pantalla_map), NULL);
    GtkWidget *pantalla_menu = gtk_menu_item_new_with_label("Brillo pantalla");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(pantalla_menu), submenu_pantalla);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), pantalla_menu);

    // Submenú brillo de teclado (0-3) — same pre-populate + map pattern.
    GtkWidget *submenu_teclado = gtk_menu_new();
    on_submenu_teclado_map(submenu_teclado, NULL);
    g_signal_connect(submenu_teclado, "map",
                     G_CALLBACK(on_submenu_teclado_map), NULL);
    GtkWidget *teclado_menu = gtk_menu_item_new_with_label("Brillo teclado");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(teclado_menu), submenu_teclado);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), teclado_menu);

    // Submenú monitor principal — ítems fijos creados una sola vez.
    // La señal map no refire con AppIndicator/dbusmenu; el hilo DRM llama
    // on_submenu_primario_map para actualizar visibilidad y estado active.
    GtkWidget *submenu_primario = gtk_menu_new();
    {
        GSList *grp = NULL;
        for (int i = 0; g_primario_items[i].name; i++) {
            GtkWidget *it = gtk_radio_menu_item_new_with_label(
                                grp, g_primario_items[i].name);
            g_primario_items[i].item = it;
            grp = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(it));
            /* Prevent gtk_widget_show_all from overriding hide() calls. */
            gtk_widget_set_no_show_all(it, TRUE);
            gtk_menu_shell_append(GTK_MENU_SHELL(submenu_primario), it);
            g_signal_connect(it, "activate",
                             G_CALLBACK(on_set_primary_monitor),
                             (gpointer)g_primario_items[i].name);
        }
    }
    on_submenu_primario_map(submenu_primario, NULL);
    GtkWidget *primario_menu = gtk_menu_item_new_with_label("Monitor principal");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(primario_menu), submenu_primario);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), primario_menu);

    item = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Salir");
    g_signal_connect(item, "activate", G_CALLBACK(on_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    gtk_widget_show_all(menu);
    return menu;
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);

    if (zbd_install_signal_handlers() != 0)
    {
        return 1;
    }

    if (cargar_configuracion() < 0) {
        return 1;
    }

    if (display_init() < 0) {
        fprintf(stderr, "display: ningun backend disponible; abortando.\n");
        return 1;
    }

    /* Detectar si el servicio system D-Bus esta corriendo. Si lo esta,
     * la tray rutea las operaciones privilegiadas a traves de D-Bus.
     * Si no, ejecuta directamente (solo util cuando zbd-tray se lanza
     * como root para depuracion). */
    system_service_available = zbd_ipc_client_is_service_available();
    fprintf(stderr, "ipc: zbd-system %s en el bus\n",
            system_service_available ? "presente" : "ausente; usando llamadas directas");

    /* Icon registered in /usr/share/icons/hicolor/scalable/apps/ by make
     * install. GNOME Shell resolves it by name via the hicolor theme, which
     * is the correct freedesktop approach. Using an absolute path here would
     * fail with GNOME Shell's AppIndicator extension because the shell looks
     * up icons by name in the theme, not from arbitrary file paths. */
    const gchar *icon_name = "zbd-tray";

    indicator = app_indicator_new("zbd-indicator", icon_name, APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_icon_full(indicator, icon_name, "ZBD Tray");

    GtkWidget *menu = create_menu();
    app_indicator_set_menu(indicator, GTK_MENU(menu));
    pthread_create(&hilo_drm, NULL, monitorizar_drm_hotplug, NULL);

    g_unix_signal_add(SIGTERM, on_shutdown_signal, NULL);
    g_unix_signal_add(SIGINT,  on_shutdown_signal, NULL);
    g_unix_signal_add(SIGHUP,  on_reload_config,   NULL);

    gtk_main();
    return 0;
}