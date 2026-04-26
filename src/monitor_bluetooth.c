/*
 * monitor_bluetooth.c — track the keyboard's Bluetooth connection state
 * and toggle eDP-2 / wallpapers accordingly.
 *
 * Two pieces of work:
 *
 * 1) teclado_conectado() asks bluetoothctl for the current state of the
 *    keyboard MAC and reports whether it is connected.
 *
 * 2) monitorizar_cambios_bluetooth() runs `bluetoothctl --monitor` as a
 *    long-lived child process and reacts to "Connected: yes" /
 *    "Connected: no" lines on the configured MAC.
 *
 * Previously both used popen() + sprintf, which spawned a /bin/sh -c
 * with the MAC interpolated into the command string. That was a
 * command-injection vector via /etc/zbd/zbd.conf. This rewrite uses the
 * shell-free helpers from exec.h: bluetoothctl is invoked with an
 * explicit argv vector, the MAC is passed verbatim, and the daemon
 * parses the output in C instead of piping into grep.
 *
 * The monitor loop also honours zbd_shutdown_requested (set by the
 * SIGINT/SIGTERM handler in runtime.c) so the thread releases the
 * child process cleanly on shutdown.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <signal.h>

#include "comun.h"
#include "pantalla.h"
#include "teclado.h"
#include "exec.h"
#include "runtime.h"
#include "monitor_bluetooth.h"

/* ===================================================
 * teclado_conectado:
 * Devuelve 1 si bluetoothctl info <MAC> contiene "Connected: yes",
 * 0 en caso contrario. La MAC se pasa como argv (sin shell), por lo
 * que un valor con metacaracteres en la config no se interpreta.
 * =================================================== */
int teclado_conectado(void)
{
    if (!cfg || !cfg->bluetooth_mac_teclado || !*cfg->bluetooth_mac_teclado)
    {
        fprintf(stderr, "teclado_conectado: bluetooth_mac_teclado vacio\n");
        return 0;
    }

    char *const args[] = {
        "bluetoothctl", "info", cfg->bluetooth_mac_teclado, NULL
    };
    pid_t pid = 0;
    FILE *fp = exec_cmd_pipe("bluetoothctl", args, &pid);
    if (!fp)
    {
        fprintf(stderr, "teclado_conectado: exec_cmd_pipe fallo: %s\n",
                strerror(errno));
        return 0;
    }

    char line[512];
    int connected = 0;
    while (fgets(line, sizeof(line), fp))
    {
        if (strstr(line, "Connected: yes"))
        {
            connected = 1;
            /* Seguir consumiendo el pipe para que bluetoothctl no
             * quede bloqueado en una escritura cuando cerremos el
             * descriptor. */
        }
    }

    exec_cmd_pipe_close(fp, pid);
    return connected;
}

/* ===================================================
 * monitorizar_cambios_bluetooth:
 * 1) Revisa estado actual de teclado y eDP-2 y ajusta si hace falta.
 * 2) Lanza `bluetoothctl --monitor` y reacciona a sus eventos.
 * =================================================== */
void *monitorizar_cambios_bluetooth(void *arg)
{
    (void)arg;

    /* == 1) Estado actual == */
    int estadoTeclado  = teclado_conectado();
    int estadoPantalla = monitor_estado("eDP-2");

    if (estadoTeclado == 1 && estadoPantalla == 0)
    {
        printf("Teclado conectado. Encendiendo eDP-2...\n");
        configurar_monitores("encender");
        usleep(250000);
        poner_fondo_2_monitores();
    }
    else if (estadoTeclado == 0 && estadoPantalla == 1)
    {
        printf("Teclado desconectado. Apagando eDP-2...\n");
        configurar_monitores("apagar");
        usleep(250000);
        poner_fondo_1_monitor();
    }

    /* == 2) Iniciar monitorizacion de bluetoothctl --monitor == */
    printf("Iniciando monitorizacion de eventos Bluetooth...\n");

    char *const monitor_args[] = { "bluetoothctl", "--monitor", NULL };
    pid_t monitor_pid = 0;
    FILE *fp = exec_cmd_pipe("bluetoothctl", monitor_args, &monitor_pid);
    if (!fp)
    {
        fprintf(stderr, "Error al ejecutar 'bluetoothctl --monitor': %s\n",
                strerror(errno));
        return NULL;
    }

    /* Lectura linea por linea. El bucle termina cuando bluetoothctl
     * cierra el pipe, cuando se senala el apagado del daemon o cuando
     * fgets devuelve EOF/error. */
    char line[512];
    while (!zbd_shutdown_requested && fgets(line, sizeof(line), fp))
    {
        if (strstr(line, "Connected: yes"))
        {
            printf("Teclado conectado\n");
            if (!monitor_estado("eDP-2"))
            {
                printf("El teclado esta conectado y eDP-2 esta apagada. Encendiendo eDP-2...\n");
                configurar_monitores("encender");
                usleep(250000);
                poner_fondo_2_monitores();
            }
        }
        else if (strstr(line, "Connected: no"))
        {
            printf("Teclado desconectado\n");
            if (monitor_estado("eDP-2"))
            {
                printf("El teclado no esta conectado y eDP-2 esta encendida. Apagando eDP-2...\n");
                configurar_monitores("apagar");
                usleep(250000);
                poner_fondo_1_monitor();
            }
        }
        /* Resto de lineas: ignoradas. */
    }

    /* Si el bucle salio por shutdown_requested y bluetoothctl sigue
     * vivo, le pedimos que termine para que exec_cmd_pipe_close no
     * bloquee indefinidamente. */
    if (zbd_shutdown_requested && monitor_pid > 0)
    {
        kill(monitor_pid, SIGTERM);
    }

    exec_cmd_pipe_close(fp, monitor_pid);
    return NULL;
}
