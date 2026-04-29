/*
 * ipc.h — D-Bus IPC between zbd-tray (user, unprivileged) and
 * zbd-system (root).
 *
 * The privileged operations the tray needs (writing the backlight
 * sysfs file, the battery charge threshold sysfs file, libusb HID
 * SET_REPORT to the keyboard, loading the DMIC pactl source) live
 * in zbd-system. The tray invokes them over the system D-Bus bus
 * instead of holding the privilege itself, which is the point of
 * the system/tray split.
 *
 * The D-Bus contract:
 *
 *   bus name : org.anexa.zbd1
 *   path     : /org/anexa/zbd1/System
 *   interface: org.anexa.zbd1.System
 *
 *   methods:
 *     SetScreenBrightness(i level) -> ()    level in [10..100]
 *     SetKeyboardBacklight(i level) -> ()   level in [0..3]
 *     SetBatteryThreshold(i level) -> ()    level in [20..100]
 *     SetScreenpadBrightness(i level) -> () level in [0..100] (% → [0..235] raw)
 *     ConfigureDmic()              -> ()    sets @DEFAULT_SOURCE@ vol=70% unmuted
 *
 * In this deployment both zbd-system (privileged service) and
 * zbd-tray (graphical session) run as root, so pactl invocations
 * issued from the privileged process reach the same PulseAudio /
 * PipeWire instance the user interacts with. ConfigureDmic is
 * therefore safe to expose here — it remains valuable to ship over
 * D-Bus so the tray can ask the system service to reapply the DMIC
 * configuration after a PA restart, log file rotation, etc.
 *
 * Bus policy (dbus/org.anexa.zbd.conf): only root may own the name,
 * only members of the `zbd` group can send to it. Polkit
 * (polkit/org.anexa.zbd.policy) further authorises individual
 * actions.
 */

#ifndef ZBD_IPC_H
#define ZBD_IPC_H

#define ZBD_DBUS_BUS_NAME    "org.anexa.zbd1"
#define ZBD_DBUS_OBJECT_PATH "/org/anexa/zbd1/System"
#define ZBD_DBUS_INTERFACE   "org.anexa.zbd1.System"

/*
 * Client side. Each function opens the system bus, invokes the
 * corresponding method, and returns 0 on success or -1 on error
 * (with a diagnostic on stderr). Each call is self-contained: no
 * persistent connection is kept between calls.
 */
int zbd_ipc_client_set_screen_brightness(int level);
int zbd_ipc_client_set_keyboard_backlight(int level);
int zbd_ipc_client_set_battery_threshold(int level);
int zbd_ipc_client_set_screenpad_brightness(int level);
int zbd_ipc_client_configure_dmic(void);

/*
 * Convenience: returns 1 if the system bus has the zbd-system
 * service registered (so calls would route through), 0 otherwise.
 * Useful for `zbd-tray` to fall back to a direct call when running
 * standalone (e.g. when launched manually as root for debugging).
 */
int zbd_ipc_client_is_service_available(void);

/*
 * Server side. Opens the system bus, registers the vtable on the
 * standard path/interface, requests the bus name and runs the
 * sd-bus event loop until zbd_shutdown_requested is set or an
 * error occurs. Returns 0 on clean exit, -1 on failure.
 */
int zbd_ipc_server_run(void);

#endif /* ZBD_IPC_H */
