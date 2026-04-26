#ifndef ZBD_MONITOR_BLUETOOTH_H
#define ZBD_MONITOR_BLUETOOTH_H

/*
 * pthread entry point: spawn `bluetoothctl --monitor`, react to
 * "Connected: yes/no" lines on the configured MAC, and exit when the
 * shutdown flag is raised.
 */
void *monitorizar_cambios_bluetooth(void *arg);

/*
 * Synchronous probe: returns 1 if `bluetoothctl info <MAC>` reports
 * "Connected: yes" for the configured MAC, 0 otherwise.
 */
int teclado_conectado(void);

#endif /* ZBD_MONITOR_BLUETOOTH_H */
