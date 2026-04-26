#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <glib-2.0/gio/gio.h>
#include <glib-2.0/glib-unix.h>

#include "comun.h"
#include "monitor_orientacion.h"
#include "runtime.h"
#include "display.h"

/*
 * Map iio-sensor-proxy orientation strings to the rotation enum the
 * display backend understands. iio-sensor-proxy emits one of:
 *   "normal"    — top of the device is up.
 *   "left-up"   — device rotated 90° clockwise (left edge up).
 *   "right-up"  — device rotated 90° anti-clockwise (right edge up).
 *   "bottom-up" — device upside down.
 *
 * We rotate eDP-1 by default; the second panel (eDP-2), when on,
 * follows. The rotation strings come from the AccelerometerOrientation
 * property of net.hadess.SensorProxy.
 */
static int orientation_to_rotation(const char *o, display_rotation *out)
{
    if (!o || !out) return -1;
    if (!strcmp(o, "normal"))    { *out = DISPLAY_ROTATION_NORMAL;    return 0; }
    if (!strcmp(o, "left-up"))   { *out = DISPLAY_ROTATION_LEFT_UP;   return 0; }
    if (!strcmp(o, "right-up"))  { *out = DISPLAY_ROTATION_RIGHT_UP;  return 0; }
    if (!strcmp(o, "bottom-up")) { *out = DISPLAY_ROTATION_BOTTOM_UP; return 0; }
    return -1;
}

/*
 * Apply `o` to the active outputs. Always rotates eDP-1; if eDP-2
 * is currently enabled, rotates it too. Errors are logged but not
 * propagated — a transient failure should not kill the monitor.
 */
static void apply_orientation(const char *o)
{
    display_rotation r;
    if (orientation_to_rotation(o, &r) != 0)
    {
        fprintf(stderr, "monitor_orientacion: orientacion desconocida '%s'\n",
                o ? o : "(null)");
        return;
    }
    if (display_set_rotation("eDP-1", r) != 0)
    {
        fprintf(stderr, "monitor_orientacion: rotacion de eDP-1 a '%s' fallo\n", o);
    }
    if (display_is_output_on("eDP-2"))
    {
        if (display_set_rotation("eDP-2", r) != 0)
        {
            fprintf(stderr, "monitor_orientacion: rotacion de eDP-2 a '%s' fallo\n", o);
        }
    }
}

/* Callback para SIGTERM/SIGINT registrado via g_unix_signal_add: hace
 * salir el GMainLoop limpiamente para que el hilo pueda hacer cleanup. */
static gboolean on_shutdown_signal_glib(gpointer user_data)
{
    GMainLoop *loop = (GMainLoop *)user_data;
    zbd_shutdown_requested = 1;
    if (loop) g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

// Variable global para almacenar la orientación actual
char current_orientation[32] = "Unknown";

// Callback para manejar los cambios de propiedad
static void on_property_changed(
    GDBusProxy *proxy,
    GVariant *changed_properties,
    GStrv invalidated_properties,
    gpointer user_data) {
    (void)proxy;
    (void)invalidated_properties;
    (void)user_data;
    GVariantIter iter;
    const gchar *key;
    GVariant *value;

    g_variant_iter_init(&iter, changed_properties);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
        if (g_strcmp0(key, "AccelerometerOrientation") == 0) {
            const gchar *orientation = g_variant_get_string(value, NULL);
            snprintf(current_orientation, sizeof(current_orientation), "%s", orientation);
            printf("Orientation changed: %s\n", current_orientation);
            apply_orientation(current_orientation);
        }
        g_variant_unref(value);
    }
}

void *monitorizar_cambios_orientacion(void *arg) {
    (void)arg;
    GError *error = NULL;
    GDBusProxy *proxy = NULL;

    // Crear el proxy para el objeto D-Bus
    proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        cfg->orientacion_bus,
        cfg->orientacion_path,
        cfg->orientacion_interfaz,
        NULL,
        &error);

    if (!proxy) {
        fprintf(stderr, "Error creating proxy: %s\n", error->message);
        g_error_free(error);
        return NULL;
    }

    // Verificar si el sensor está habilitado
    GVariant *enabled = g_dbus_proxy_get_cached_property(proxy, "HasAccelerometer");
    if (!enabled || !g_variant_get_boolean(enabled)) {
        fprintf(stderr, "Accelerometer not available.\n");
        g_clear_object(&proxy);
        return NULL;
    }
    g_variant_unref(enabled);

    // Registrar la orientación inicial
    GVariant *initial_orientation = g_dbus_proxy_get_cached_property(proxy, "AccelerometerOrientation");
    if (initial_orientation) {
        const gchar *orientation = g_variant_get_string(initial_orientation, NULL);
        snprintf(current_orientation, sizeof(current_orientation), "%s", orientation);
        printf("Initial orientation: %s\n", current_orientation);
        apply_orientation(current_orientation);
        g_variant_unref(initial_orientation);
    } else {
        printf("Unable to get initial orientation.\n");
    }

    // Habilitar el sensor
    g_dbus_proxy_call_sync(proxy, "ClaimAccelerometer", NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (error) {
        fprintf(stderr, "Error enabling accelerometer: %s\n", error->message);
        g_error_free(error);
        g_clear_object(&proxy);
        return NULL;
    }

    // Conectar al evento de cambios de propiedad
    g_signal_connect(proxy, "g-properties-changed", G_CALLBACK(on_property_changed), NULL);

    printf("Listening for accelerometer orientation changes...\n");

    // Main loop para mantener el hilo corriendo. Se registran fuentes
    // de SIGTERM/SIGINT para que un Ctrl-C o un systemctl stop saquen
    // del loop limpiamente.
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, on_shutdown_signal_glib, loop);
    g_unix_signal_add(SIGINT,  on_shutdown_signal_glib, loop);
    g_unix_signal_add(SIGHUP,  on_shutdown_signal_glib, loop);
    g_main_loop_run(loop);

    // Cleanup
    g_main_loop_unref(loop);
    g_clear_object(&proxy);

    return NULL;
}
