/*
 * runtime.h — process-wide control flags and signal-handling helpers.
 *
 * The daemon has several long-running loops (libudev select(), bluetoothctl
 * fgets() pipe reader, GMainLoop on iio-sensor-proxy) that all need to be
 * able to abandon their work cleanly when the process receives SIGINT,
 * SIGTERM or SIGHUP. This module owns the global shutdown flag, installs
 * the signal handlers and exposes a tiny API the modules can poll.
 *
 * The flag is `volatile sig_atomic_t` because it is written from a signal
 * handler and read from regular code; reading and writing such a value is
 * the only thing the C standard guarantees safe from inside a handler.
 */

#ifndef ZBD_RUNTIME_H
#define ZBD_RUNTIME_H

#include <signal.h>

/*
 * Set to a non-zero value when SIGINT, SIGTERM or SIGHUP is received.
 * Loops should check this between iterations and exit gracefully.
 */
extern volatile sig_atomic_t zbd_shutdown_requested;

/*
 * Install the SIGINT/SIGTERM/SIGHUP handler that flips
 * zbd_shutdown_requested to 1, and ignore SIGPIPE so a closed pipe (for
 * example bluetoothctl exiting) does not kill the whole daemon.
 *
 * Returns 0 on success, -1 on failure (with errno set by sigaction(2)).
 */
int zbd_install_signal_handlers(void);

#endif /* ZBD_RUNTIME_H */
