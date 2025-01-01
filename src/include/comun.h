#ifndef COMUN_H
#define COMUN_H

struct SConfiguracion
{
    char *modo_deteccion;
    char *bluetooth_mac_teclado;
    char *udev_usb_path;
    char *orientacion_bus;
    char *orientacion_path;
    char *orientacion_interfaz;
    char *pantalla_resolucion;
    char *pantalla_tasa_refresco;
    char *pantalla_fondo_edp1;
    char *pantalla_fondo_edp2;
    int pantalla_nivel_brillo;
    int teclado_nivel_brillo;
};

extern struct SConfiguracion *cfg;

int ejecutar_comando(const char *fmt, ...);

#endif
