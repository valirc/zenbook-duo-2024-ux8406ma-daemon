#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libudev.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>

#include "comun.h"
#include "pantalla.h"
#include "monitor_teclado_usb.h"
#include "runtime.h"

/*
 * Devuelve 1 si el dispositivo USB en `usb_path` está presente en sysfs
 * (teclado conectado), 0 si no existe.
 *
 * IMPORTANTE: no llama udev_unref(udev) — la propiedad del contexto udev
 * pertenece al hilo llamante.  La versión anterior la liberaba aquí, lo que
 * provocaba un double-free al final del hilo.
 */
static int inicializar_estado(struct udev *udev, const char *usb_path)
{
    /* udev_device_new_from_syspath needs the full path including /sys;
     * the config stores only the devpath (without /sys prefix) so that
     * it matches udev_device_get_devpath() in the event loop. */
    char syspath[512];
    snprintf(syspath, sizeof(syspath), "/sys%s", usb_path);

    struct udev_device *dev = udev_device_new_from_syspath(udev, syspath);
    if (!dev)
        return 0;   /* nodo sysfs no existe → teclado no conectado */

    const char *devtype = udev_device_get_property_value(dev, "DEVTYPE");
    int presente = (devtype && strcmp(devtype, "usb_device") == 0) ? 1 : 0;
    udev_device_unref(dev);
    return presente;
}

void *monitorizar_cambios_teclado_usb(void *arg)
{
    (void)arg;
    struct udev *udev = udev_new();
    if (!udev)
    {
        fprintf(stderr, "Cannot create udev context.\n");
        return NULL;
    }

    struct udev_monitor *mon = udev_monitor_new_from_netlink(udev, "udev");
    if (!mon)
    {
        fprintf(stderr, "Cannot create udev monitor.\n");
        udev_unref(udev);
        return NULL;
    }

    udev_monitor_filter_add_match_subsystem_devtype(mon, "usb", NULL);
    udev_monitor_enable_receiving(mon);

    int fd = udev_monitor_get_fd(mon);

    /*
     * Estado inicial: si el teclado ya está conectado físicamente encima del
     * ScreenPad, eDP-2 debe estar APAGADO (el teclado lo cubre).
     * Si el teclado está desconectado, eDP-2 debe estar ENCENDIDO.
     */
    int teclado_presente = inicializar_estado(udev, cfg->udev_usb_path);
    if (teclado_presente)
    {
        fprintf(stderr, "monitor_usb: teclado conectado al arranque — apagando eDP-2\n");
        configurar_monitores("apagar");
        usleep(250000);
        poner_fondo_1_monitor();
    }
    else
    {
        fprintf(stderr, "monitor_usb: teclado NO conectado al arranque — encendiendo eDP-2\n");
        configurar_monitores("encender");
        usleep(250000);
        poner_fondo_2_monitores();
    }

    fprintf(stderr, "monitor_usb: escuchando eventos USB en %s\n", cfg->udev_usb_path);

    while (!zbd_shutdown_requested)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);

        struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
        int ret = select(fd + 1, &fds, NULL, NULL, &timeout);
        if (ret < 0)
        {
            if (errno == EINTR) continue;
            fprintf(stderr, "monitor_usb: select() fallo: %s\n", strerror(errno));
            break;
        }
        if (ret == 0) continue;

        if (!FD_ISSET(fd, &fds)) continue;

        struct udev_device *dev = udev_monitor_receive_device(mon);
        if (!dev) continue;

        const char *action  = udev_device_get_action(dev);
        const char *devpath = udev_device_get_devpath(dev);

        /*
         * Filtrar por la ruta sysfs del teclado configurada.
         * NO usamos devnode: los eventos "remove" no tienen nodo de dispositivo
         * (ya fue eliminado) y la comprobación de devnode los filtraría.
         */
        if (action && devpath && !strcmp(devpath, cfg->udev_usb_path))
        {
            if (!strcmp(action, "add"))
            {
                /*
                 * Teclado físicamente conectado encima del ScreenPad:
                 * apagar eDP-2 (el teclado lo cubre).
                 */
                fprintf(stderr, "monitor_usb: teclado conectado\n");
                if (monitor_estado("eDP-2"))
                {
                    fprintf(stderr, "monitor_usb: eDP-2 activo → apagando\n");
                    configurar_monitores("apagar");
                    usleep(250000);
                    poner_fondo_1_monitor();
                }
            }
            else if (!strcmp(action, "remove"))
            {
                /*
                 * Teclado físicamente desconectado: el ScreenPad queda expuesto.
                 * Encender eDP-2.
                 */
                fprintf(stderr, "monitor_usb: teclado desconectado\n");
                if (!monitor_estado("eDP-2"))
                {
                    fprintf(stderr, "monitor_usb: eDP-2 inactivo → encendiendo\n");
                    configurar_monitores("encender");
                    usleep(250000);
                    poner_fondo_2_monitores();
                }
            }
        }

        udev_device_unref(dev);
    }

    udev_monitor_unref(mon);
    udev_unref(udev);
    return NULL;
}
