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
#include "display.h"
#include "ipc.h"

/*
 * Imprime la forma de uso del binario.
 */
static void print_usage(const char *progname)
{
    fprintf(stderr,
            "Uso: %s <comando> [args]\n"
            "\n"
            "Comandos privilegiados (requieren root o ser invocados via D-Bus):\n"
            "  limitar-carga-bateria <20..100>  Limitar el porcentaje maximo de carga\n"
            "  set-brillo-pantalla <10..100>    Brillo del backlight Intel\n"
            "  set-brillo-teclado  <0..3>       Brillo del teclado retroiluminado\n"
            "  activar-dmic-raw                 Cargar source PulseAudio del DMIC\n"
            "\n"
            "Modos de servicio:\n"
            "  service                          Arrancar como servicio D-Bus en\n"
            "                                   org.anexa.zbd1 (escucha hasta SIGTERM)\n"
            "  daemon                           Modo legacy: ejecuta los hilos de\n"
            "                                   monitorizacion (BT/USB/orientacion)\n",
            progname);
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

    const char *command = argv[1];

    /* display_init solo se llama en los modos que realmente reconfiguran
     * el compositor (daemon legacy con sus hilos de monitorizacion).
     * Los CLI privilegiados y el modo D-Bus service no tocan pantallas:
     * solo /sys (brillo, bateria) o el HID del teclado. Inicializar el
     * backend ahi solo afade superficie de fallo (e.g. xrandr ausente)
     * sin beneficio. */
    if (strcmp(command, "daemon") == 0)
    {
        if (display_init() < 0)
        {
            fprintf(stderr, "display: ningun backend disponible; abortando.\n");
            return EXIT_FAILURE;
        }
        fprintf(stderr, "Configuracion cargada (backend display: %s).\n",
                display_active_backend());
    }
    else
    {
        fprintf(stderr, "Configuracion cargada.\n");
    }

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
        return set_pantalla_brillo(atoi(argv[2]));
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
    else if (strcmp(command, "service") == 0)
    {
        /* Diagnostico via stderr: stdout esta line-buffered cuando
         * va al journal y los printf en el arranque pueden quedarse
         * en el buffer hasta el primer \n + flush. fprintf(stderr)
         * llega al journal de inmediato. */
        fprintf(stderr, "Arrancando servicio D-Bus en %s ...\n", ZBD_DBUS_BUS_NAME);
        if (zbd_install_signal_handlers() != 0)
        {
            return EXIT_FAILURE;
        }
        /* Aplicar los defaults persistentes del config antes de
         * entrar al event loop, para que un boot/restart deje el
         * sistema en el estado deseado sin necesidad de un servicio
         * oneshot adicional. */
        fprintf(stderr,
                "Aplicando defaults: brillo=%d, teclado=%d, bateria=%d\n",
                cfg->pantalla_nivel_brillo,
                cfg->teclado_nivel_brillo,
                cfg->bateria_carga_maxima);
        set_pantalla_brillo(cfg->pantalla_nivel_brillo);
        set_brillo_teclado(cfg->teclado_nivel_brillo);
        if (cfg->bateria_carga_maxima > 0)
        {
            limitar_carga_bateria(cfg->bateria_carga_maxima);
        }

        return zbd_ipc_server_run() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    else if (strcmp(command, "daemon") == 0)
    {
        fprintf(stderr, "Iniciando daemon...\n");

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
