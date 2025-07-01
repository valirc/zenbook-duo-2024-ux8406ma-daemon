#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <dirent.h>
#include <errno.h>

#include "comun.h"
#include "pantalla.h"
#include "teclado.h"

int set_pantalla_brillo(int nivel_brillo)
{
    if (nivel_brillo < 10 || nivel_brillo > 100) {
        fprintf(stderr, "Nivel inválido. Debe ser un entero entre 10 y 100.\n");
        return EXIT_FAILURE;
    }

    nivel_brillo = nivel_brillo * 4;

    ejecutar_comando("echo %d > /sys/class/backlight/intel_backlight/brightness", nivel_brillo);

    return EXIT_SUCCESS;
}

/* ===================================================
 * configurar_monitores: "encender" o "apagar" eDP-2
 * =================================================== */
void configurar_monitores(const char *accion)
{
    if (strcmp(accion, "encender") == 0) {
        printf("Activando eDP-2...\n");
        ejecutar_comando("xrandr --output eDP-2 --auto");
        printf("Configurando la posición y resolución de las pantallas...\n");
        ejecutar_comando(
            "xrandr --output eDP-1 --mode %s --rate %s --primary "
            "--output eDP-2 --mode %s --rate %s --below eDP-1",
            cfg->pantalla_resolucion, cfg->pantalla_tasa_refresco, cfg->pantalla_resolucion, cfg->pantalla_tasa_refresco
        );
    }
    else if (strcmp(accion, "apagar") == 0) {
        printf("Apagando eDP-2...\n");
        ejecutar_comando("xrandr --output eDP-2 --off");
        printf("Configurando eDP-1 con resolución %s a %s Hz...\n",
               cfg->pantalla_resolucion, cfg->pantalla_tasa_refresco);
        ejecutar_comando(
            "xrandr --output eDP-1 --mode %s --rate %s",
            cfg->pantalla_resolucion, cfg->pantalla_tasa_refresco
        );
    }

    set_pantalla_brillo(cfg->pantalla_nivel_brillo);
    set_brillo_teclado(cfg->teclado_nivel_brillo);
}

int monitor_estado(const char *monitor_id) {
    if (!monitor_id || strlen(monitor_id) == 0) {
        fprintf(stderr, "El identificador del monitor es inválido.\n");
        return 0;
    }

    char monitor_path[512];
    snprintf(monitor_path, sizeof(monitor_path), "/sys/class/drm/card1-%s/enabled", monitor_id);

    /* Verificar si el archivo existe */
    FILE *enabled_file = fopen(monitor_path, "r");
    if (!enabled_file) {
        fprintf(stderr, "El monitor %s no existe o no se puede acceder a %s: %s\n", 
                monitor_id, monitor_path, strerror(errno));
        return 0;
    }

    /* Leer el estado del archivo "enabled" */
    char status[16];
    if (fgets(status, sizeof(status), enabled_file)) {
        fclose(enabled_file);

        /* Comparar el valor leído con "enabled" */
        return !strcmp(status, "enabled\n");
    }

    fclose(enabled_file);

    /* Si no se pudo leer el estado, devolvemos 0 como fallback */
    return 0;
}

/* ===================================================
 * Cuando eDP-2 se enciende, se usan 2 fondos con feh,
 * cuando se apaga, solo uno.
 * =================================================== */
void poner_fondo_2_monitores(void)
{
    ejecutar_comando("feh --bg-scale %s --bg-scale %s &", cfg->pantalla_fondo_edp1, cfg->pantalla_fondo_edp2);
}

void poner_fondo_1_monitor(void)
{
    ejecutar_comando("feh --bg-scale %s &", cfg->pantalla_fondo_edp1);
}
