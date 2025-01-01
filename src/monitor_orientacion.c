#include <stdio.h>
#include <stdlib.h>
#include <glib-2.0/gio/gio.h>

#include "comun.h"
#include "monitor_orientacion.h"

// Variable global para almacenar la orientación actual
char current_orientation[32] = "Unknown";

// Callback para manejar los cambios de propiedad
static void on_property_changed(
    GDBusProxy *proxy,
    GVariant *changed_properties,
    GStrv invalidated_properties,
    gpointer user_data) {
    GVariantIter iter;
    const gchar *key;
    GVariant *value;

    g_variant_iter_init(&iter, changed_properties);
    while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
        if (g_strcmp0(key, "AccelerometerOrientation") == 0) {
            const gchar *orientation = g_variant_get_string(value, NULL);
            snprintf(current_orientation, sizeof(current_orientation), "%s", orientation);
            printf("Orientation changed: %s\n", current_orientation);
        }
        g_variant_unref(value);
    }
}

void *monitorizar_cambios_orientacion(void *arg) {
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
        return EXIT_FAILURE;
    }

    // Verificar si el sensor está habilitado
    GVariant *enabled = g_dbus_proxy_get_cached_property(proxy, "HasAccelerometer");
    if (!enabled || !g_variant_get_boolean(enabled)) {
        fprintf(stderr, "Accelerometer not available.\n");
        g_clear_object(&proxy);
        return EXIT_FAILURE;
    }
    g_variant_unref(enabled);

    // Registrar la orientación inicial
    GVariant *initial_orientation = g_dbus_proxy_get_cached_property(proxy, "AccelerometerOrientation");
    if (initial_orientation) {
        const gchar *orientation = g_variant_get_string(initial_orientation, NULL);
        snprintf(current_orientation, sizeof(current_orientation), "%s", orientation);
        printf("Initial orientation: %s\n", current_orientation);
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
        return EXIT_FAILURE;
    }

    // Conectar al evento de cambios de propiedad
    g_signal_connect(proxy, "g-properties-changed", G_CALLBACK(on_property_changed), NULL);

    printf("Listening for accelerometer orientation changes...\n");

    // Main loop para mantener el programa corriendo
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    // Cleanup
    g_main_loop_unref(loop);
    g_clear_object(&proxy);

    return EXIT_SUCCESS;
}
