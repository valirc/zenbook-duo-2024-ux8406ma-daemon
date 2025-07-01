#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "pantalla.h"
#include "teclado.h"
#include "comun.h"
#include "config.h"

#define CONFIG_PATH "/etc/zbd/zbd.conf"

struct SConfiguracion *cfg = NULL;

int cargar_configuracion()
{
    FILE *config_file = fopen(CONFIG_PATH, "r");
    if (!config_file)
    {
        fprintf(stderr, "Configuración no definida (%s).\n", strerror(errno));
        return -1;
    }

    char line[256];
    cfg = (struct SConfiguracion *)calloc(1, sizeof(struct SConfiguracion));

    while (fgets(line, sizeof(line), config_file))
    {
        if (line[0] == '\n' || line[0] == '#')
            continue;

        char key[50], value[200];
        if (sscanf(line, "%49[^=]=%49s", key, value) == 2)
        {
            if (!strcmp(key, "modo_deteccion"))
                cfg->modo_deteccion = strdup(value);
            else if (!strcmp(key, "bluetooth_mac_teclado"))
                cfg->bluetooth_mac_teclado = strdup(value);
            else if (!strcmp(key, "udev_usb_path"))
                cfg->udev_usb_path = strdup(value);
            else if (!strcmp(key, "orientacion_bus"))
                cfg->orientacion_bus = strdup(value);
            else if (!strcmp(key, "orientacion_path"))
                cfg->orientacion_path = strdup(value);
            else if (!strcmp(key, "orientacion_interfaz"))
                cfg->orientacion_interfaz = strdup(value);
            else if (!strcmp(key, "pantalla_resolucion"))
                cfg->pantalla_resolucion = strdup(value);
            else if (!strcmp(key, "pantalla_tasa_refresco"))
                cfg->pantalla_tasa_refresco = strdup(value);
            else if (!strcmp(key, "pantalla_fondo_edp1"))
                cfg->pantalla_fondo_edp1 = strdup(value);
            else if (!strcmp(key, "pantalla_fondo_edp2"))
                cfg->pantalla_fondo_edp2 = strdup(value);
            else if (!strcmp(key, "pantalla_nivel_brillo"))
            {
                cfg->pantalla_nivel_brillo = atoi(value);
                set_pantalla_brillo(cfg->pantalla_nivel_brillo);
            }
            else if (!strcmp(key, "teclado_nivel_brillo"))
            {
                cfg->teclado_nivel_brillo = atoi(value);
                set_brillo_teclado(cfg->teclado_nivel_brillo);
            }
            else if (!strcmp(key, "bateria_carga_maxima"))
            {
                limitar_carga_bateria(atoi(value));
            }
        }
    }

    fclose(config_file);
    return 0;
}

int limitar_carga_bateria(int nivel_bateria)
{
    if (nivel_bateria < 20 || nivel_bateria > 100)
    {
        fprintf(stderr, "Nivel inválido. Debe ser un entero entre 20 y 100.\n");
        return EXIT_FAILURE;
    }

    ejecutar_comando("echo %d > /sys/class/power_supply/BAT0/charge_control_end_threshold", nivel_bateria);
    return EXIT_SUCCESS;
}
