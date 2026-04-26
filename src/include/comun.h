/*
 * comun.h — daemon-wide configuration struct and process-global state.
 *
 * The legacy `int ejecutar_comando(const char *fmt, ...)` helper that
 * lived here has been removed: every caller now goes through
 * exec.h's exec_cmd_argv()/exec_cmd_pipe() helpers, which fork+execvp
 * with an explicit argv vector and never spawn a shell.
 */

#ifndef ZBD_COMUN_H
#define ZBD_COMUN_H

struct SConfiguracion
{
    char *modo_deteccion;          /* "udev" | "bluetooth" | "both" */
    char *bluetooth_mac_teclado;   /* "AA:BB:CC:DD:EE:FF" */
    char *udev_usb_path;           /* /devices/.../usbN/N-N */
    char *orientacion_bus;         /* D-Bus name, typically net.hadess.SensorProxy */
    char *orientacion_path;        /* D-Bus object path */
    char *orientacion_interfaz;    /* D-Bus interface */
    char *pantalla_resolucion;     /* "WIDTHxHEIGHT" */
    char *pantalla_tasa_refresco;  /* numeric refresh rate, e.g. "120" */
    char *pantalla_fondo_edp1;     /* absolute path */
    char *pantalla_fondo_edp2;     /* absolute path */
    int   pantalla_nivel_brillo;   /* 10..100, daemon multiplies by 4 */
    int   teclado_nivel_brillo;    /* 0..3 */
    int   bateria_carga_maxima;    /* 20..100 */
};

extern struct SConfiguracion *cfg;

#endif /* ZBD_COMUN_H */
