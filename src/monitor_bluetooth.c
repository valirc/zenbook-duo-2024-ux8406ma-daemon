#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>

#include "comun.h"
#include "pantalla.h"
#include "teclado.h"

//const char *MAC_TECLADO = "C2:CE:E8:06:01:F9";

/* ===================================================
 * teclado_conectado:
 * Devuelve 1 si bluetoothctl info <MAC> contiene 
 * "Connected: yes", sino 0.
 * =================================================== */
int teclado_conectado(void)
{
    /* Construimos el comando: 
       bluetoothctl info C2:CE:E8:06:01:F9 | grep -c "Connected: yes" */
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "bluetoothctl info %s | grep -c 'Connected: yes'",
             cfg->bluetooth_mac_teclado);

    /* Abrimos popen */
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "Error al ejecutar '%s': %s\n", cmd, strerror(errno));
        return 0;
    }

    int count = 0;
    fscanf(fp, "%d", &count);
    pclose(fp);

    /* Si count >= 1 => hay "Connected: yes" => devolvemos 1 */
    return (count >= 1) ? 1 : 0;
}

/* ===================================================
 * monitorizar_cambios_bluetooth:
 * 1) Revisa estado actual de teclado y eDP-2.
 * 2) Ajusta si hace falta.
 * 3) Escucha 'bluetoothctl --monitor' leyendo cada línea.
 * =================================================== */
void *monitorizar_cambios_bluetooth(void *arg)
{
    /* == 1) Estado actual == */
    int estadoTeclado  = teclado_conectado();     /* 1 conectado, 0 no conectado */
    int estadoPantalla = monitor_estado("eDP-2"); /* 1 encendida, 0 apagada */

    /* == Lógica del script: == */
    if (estadoTeclado == 1 && estadoPantalla == 0) {
        printf("Teclado conectado. Encendiendo eDP-2...\n");
        configurar_monitores("encender");
        usleep(250000); /* sleep 0.25s */
        poner_fondo_2_monitores();
    } 
    else if (estadoTeclado == 0 && estadoPantalla == 1) {
        printf("Teclado desconectado. Apagando eDP-2...\n");
        configurar_monitores("apagar");
        usleep(250000);
        poner_fondo_1_monitor();
    }

    /* == 2) Iniciar monitorización de bluetoothctl --monitor == */
    printf("Iniciando monitorización de eventos Bluetooth...\n");

    /* Abrimos un pipe con popen("bluetoothctl --monitor", "r") */
    FILE *fp = popen("bluetoothctl --monitor", "r");
    if (!fp) {
        fprintf(stderr, "Error al ejecutar 'bluetoothctl --monitor': %s\n", strerror(errno));
        return NULL;
    }

    /* Leemos línea por línea */
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* Reaccionamos si la línea contiene "Connected: yes" o "Connected: no" */
        if (strstr(line, "Connected: yes") != NULL) {
            printf("Teclado conectado\n");
            /* Revisar si eDP-2 está apagada */
            if (!monitor_estado("eDP-2")) {
                printf("El teclado está conectado y eDP-2 está apagada. Encendiendo eDP-2...\n");
                configurar_monitores("encender");
                usleep(250000);
                poner_fondo_2_monitores();
            }
        }
        else if (strstr(line, "Connected: no") != NULL) {
            printf("Teclado desconectado\n");
            /* Revisar si eDP-2 está encendida */
            if (monitor_estado("eDP-2")) {
                printf("El teclado no está conectado y eDP-2 está encendida. Apagando eDP-2...\n");
                configurar_monitores("apagar");
                usleep(250000);
                poner_fondo_1_monitor();
            }
        }
        /* Omitimos otras líneas */
    }

    /* Cuando bluetoothctl --monitor se detenga, salimos de la función */
    pclose(fp);

    return NULL;
}
