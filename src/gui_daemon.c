#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include <pthread.h>
#include "comun.h"
#include "pantalla.h"
#include "teclado.h"
#include "monitor_bluetooth.h"
#include "monitor_orientacion.h"
#include "monitor_teclado_usb.h"

static AppIndicator *indicator;
static pthread_t hilo_orientacion;
static pthread_t hilo_bluetooth;
static pthread_t hilo_usb;

static void on_start_orientacion(GtkMenuItem *item, gpointer user_data)
{
    pthread_create(&hilo_orientacion, NULL, monitorizar_cambios_orientacion, NULL);
}

static void on_start_bluetooth(GtkMenuItem *item, gpointer user_data)
{
    pthread_create(&hilo_bluetooth, NULL, monitorizar_cambios_bluetooth, NULL);
}

static void on_start_usb(GtkMenuItem *item, gpointer user_data)
{
    pthread_create(&hilo_usb, NULL, monitorizar_cambios_teclado_usb, NULL);
}

static void on_encender_edp2(GtkMenuItem *item, gpointer user_data)
{
    configurar_monitores("encender");
    poner_fondo_2_monitores();
}

static void on_apagar_edp2(GtkMenuItem *item, gpointer user_data)
{
    configurar_monitores("apagar");
    poner_fondo_1_monitor();
}

static void on_quit(GtkMenuItem *item, gpointer user_data)
{
    gtk_main_quit();
}

static GtkWidget* create_menu()
{
    GtkWidget *menu, *item;
    menu = gtk_menu_new();

    item = gtk_menu_item_new_with_label("Monitorizar orientación");
    g_signal_connect(item, "activate", G_CALLBACK(on_start_orientacion), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Monitorizar bluetooth");
    g_signal_connect(item, "activate", G_CALLBACK(on_start_bluetooth), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Monitorizar teclado USB");
    g_signal_connect(item, "activate", G_CALLBACK(on_start_usb), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Encender eDP-2");
    g_signal_connect(item, "activate", G_CALLBACK(on_encender_edp2), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);

    item = gtk_menu_item_new_with_label("Apagar eDP-2");
    g_signal_connect(item, "activate", G_CALLBACK(on_apagar_edp2), NULL);
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

    if (cargar_configuracion() < 0) {
        return 1;
    }

    indicator = app_indicator_new("zbd-indicator", "computer", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_attention_icon(indicator, "computer");

    GtkWidget *menu = create_menu();
    app_indicator_set_menu(indicator, GTK_MENU(menu));

    gtk_main();
    return 0;
}