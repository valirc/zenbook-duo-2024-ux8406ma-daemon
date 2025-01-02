#ifndef PANTALLA_H
#define PANTALLA_H

int set_pantalla_brillo(int nivel_brillo);

void configurar_monitores(const char *accion);

int monitor_estado(const char *monitor_id);

void poner_fondo_2_monitores(void);

void poner_fondo_1_monitor(void);
#endif
