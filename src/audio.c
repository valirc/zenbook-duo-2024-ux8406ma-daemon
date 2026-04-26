/*
 * audio.c — load and route the Intel SST DMIC raw source.
 *
 * Wraps the four pactl invocations the user wants every time the
 * daemon boots:
 *   1. load-module module-alsa-source device=hw:0,6 ... dmic_raw
 *   2. set-default-source dmic_raw
 *   3. set-source-volume  @DEFAULT_SOURCE@ 70%
 *   4. set-source-mute    @DEFAULT_SOURCE@ false
 *
 * pactl talks to the PulseAudio / PipeWire instance reachable from
 * the current process: it uses XDG_RUNTIME_DIR to find the socket.
 * In this deployment the daemon runs as root, the user's graphical
 * session is also root, and therefore pactl reaches the same PA.
 *
 * That said, the daemon may start *before* PulseAudio is up
 * (zbd-system.service is a `Type=dbus` unit that only orders
 * After=dbus.service, not the audio stack). The very first
 * `pactl load-module` attempt at service boot can therefore fail
 * with "Connection failure: Connection refused". The function
 * still returns EXIT_FAILURE in that case so the caller can decide
 * what to do; main.c's service-boot path treats that as best-effort
 * and does NOT abort the daemon.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "audio.h"
#include "exec.h"

#define DMIC_DEVICE      "device=hw:0,6"
#define DMIC_SOURCE_NAME "source_name=dmic_raw"
#define DMIC_CHANNELS    "channels=2"
#define DMIC_FORMAT      "format=s16le"

static int pactl_server_reachable(void)
{
    /* `pactl info` returns 0 when it can connect to the daemon and
     * non-zero otherwise. We call it once before the load-module so
     * we can produce a clearer diagnostic than pactl's default. */
    char *const args[] = { "pactl", "info", NULL };
    int rc = exec_cmd_argv("pactl", args);
    return rc == 0;
}

int configurar_dmic_raw(void)
{
    if (!pactl_server_reachable())
    {
        fprintf(stderr,
                "configurar_dmic_raw: PulseAudio/PipeWire no responde en "
                "XDG_RUNTIME_DIR=%s; saltando configuracion DMIC.\n",
                getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(unset)");
        return EXIT_FAILURE;
    }

    /* Cargar el module-alsa-source apuntando al DMIC del Intel SST. */
    char *const args_load[] = {
        "pactl", "load-module", "module-alsa-source",
        DMIC_DEVICE, DMIC_SOURCE_NAME, DMIC_CHANNELS, DMIC_FORMAT,
        NULL
    };
    int ret = exec_cmd_argv("pactl", args_load);
    if (ret != 0)
    {
        fprintf(stderr, "configurar_dmic_raw: pactl load-module fallo (%d)\n", ret);
        return EXIT_FAILURE;
    }

    /* PulseAudio/PipeWire pueden tardar un instante en exponer el sink. */
    sleep(1);

    char *const args_default[] = { "pactl", "set-default-source", "dmic_raw", NULL };
    char *const args_volume[]  = { "pactl", "set-source-volume",  "@DEFAULT_SOURCE@", "70%", NULL };
    char *const args_unmute[]  = { "pactl", "set-source-mute",    "@DEFAULT_SOURCE@", "false", NULL };

    if (exec_cmd_argv("pactl", args_default) != 0 ||
        exec_cmd_argv("pactl", args_volume)  != 0 ||
        exec_cmd_argv("pactl", args_unmute)  != 0)
    {
        fprintf(stderr, "configurar_dmic_raw: ajuste de fuente por defecto fallo\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
