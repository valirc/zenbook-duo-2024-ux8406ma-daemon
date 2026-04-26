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
#include "audio.h"
#include "monitor_bluetooth.h"
#include "monitor_orientacion.h"
#include "monitor_teclado_usb.h"
#include "config.h"
#include "runtime.h"

/*
 * Función para imprimir la forma de uso (similar al '*)' del script bash.
 */
void print_usage(const char *progname)
{
    fprintf(stderr, "Uso: %s <limitar-carga-bateria <n>|set-brillo-pantalla <n>|set-brillo-teclado <n>|activar-dmic-raw|monitorizar-rotacion|monitorizar-bluetooth>\n", progname);
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

        return limitar_carga_bateria(atoi(argv[2]));
    }
    else if (strcmp(command, "set-brillo-pantalla") == 0)
    {
        if (argc < 3)
        {
            fprintf(stderr, "Falta el nivel para set-brillo-pantalla.\n");
            return EXIT_FAILURE;
        }
        set_pantalla_brillo(atoi(argv[2]));
    }
    else if (strcmp(command, "activar-dmic-raw") == 0)
    {
        return configurar_dmic_raw();
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

        if (zbd_install_signal_handlers() != 0)
        {
            return EXIT_FAILURE;
        }

        pthread_t hilo_orientacion = 0, hilo_modo_deteccion = 0;

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
        }
        else if (!strcmp(cfg->modo_deteccion, "udev"))
        {
            if (pthread_create(&hilo_modo_deteccion, NULL, monitorizar_cambios_teclado_usb, NULL) != 0)
            {
                fprintf(stderr, "Error al crear hilo udev\n");
                return 1;
            }
        }
        else
        {
            fprintf(stderr, "modo_deteccion invalido: '%s' (se espera 'udev' o 'bluetooth')\n",
                    cfg->modo_deteccion ? cfg->modo_deteccion : "(null)");
            return 1;
        }

        /* Bloquear hasta que ambos hilos terminen (normalmente solo via senal). */
        pthread_join(hilo_orientacion, NULL);
        pthread_join(hilo_modo_deteccion, NULL);
    }
    else
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
