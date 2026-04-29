/*
 * ipc_server.c — sd-bus server hosted by zbd-system.
 *
 * Opens the system D-Bus bus, registers the
 * org.anexa.zbd1.System vtable on /org/anexa/zbd1/System, requests
 * the org.anexa.zbd1 name, and runs an sd-bus event loop until the
 * runtime shutdown flag is set or an error happens.
 *
 * Each method handler delegates to the existing implementation
 * (set_pantalla_brillo, set_brillo_teclado, limitar_carga_bateria,
 * configurar_dmic_raw) so the same code path that the CLI exposes
 * continues to be the single source of truth for the privileged
 * operations.
 *
 * Authorization
 *   - The dbus/org.anexa.zbd.conf system policy already restricts
 *     who can talk to this name (root + group `zbd`).
 *   - polkit/org.anexa.zbd.policy declares the per-action policy.
 *     Future work: add explicit polkit::CheckAuthorization calls
 *     here so the policy is enforced even if a privileged peer
 *     bypasses the bus policy. For now we trust the bus-level
 *     gate.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

#include "ipc.h"
#include "audio.h"
#include "config.h"
#include "pantalla.h"
#include "teclado.h"
#include "runtime.h"

/* ---- method handlers --------------------------------------------- */

static int method_set_screen_brightness(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata;
    int level = 0;
    int r = sd_bus_message_read(m, "i", &level);
    if (r < 0)
    {
        return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                                 "argumento invalido para SetScreenBrightness");
    }
    if (set_pantalla_brillo(level) != EXIT_SUCCESS)
    {
        return sd_bus_error_setf(err, SD_BUS_ERROR_FAILED,
                                 "set_pantalla_brillo fallo (level=%d)", level);
    }
    return sd_bus_reply_method_return(m, NULL);
}

static int method_set_keyboard_backlight(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata;
    int level = 0;
    int r = sd_bus_message_read(m, "i", &level);
    if (r < 0)
    {
        return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                                 "argumento invalido para SetKeyboardBacklight");
    }
    if (set_brillo_teclado(level) != EXIT_SUCCESS)
    {
        return sd_bus_error_setf(err, SD_BUS_ERROR_FAILED,
                                 "set_brillo_teclado fallo (level=%d)", level);
    }
    return sd_bus_reply_method_return(m, NULL);
}

static int method_set_battery_threshold(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata;
    int level = 0;
    int r = sd_bus_message_read(m, "i", &level);
    if (r < 0)
    {
        return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                                 "argumento invalido para SetBatteryThreshold");
    }
    if (limitar_carga_bateria(level) != EXIT_SUCCESS)
    {
        return sd_bus_error_setf(err, SD_BUS_ERROR_FAILED,
                                 "limitar_carga_bateria fallo (level=%d)", level);
    }
    return sd_bus_reply_method_return(m, NULL);
}

static int method_set_screenpad_brightness(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata;
    int level = 0;
    int r = sd_bus_message_read(m, "i", &level);
    if (r < 0)
    {
        return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                                 "argumento invalido para SetScreenpadBrightness");
    }
    if (level < 0 || level > 100)
    {
        return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                                 "SetScreenpadBrightness: level %d fuera de rango [0,100]", level);
    }
    int raw = level * 235 / 100;
    if (set_screenpad_brillo(raw) != 0)
    {
        return sd_bus_error_setf(err, SD_BUS_ERROR_FAILED,
                                 "set_screenpad_brillo fallo (level=%d raw=%d)", level, raw);
    }
    return sd_bus_reply_method_return(m, NULL);
}

static int method_configure_dmic(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
    (void)userdata;
    if (configurar_dmic_raw() != EXIT_SUCCESS)
    {
        return sd_bus_error_setf(err, SD_BUS_ERROR_FAILED,
                                 "configurar_dmic_raw fallo "
                                 "(probablemente PulseAudio/PipeWire no esta listo aun)");
    }
    return sd_bus_reply_method_return(m, NULL);
}

static const sd_bus_vtable system_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("SetScreenBrightness",   "i", "", method_set_screen_brightness,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetKeyboardBacklight",  "i", "", method_set_keyboard_backlight,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetBatteryThreshold",    "i", "", method_set_battery_threshold,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetScreenpadBrightness","i", "", method_set_screenpad_brightness,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ConfigureDmic",          "", "", method_configure_dmic,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

/* ---- event loop -------------------------------------------------- */

int zbd_ipc_server_run(void)
{
    sd_bus *bus = NULL;
    int rc;

    rc = sd_bus_open_system(&bus);
    if (rc < 0)
    {
        fprintf(stderr, "ipc-server: sd_bus_open_system: %s\n", strerror(-rc));
        return -1;
    }

    rc = sd_bus_add_object_vtable(bus, NULL,
                                  ZBD_DBUS_OBJECT_PATH,
                                  ZBD_DBUS_INTERFACE,
                                  system_vtable, NULL);
    if (rc < 0)
    {
        fprintf(stderr, "ipc-server: add_object_vtable: %s\n", strerror(-rc));
        sd_bus_unref(bus);
        return -1;
    }

    rc = sd_bus_request_name(bus, ZBD_DBUS_BUS_NAME, 0);
    if (rc < 0)
    {
        fprintf(stderr, "ipc-server: request_name(%s): %s\n",
                ZBD_DBUS_BUS_NAME, strerror(-rc));
        sd_bus_unref(bus);
        return -1;
    }

    fprintf(stderr, "ipc-server: escuchando en %s%s on %s\n",
            ZBD_DBUS_BUS_NAME, ZBD_DBUS_OBJECT_PATH, ZBD_DBUS_INTERFACE);

    /* Event loop. We block on sd_bus_wait() with a 1 s timeout so
     * we can periodically observe zbd_shutdown_requested even when
     * no message is in flight. */
    while (!zbd_shutdown_requested)
    {
        rc = sd_bus_process(bus, NULL);
        if (rc < 0)
        {
            fprintf(stderr, "ipc-server: sd_bus_process: %s\n", strerror(-rc));
            break;
        }
        if (rc > 0)
        {
            continue; /* hubo trabajo, vuelve a procesar */
        }

        /* Esperar hasta 1 s por nuevos mensajes. */
        rc = sd_bus_wait(bus, 1 * 1000000ULL);
        if (rc < 0 && rc != -EINTR)
        {
            fprintf(stderr, "ipc-server: sd_bus_wait: %s\n", strerror(-rc));
            break;
        }
    }

    sd_bus_release_name(bus, ZBD_DBUS_BUS_NAME);
    sd_bus_unref(bus);
    return rc < 0 ? -1 : 0;
}
