/*
 * ipc_client.c — sd-bus client wrappers used by zbd-tray.
 *
 * Each public entry point opens a transient system-bus connection,
 * invokes the corresponding method on org.anexa.zbd1.System, and
 * tears the connection down. We do not pool the connection because
 * these calls are infrequent (one per tray menu activation) and
 * keeping a persistent client connection would complicate
 * thread-safety and shutdown.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <systemd/sd-bus.h>

#include "ipc.h"

static int call_method_with_int_arg(const char *method, int level)
{
    sd_bus *bus = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int rc;

    rc = sd_bus_open_system(&bus);
    if (rc < 0)
    {
        fprintf(stderr, "ipc: sd_bus_open_system fallo: %s\n", strerror(-rc));
        return -1;
    }

    rc = sd_bus_call_method(bus,
                            ZBD_DBUS_BUS_NAME,
                            ZBD_DBUS_OBJECT_PATH,
                            ZBD_DBUS_INTERFACE,
                            method,
                            &error, &reply,
                            "i", level);
    if (rc < 0)
    {
        fprintf(stderr, "ipc: %s fallo: %s\n",
                method,
                error.message ? error.message : strerror(-rc));
    }

    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    sd_bus_unref(bus);
    return rc < 0 ? -1 : 0;
}

static int call_method_no_args(const char *method)
{
    sd_bus *bus = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int rc;

    rc = sd_bus_open_system(&bus);
    if (rc < 0)
    {
        fprintf(stderr, "ipc: sd_bus_open_system fallo: %s\n", strerror(-rc));
        return -1;
    }

    rc = sd_bus_call_method(bus,
                            ZBD_DBUS_BUS_NAME,
                            ZBD_DBUS_OBJECT_PATH,
                            ZBD_DBUS_INTERFACE,
                            method,
                            &error, &reply, NULL);
    if (rc < 0)
    {
        fprintf(stderr, "ipc: %s fallo: %s\n",
                method,
                error.message ? error.message : strerror(-rc));
    }

    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    sd_bus_unref(bus);
    return rc < 0 ? -1 : 0;
}

int zbd_ipc_client_set_screen_brightness(int level)
{
    return call_method_with_int_arg("SetScreenBrightness", level);
}

int zbd_ipc_client_set_keyboard_backlight(int level)
{
    return call_method_with_int_arg("SetKeyboardBacklight", level);
}

int zbd_ipc_client_set_battery_threshold(int level)
{
    return call_method_with_int_arg("SetBatteryThreshold", level);
}

int zbd_ipc_client_configure_dmic(void)
{
    return call_method_no_args("ConfigureDmic");
}

int zbd_ipc_client_is_service_available(void)
{
    sd_bus *bus = NULL;
    int rc = sd_bus_open_system(&bus);
    if (rc < 0)
    {
        return 0;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    /* org.freedesktop.DBus.NameHasOwner returns a boolean. */
    rc = sd_bus_call_method(bus,
                            "org.freedesktop.DBus",
                            "/org/freedesktop/DBus",
                            "org.freedesktop.DBus",
                            "NameHasOwner",
                            &error, &reply,
                            "s", ZBD_DBUS_BUS_NAME);
    int present = 0;
    if (rc >= 0 && reply)
    {
        sd_bus_message_read(reply, "b", &present);
    }
    sd_bus_error_free(&error);
    sd_bus_message_unref(reply);
    sd_bus_unref(bus);
    return present ? 1 : 0;
}
