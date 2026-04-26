#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "comun.h"
#include "audio.h"

int configurar_dmic_raw(void)
{
    int ret = ejecutar_comando(
        "pactl load-module module-alsa-source device=\"hw:0,6\" "
        "source_name=dmic_raw channels=2 format=s16le");
    if (ret != 0)
    {
        fprintf(stderr, "configurar_dmic_raw: pactl load-module fallo (%d)\n", ret);
        return EXIT_FAILURE;
    }

    /* PulseAudio/PipeWire pueden tardar un instante en exponer el sink */
    sleep(1);

    if (ejecutar_comando("pactl set-default-source dmic_raw") != 0 ||
        ejecutar_comando("pactl set-source-volume @DEFAULT_SOURCE@ 70%%") != 0 ||
        ejecutar_comando("pactl set-source-mute @DEFAULT_SOURCE@ false") != 0)
    {
        fprintf(stderr, "configurar_dmic_raw: ajuste de fuente por defecto fallo\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}