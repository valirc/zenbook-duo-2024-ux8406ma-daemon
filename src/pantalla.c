/*
 * pantalla.c — display, brightness and wallpaper helpers (xrandr backend).
 *
 * This file is the xrandr-only implementation of the screen-management
 * primitives used by the daemon. A future commit will introduce a
 * pluggable display backend (xrandr, gdctl, mutter D-Bus); for the
 * moment the API is unchanged but the implementation no longer goes
 * through /bin/sh.
 *
 * - set_pantalla_brillo() writes directly to the backlight sysfs file
 *   instead of `echo X > /sys/...`, because the redirection required a
 *   shell.
 * - configurar_monitores() and the wallpaper helpers build explicit
 *   argv vectors and exec_cmd_argv into xrandr / feh, so values like
 *   the resolution and refresh rate from /etc/zbd/zbd.conf are passed
 *   as opaque arguments and cannot be re-parsed by a shell.
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
#include "exec.h"

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

/* ===================================================
 * configurar_monitores: "encender" o "apagar" eDP-2
 * =================================================== */
void configurar_monitores(const char *accion)
{
    if (!accion)
    {
        fprintf(stderr, "configurar_monitores: accion nula\n");
        return;
    }

    if (strcmp(accion, "encender") == 0)
    {
        printf("Activando eDP-2...\n");
        char *const args_auto[] = { "xrandr", "--output", "eDP-2", "--auto", NULL };
        if (exec_cmd_argv("xrandr", args_auto) != 0)
        {
            fprintf(stderr, "configurar_monitores(encender): xrandr --auto fallo\n");
            return;
        }

        printf("Configurando la posicion y resolucion de las pantallas...\n");
        char *const args_pos[] = {
            "xrandr",
            "--output", "eDP-1", "--mode", cfg->pantalla_resolucion,
                "--rate",  cfg->pantalla_tasa_refresco, "--primary",
            "--output", "eDP-2", "--mode", cfg->pantalla_resolucion,
                "--rate",  cfg->pantalla_tasa_refresco, "--below", "eDP-1",
            NULL
        };
        exec_cmd_argv("xrandr", args_pos);
    }
    else if (strcmp(accion, "apagar") == 0)
    {
        printf("Apagando eDP-2...\n");
        char *const args_off[] = { "xrandr", "--output", "eDP-2", "--off", NULL };
        if (exec_cmd_argv("xrandr", args_off) != 0)
        {
            fprintf(stderr, "configurar_monitores(apagar): xrandr --off fallo\n");
            return;
        }

        printf("Configurando eDP-1 con resolucion %s a %s Hz...\n",
               cfg->pantalla_resolucion, cfg->pantalla_tasa_refresco);
        char *const args_mode[] = {
            "xrandr",
            "--output", "eDP-1", "--mode", cfg->pantalla_resolucion,
                "--rate",  cfg->pantalla_tasa_refresco,
            NULL
        };
        exec_cmd_argv("xrandr", args_mode);
    }
    else
    {
        fprintf(stderr, "configurar_monitores: accion desconocida '%s'\n", accion);
        return;
    }

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

    char monitor_path[512];
    snprintf(monitor_path, sizeof(monitor_path),
             "/sys/class/drm/card1-%s/enabled", monitor_id);

    FILE *enabled_file = fopen(monitor_path, "r");
    if (!enabled_file)
    {
        fprintf(stderr, "El monitor %s no existe o no se puede acceder a %s: %s\n",
                monitor_id, monitor_path, strerror(errno));
        return 0;
    }

    char status[16];
    int result = 0;
    if (fgets(status, sizeof(status), enabled_file))
    {
        result = !strcmp(status, "enabled\n");
    }
    fclose(enabled_file);
    return result;
}

/* ===================================================
 * Cuando eDP-2 se enciende, se usan 2 fondos con feh,
 * cuando se apaga, solo uno.
 * =================================================== */
void poner_fondo_2_monitores(void)
{
    char *const args[] = {
        "feh", "--bg-scale", cfg->pantalla_fondo_edp1,
               "--bg-scale", cfg->pantalla_fondo_edp2,
        NULL
    };
    exec_cmd_argv("feh", args);
}

void poner_fondo_1_monitor(void)
{
    char *const args[] = {
        "feh", "--bg-scale", cfg->pantalla_fondo_edp1, NULL
    };
    exec_cmd_argv("feh", args);
}
