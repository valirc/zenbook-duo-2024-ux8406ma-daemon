#include <unistd.h>
#include "comun.h"

void configurar_dmic_raw(void)
{
    ejecutar_comando("pactl load-module module-alsa-source device=\"hw:0,6\" source_name=dmic_raw channels=2 format=s16le");
    sleep(1);
    ejecutar_comando("pactl set-default-source dmic_raw");
    ejecutar_comando("pactl set-source-volume @DEFAULT_SOURCE@ 70%%");
    ejecutar_comando("pactl set-source-mute @DEFAULT_SOURCE@ false");
}