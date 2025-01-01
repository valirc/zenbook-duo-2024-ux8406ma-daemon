#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>

#include "comun.h"
#include "teclado.h"
#include "pantalla.h"
#include "monitor_bluetooth.h"
#include "monitor_orientacion.h"
#include "monitor_teclado_usb.h"

#define CONFIG_PATH "/etc/zbd/zbd.conf"

struct SConfiguracion *cfg = NULL;

int cargar_configuracion()
{
    FILE *config_file = fopen(CONFIG_PATH, "r");
    if (!config_file)
    {
        fprintf(stderr, "Configuración no definida (%s).\n", strerror(errno));
        return -1; // Error: archivo no encontrado
    }

    char line[256];

    cfg = (struct SConfiguracion *)calloc(1, sizeof(struct SConfiguracion));

    while (fgets(line, sizeof(line), config_file))
    {
        // Ignorar líneas vacías o comentarios
        if (line[0] == '\n' || line[0] == '#')
        {
            continue;
        }
/*
modo_deteccion = "udev"

mac_teclado = "C2:CE:E8:06:01:F9"

udev_usb_path = "/devices/pci0000:00/0000:00:14.0/usb3/3-6"

orientacion_bus = "net.hadess.SensorProxy"
orientacion_path = "/net/hadess/SensorProxy"
orientacion_interfaz = "net.hadess.SensorProxy"

pantalla_resolucion = "2880x1800"
pantalla_tasa_refresco = "120"
pantalla_fondo_edp1 = "/root/recursos/fondos/bg_edp1.jpg"
pantalla_fondo_edp2 = "/root/recursos/fondos/bg_edp2.jpg"

teclado_nivel_brillo = "1"

bateria_carga_maxima = 80*/
        // Leer parámetros KEY=VALUE
        char key[50], value[200];
        if (sscanf(line, "%49[^=]=%49s", key, value) == 2) {
            if (!strcmp(key, "modo_deteccion"))
            {
                cfg->modo_deteccion = strdup(value);
            }
            else if (!strcmp(key, "bluetooth_mac_teclado"))
            {
                cfg->bluetooth_mac_teclado = strdup(value);
            }
            else if (!strcmp(key, "udev_usb_path"))
            {
                cfg->udev_usb_path = strdup(value);
            }
            else if (!strcmp(key, "orientacion_bus"))
            {
                cfg->orientacion_bus = strdup(value);
            }
            else if (!strcmp(key, "orientacion_path"))
            {
                cfg->orientacion_path = strdup(value);
            }
            else if (!strcmp(key, "orientacion_interfaz"))
            {
                cfg->orientacion_interfaz = strdup(value);
            }
            else if (!strcmp(key, "pantalla_resolucion"))
            {
                cfg->pantalla_resolucion = strdup(value);
            }
            else if (!strcmp(key, "pantalla_tasa_refresco"))
            {
                cfg->pantalla_tasa_refresco = strdup(value);
            }
            else if (!strcmp(key, "pantalla_fondo_edp1"))
            {
                cfg->pantalla_fondo_edp1 = strdup(value);
            }
            else if (!strcmp(key, "pantalla_fondo_edp2"))
            {
                cfg->pantalla_fondo_edp2 = strdup(value);
            }
            else if (!strcmp(key, "pantalla_nivel_brillo"))
            {
                set_pantalla_brillo(atoi(value));
            }
            else if (!strcmp(key, "teclado_nivel_brillo"))
            {
                set_brillo_teclado(atoi(value));
            }
            else if (!strcmp(key, "bateria_carga_maxima"))
            {
                limitar_carga_bateria(atoi(value));
            }
        }
    }

    fclose(config_file);

    return 0; // Éxito
}
/*
 * Función para imprimir la forma de uso (similar al '*)' del script bash.
 */
void print_usage(const char *progname)
{
    fprintf(stderr, "Uso: %s <limitar-carga-bateria <n>|set-brillo-pantalla <n>|set-brillo-teclado <n>|monitorizar-rotacion|monitorizar-bluetooth>\n", progname);
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

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (cargar_configuracion() < 0)
    {
        return EXIT_FAILURE; // Salir si no se puede cargar la configuración
    }

    printf("Configuración cargada exitosamente.\n");

    const char *command = argv[1];

    if (strcmp(command, "limitar-carga-bateria") == 0)
    {
        if (argc < 3)
        {
            fprintf(stderr, "Falta el nivel para limitar-carga-bateria.\n");
            return EXIT_FAILURE;
        }
        return limitar_carga_bateria(argv[2]);
    }
    else if (strcmp(command, "set-brillo-pantalla") == 0)
    {
        if (argc < 3)
        {
            fprintf(stderr, "Falta el nivel para set-brillo-pantalla.\n");
            return EXIT_FAILURE;
        }
        set_pantalla_brillo(argv[2]);
    }
    else if (strcmp(command, "set-brillo-teclado") == 0)
    {
        if (argc < 3)
        {
            fprintf(stderr, "Falta el nivel para set-brillo-teclado.\n");
            return EXIT_FAILURE;
        }
        return set_brillo_teclado(atoi(argv[2]));
    }
    else if (strcmp(command, "daemon") == 0)
    {
        printf("Iniciando daemon...\n");
        pthread_t hilo_orientacion, hilo_modo_deteccion;

        pthread_join(hilo_orientacion, NULL);

        if (pthread_create(&hilo_orientacion, NULL, monitorizar_cambios_orientacion, NULL) != 0)
        {
            fprintf(stderr, "Error al crear hilo de rotacion\n");
            return 1;
        }

        if (!strcmp(cfg->modo_deteccion, "bluetooth"))
        {
            if (pthread_create(&hilo_modo_deteccion, NULL, monitorizar_cambios_bluetooth, NULL) != 0)
            {
                fprintf(stderr, "Error al crear hilo de bluetooth\n");
                return 1;
            }

            pthread_join(hilo_modo_deteccion, NULL);
        }
        else if (!strcmp(cfg->modo_deteccion, "udev"))
        {
            if (pthread_create(&hilo_modo_deteccion, NULL, monitorizar_cambios_teclado_usb, NULL) != 0)
            {
                fprintf(stderr, "Error al crear hilo de bluetooth\n");
                return 1;
            }

            pthread_join(hilo_modo_deteccion, NULL);
        }
    }
    else
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
