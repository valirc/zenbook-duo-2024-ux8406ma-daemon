/*
 * runtime.c — global shutdown flag and signal-handler installation.
 *
 * See runtime.h for the contract. This file is intentionally minimal so
 * the signal-handling path performs only async-signal-safe operations
 * (writing to a sig_atomic_t).
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "runtime.h"

volatile sig_atomic_t zbd_shutdown_requested = 0;

static void on_shutdown_signal(int signo)
{
    (void)signo;
    zbd_shutdown_requested = 1;
}

int zbd_install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    /* SA_RESTART so blocking syscalls (read, select with NULL timeout)
     * either resume after the handler or return EINTR cleanly; the
     * loops still poll zbd_shutdown_requested, so either path works. */
    sa.sa_flags = SA_RESTART;

    const int signals_to_catch[] = { SIGINT, SIGTERM, SIGHUP };
    for (size_t i = 0; i < sizeof(signals_to_catch) / sizeof(signals_to_catch[0]); ++i)
    {
        if (sigaction(signals_to_catch[i], &sa, NULL) != 0)
        {
            fprintf(stderr,
                    "zbd_install_signal_handlers: sigaction(%d) fallo: %s\n",
                    signals_to_catch[i], strerror(errno));
            return -1;
        }
    }

    /* Ignore SIGPIPE so that, for example, bluetoothctl exiting while we
     * read its pipe does not kill the daemon: the read returns 0/EPIPE
     * and the loop handles it. */
    struct sigaction sa_ign;
    memset(&sa_ign, 0, sizeof(sa_ign));
    sa_ign.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa_ign, NULL) != 0)
    {
        fprintf(stderr,
                "zbd_install_signal_handlers: sigaction(SIGPIPE) fallo: %s\n",
                strerror(errno));
        return -1;
    }

    return 0;
}
