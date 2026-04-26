/*
 * pantalla.c — hardware brightness + thin wrappers around the
 * pluggable display backend.
 *
 * Historically pantalla.c held the xrandr-only logic for turning
 * eDP-2 on/off and setting wallpapers. That logic has now moved to
 * the display.c dispatcher and the per-compositor backends
 * (display_xrandr.c, display_gdctl.c). The functions exported here
 * keep the same names and signatures so the rest of the daemon
 * (monitor_*.c) does not have to be touched in this refactor; they
 * just delegate to the active display backend.
 *
 * What still lives in this file:
 *   - set_pantalla_brillo(): writes /sys/class/backlight/intel_backlight
 *     directly. Brightness is a hardware-level operation that has no
 *     business going through a compositor backend.
 *
 * What has been delegated:
 *   - configurar_monitores("encender"|"apagar")  → display_set_output()
 *   - monitor_estado()                            → display_is_output_on()
 *   - poner_fondo_*                               → display_set_wallpapers()
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "comun.h"
#include "pantalla.h"
#include "teclado.h"
#include "display.h"

#define BACKLIGHT_PATH "/sys/class/backlight/intel_backlight/brightness"

int set_pantalla_brillo(int nivel_brillo)
{
    if (nivel_brillo < 10 || nivel_brillo > 100)
    {
        fprintf(stderr, "Nivel invalido. Debe ser un entero entre 10 y 100.\n");
        return EXIT_FAILURE;
    }

    /* El backlight de intel_backlight tiene rango [0, max_brightness];
     * la calibracion empirica para el UX8406MA da max=400 y queremos
     * que [10..100] mapee a [40..400], es decir, multiplicamos por 4. */
    int valor = nivel_brillo * 4;

    int fd = open(BACKLIGHT_PATH, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
    {
        fprintf(stderr, "set_pantalla_brillo: no se puede abrir %s: %s\n",
                BACKLIGHT_PATH, strerror(errno));
        return EXIT_FAILURE;
    }

    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d\n", valor);
    if (len <= 0 || (size_t)len >= sizeof(buf))
    {
        close(fd);
        fprintf(stderr, "set_pantalla_brillo: snprintf overflow\n");
        return EXIT_FAILURE;
    }

    ssize_t written = write(fd, buf, (size_t)len);
    int saved = errno;
    close(fd);

    if (written != len)
    {
        fprintf(stderr, "set_pantalla_brillo: write fallo: %s\n",
                strerror(saved));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

void configurar_monitores(const char *accion)
{
    if (!accion)
    {
        fprintf(stderr, "configurar_monitores: accion nula\n");
        return;
    }

    if (strcmp(accion, "encender") == 0)
    {
        printf("Activando eDP-2 via backend %s...\n",
               display_active_backend() ? display_active_backend() : "(none)");
        if (display_set_output("eDP-2", DISPLAY_OUTPUT_ON,
                               cfg->pantalla_resolucion,
                               cfg->pantalla_tasa_refresco) != 0)
        {
            fprintf(stderr, "configurar_monitores(encender): backend fallo\n");
            return;
        }
    }
    else if (strcmp(accion, "apagar") == 0)
    {
        printf("Apagando eDP-2 via backend %s...\n",
               display_active_backend() ? display_active_backend() : "(none)");
        if (display_set_output("eDP-2", DISPLAY_OUTPUT_OFF, NULL, NULL) != 0)
        {
            fprintf(stderr, "configurar_monitores(apagar): backend fallo\n");
            return;
        }
    }
    else
    {
        fprintf(stderr, "configurar_monitores: accion desconocida '%s'\n", accion);
        return;
    }

    /* Restaurar brillo y backlight del teclado tras cualquier
     * reconfiguracion (algunos compositores los resetean). */
    set_pantalla_brillo(cfg->pantalla_nivel_brillo);
    set_brillo_teclado(cfg->teclado_nivel_brillo);
}

int monitor_estado(const char *monitor_id)
{
    if (!monitor_id || strlen(monitor_id) == 0)
    {
        fprintf(stderr, "El identificador del monitor es invalido.\n");
        return 0;
    }
    return display_is_output_on(monitor_id);
}

void poner_fondo_2_monitores(void)
{
    if (!cfg) return;
    display_set_wallpapers(cfg->pantalla_fondo_edp1, cfg->pantalla_fondo_edp2);
}

void poner_fondo_1_monitor(void)
{
    if (!cfg) return;
    display_set_wallpapers(cfg->pantalla_fondo_edp1, NULL);
}
