#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>

#include "comun.h"
#include "pantalla.h"

int set_pantalla_brillo(int nivel_brillo)
{
    if (nivel_brillo < 10 || nivel_brillo > 100) {
        fprintf(stderr, "Nivel inválido. Debe ser un entero entre 10 y 100.\n");
        return EXIT_FAILURE;
    }

    nivel_brillo = nivel_brillo * 4;

    ejecutar_comando("echo %d > /sys/class/backlight/intel_backlight/brightness");

    return EXIT_SUCCESS;
}

/* ===================================================
 * configurarmonitores: "encender" o "apagar" eDP-2
 * =================================================== */
void configurarmonitores(const char *accion)
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
}

/* ===================================================
 * pantalla_edp2_encendida:
 * Devuelve 1 si eDP-2 está encendida, 0 si no.
 * 
 * En Bash, se basaba en 'xrandr --listmonitors | grep "Monitors: N"'
 * =================================================== */
int pantalla_edp2_encendida(void)
{
    /* Usamos popen para capturar la salida de 'xrandr --listmonitors' */
    FILE *fp = popen("xrandr --listmonitors", "r");
    if (!fp) {
        fprintf(stderr, "Error al ejecutar xrandr --listmonitors: %s\n", strerror(errno));
        return 0; /* fallback a 0 si algo falla */
    }

    /* Buscar una línea con 'Monitors: N' y extraer N */
    int monitors_count = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Monitors: ", 10) == 0) {
            sscanf(line + 10, "%d", &monitors_count); 
            break;
        }
    }

    pclose(fp);

    /* Si hay 2 monitores, devolvemos 1, sino 0 */
    return (monitors_count == 2) ? 1 : 0;
}


/* ===================================================
 * Cuando eDP-2 se enciende, se usan 2 fondos con feh,
 * cuando se apaga, solo uno.
 * =================================================== */
void poner_fondo_2_monitores(void)
{
    ejecutar_comando("feh --bg-scale %s --bg-scale %s", cfg->pantalla_fondo_edp1, cfg->pantalla_fondo_edp2);
}

void poner_fondo_1_monitor(void)
{
    ejecutar_comando("feh --bg-scale %s", cfg->pantalla_fondo_edp1);
}
