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
 * Map iio-sensor-proxy orientation strings to the rotation enum.
 * iio-sensor-proxy emits one of: "normal", "left-up", "right-up",
 * "bottom-up".
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
 * Apply orientation to the active eDP outputs.
 *
 * The gdctl backend handles both eDP-1 and eDP-2 in a single atomic
 * topology rebuild, so calling display_set_rotation("eDP-1", r) is
 * sufficient — eDP-2 is included automatically if it is currently on.
 *
 * We still call for eDP-2 explicitly so the xrandr backend (X11 path)
 * continues to work correctly, as xrandr processes each output
 * independently.
 *
 * Errors are logged but not propagated: a transient failure should not
 * kill the monitor thread.
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
        fprintf(stderr, "monitor_orientacion: rotacion de eDP-1 a '%s' fallo\n", o);

    /* xrandr backend: each output rotated individually.
     * gdctl backend: eDP-2 already included in the eDP-1 call above;
     * this call rebuilds the same topology — harmless but necessary
     * for xrandr compatibility. */
    if (display_is_output_on("eDP-2"))
    {
        if (display_set_rotation("eDP-2", r) != 0)
            fprintf(stderr, "monitor_orientacion: rotacion de eDP-2 a '%s' fallo\n", o);
    }
}

static gboolean on_shutdown_signal_glib(gpointer user_data)
{
    GMainLoop *loop = (GMainLoop *)user_data;
    zbd_shutdown_requested = 1;
    if (loop) g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static void on_property_changed(
    GDBusProxy *proxy,
    GVariant   *changed_properties,
    GStrv       invalidated_properties,
    gpointer    user_data)
{
    (void)proxy;
    (void)invalidated_properties;
    (void)user_data;

    GVariantIter iter;
    const gchar *key;
    GVariant *value;

    g_variant_iter_init(&iter, changed_properties);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &value))
    {
        if (g_strcmp0(key, "AccelerometerOrientation") == 0)
        {
            const gchar *orientation = g_variant_get_string(value, NULL);
            fprintf(stderr, "monitor_orientacion: orientacion → %s\n", orientation);
            apply_orientation(orientation);
        }
        g_variant_unref(value);
    }
}

void *monitorizar_cambios_orientacion(void *arg)
{
    (void)arg;
    GError *error = NULL;

    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        cfg->orientacion_bus,
        cfg->orientacion_path,
        cfg->orientacion_interfaz,
        NULL,
        &error);

    if (!proxy)
    {
        fprintf(stderr, "monitor_orientacion: no se pudo conectar a %s: %s\n",
                cfg->orientacion_bus, error ? error->message : "error desconocido");
        if (error) g_error_free(error);
        return NULL;
    }

    /* If iio-sensor-proxy is running but reports no accelerometer
     * (e.g. HW absent or not claimed yet), exit cleanly. */
    GVariant *has_accel = g_dbus_proxy_get_cached_property(proxy, "HasAccelerometer");
    if (!has_accel || !g_variant_get_boolean(has_accel))
    {
        fprintf(stderr, "monitor_orientacion: acelerometro no disponible — hilo terminado\n");
        if (has_accel) g_variant_unref(has_accel);
        g_clear_object(&proxy);
        return NULL;
    }
    g_variant_unref(has_accel);

    /* Apply the current orientation immediately so the display starts
     * in the right state rather than waiting for the first change. */
    GVariant *initial = g_dbus_proxy_get_cached_property(proxy, "AccelerometerOrientation");
    if (initial)
    {
        const gchar *o = g_variant_get_string(initial, NULL);
        fprintf(stderr, "monitor_orientacion: orientacion inicial → %s\n", o);
        apply_orientation(o);
        g_variant_unref(initial);
    }
    else
    {
        fprintf(stderr, "monitor_orientacion: no se pudo leer orientacion inicial\n");
    }

    /* Claim the sensor so iio-sensor-proxy keeps it active. */
    g_dbus_proxy_call_sync(proxy, "ClaimAccelerometer", NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (error)
    {
        fprintf(stderr, "monitor_orientacion: ClaimAccelerometer fallo: %s\n",
                error->message);
        g_error_free(error);
        g_clear_object(&proxy);
        return NULL;
    }

    g_signal_connect(proxy, "g-properties-changed",
                     G_CALLBACK(on_property_changed), NULL);

    fprintf(stderr, "monitor_orientacion: escuchando cambios de orientacion\n");

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, on_shutdown_signal_glib, loop);
    g_unix_signal_add(SIGINT,  on_shutdown_signal_glib, loop);
    /* SIGHUP is handled by the tray main thread as "reload config";
     * this monitor thread must keep running across reloads. */
    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    g_clear_object(&proxy);
    return NULL;
}
