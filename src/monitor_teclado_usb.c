#include <libudev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "comun.h"
#include "pantalla.h"
#include "monitor_teclado_usb.h"

int inicializar_estado(struct udev *udev, const char *usb_path) {
    struct udev_device *dev = udev_device_new_from_syspath(udev, usb_path);

    if (dev) {
        const char *action = udev_device_get_property_value(dev, "DEVTYPE");
        if (action && strcmp(action, "usb_device") == 0) {
            udev_device_unref(dev);
            udev_unref(udev);
            return 1; // Dispositivo conectado
        }
        udev_device_unref(dev);
    }

    udev_unref(udev);
    return 0; // Dispositivo no conectado
}


void *monitorizar_cambios_teclado_usb(void *arg)
{
    struct udev *udev = udev_new();
    if (!udev)
    {
        fprintf(stderr, "Cannot create udev context.\n");
        exit(1);
    }

    struct udev_monitor *mon = udev_monitor_new_from_netlink(udev, "udev");
    if (!mon)
    {
        fprintf(stderr, "Cannot create udev monitor.\n");
        udev_unref(udev);
        exit(1);
    }

    udev_monitor_filter_add_match_subsystem_devtype(mon, "usb", NULL);
    udev_monitor_enable_receiving(mon);

    int fd = udev_monitor_get_fd(mon);

    int tmp = inicializar_estado(udev, cfg->udev_usb_path);

    if (tmp) {
        printf("Teclado conectado. Encendiendo eDP-2...\n");
        configurarmonitores("encender");
        usleep(250000); // sleep 0.25s
        poner_fondo_2_monitores();
    } 
    else {
        printf("Teclado desconectado. Apagando eDP-2...\n");
        configurarmonitores("apagar");
        usleep(250000);
        poner_fondo_1_monitor();
    }

    printf("Listening for USB events on port 3-6...\n");

    while (1)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);

        int ret = select(fd + 1, &fds, NULL, NULL, NULL);
        if (ret > 0 && FD_ISSET(fd, &fds))
        {
            struct udev_device *dev = udev_monitor_receive_device(mon);
            if (dev)
            {
                const char *action = udev_device_get_action(dev);
                const char *devnode = udev_device_get_devnode(dev);
                const char *devpath = udev_device_get_devpath(dev);

                if (devnode && !strcmp(devpath, cfg->udev_usb_path))
                {
                    if (!strcmp(action, "add"))
                    {
                        printf("Teclado conectado\n");

                        if (pantalla_edp2_encendida())
                        {
                            printf("El teclado no está conectado y eDP-2 está encendida. Apagando eDP-2...\n");
                            configurarmonitores("apagar");
                            usleep(250000);
                            poner_fondo_1_monitor();
                        }
                    }
                    else if (!strcmp(action, "remove"))
                    {
                        printf("Teclado desconectado\n");

                        if (!pantalla_edp2_encendida())
                        {
                            printf("El teclado está conectado y eDP-2 está apagada. Encendiendo eDP-2...\n");
                            configurarmonitores("encender");
                            usleep(250000);
                            poner_fondo_2_monitores();
                        }
                    }
                }

                udev_device_unref(dev);
            }
        }
    }

    udev_monitor_unref(mon);
    udev_unref(udev);

    return NULL;
}
