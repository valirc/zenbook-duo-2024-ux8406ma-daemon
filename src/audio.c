#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "audio.h"
#include "exec.h"

int configurar_dmic_raw(void)
{
    /* Cargar el module-alsa-source apuntando al DMIC del Intel SST. */
    char *const args_load[] = {
        "pactl", "load-module", "module-alsa-source",
        "device=hw:0,6",
        "source_name=dmic_raw",
        "channels=2",
        "format=s16le",
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
    char *const args_volume[]  = { "pactl", "set-source-volume", "@DEFAULT_SOURCE@", "70%", NULL };
    char *const args_unmute[]  = { "pactl", "set-source-mute",   "@DEFAULT_SOURCE@", "false", NULL };

    if (exec_cmd_argv("pactl", args_default) != 0 ||
        exec_cmd_argv("pactl", args_volume)  != 0 ||
        exec_cmd_argv("pactl", args_unmute)  != 0)
    {
        fprintf(stderr, "configurar_dmic_raw: ajuste de fuente por defecto fallo\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
