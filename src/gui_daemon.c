#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include <pthread.h>
#include "comun.h"
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
/* Cached at startup: 1 = the system service is running and we
 * should route privileged calls through D-Bus; 0 = no service, fall
 * back to direct calls (only useful when running zbd-tray as root). */
static int system_service_available = 0;

static void on_set_pantalla_brillo(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    int nivel = GPOINTER_TO_INT(user_data);
    if (system_service_available)
    {
        zbd_ipc_client_set_screen_brightness(nivel);
    }
    else
    {
        set_pantalla_brillo(nivel);
    }
}

static void on_set_teclado_brillo(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    int nivel = GPOINTER_TO_INT(user_data);
    if (system_service_available)
    {
        zbd_ipc_client_set_keyboard_backlight(nivel);
    }
    else
    {
        set_brillo_teclado(nivel);
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

static GtkWidget *create_menu(void)
{
    GtkWidget *menu, *item;
    menu = gtk_menu_new();

    /* DMIC: si el servicio root esta presente, lo enrutamos por D-Bus
     * (el servicio se encarga de logear bien si PA aun no esta listo);
     * si no, lo hacemos directos desde la sesion de la tray. */
    if (system_service_available)
    {
        zbd_ipc_client_configure_dmic();
    }
    else
    {
        configurar_dmic_raw();
    }
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

    // Submenú brillo de pantalla
    GtkWidget *submenu_pantalla = gtk_menu_new();
    for (int i = 0; i <= 100; i += 10) {
        char label[8];
        snprintf(label, sizeof(label), "%d%%", i);
        GtkWidget *br_item = gtk_menu_item_new_with_label(label);
        g_signal_connect(br_item, "activate", G_CALLBACK(on_set_pantalla_brillo), GINT_TO_POINTER(i));
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu_pantalla), br_item);
    }
    GtkWidget *pantalla_menu = gtk_menu_item_new_with_label("Brillo pantalla");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(pantalla_menu), submenu_pantalla);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), pantalla_menu);

    // Submenú brillo de teclado
    GtkWidget *submenu_teclado = gtk_menu_new();
    for (int i = 0; i <= 3; i++) {
        char label[4];
        snprintf(label, sizeof(label), "%d", i);
        GtkWidget *br_item = gtk_menu_item_new_with_label(label);
        g_signal_connect(br_item, "activate", G_CALLBACK(on_set_teclado_brillo), GINT_TO_POINTER(i));
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu_teclado), br_item);
    }
    GtkWidget *teclado_menu = gtk_menu_item_new_with_label("Brillo teclado");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(teclado_menu), submenu_teclado);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), teclado_menu);

    item = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

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

    const gchar *icon_path = "/usr/share/icons/zbd/zbd-tray.svg";

    indicator = app_indicator_new("zbd-indicator", icon_path, APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_icon_full(indicator, icon_path, "ZBD Tray");

    GtkWidget *menu = create_menu();
    app_indicator_set_menu(indicator, GTK_MENU(menu));

    gtk_main();
    return 0;
}