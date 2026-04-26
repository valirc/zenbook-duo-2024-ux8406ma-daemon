/*
 * config.h — load and validate /etc/zbd/zbd.conf.
 */

#ifndef ZBD_CONFIG_H
#define ZBD_CONFIG_H

#include "comun.h"

/*
 * Parse and validate the configuration file at CONFIG_PATH (default
 * /etc/zbd/zbd.conf, overridable at compile time with -DCONFIG_PATH=...).
 *
 * On success populates the global `cfg` struct with strdup'd strings
 * and validated integers, and returns 0. The caller takes implicit
 * ownership of the struct via the global pointer; cfg_release() can
 * be used to free it on shutdown.
 *
 * On any validation failure (malformed MAC, non-absolute path,
 * out-of-range integer, unknown key) the function logs a precise
 * diagnostic to stderr and returns -1. The global `cfg` is rolled
 * back to NULL so partial state is never observable.
 */
int cargar_configuracion(void);

/*
 * Same as cargar_configuracion() but reads from `path` instead of
 * CONFIG_PATH. Useful for tests and for letting either binary accept
 * an explicit `--config /path/to/file` argument. Passing NULL falls
 * back to CONFIG_PATH.
 */
int cargar_configuracion_desde(const char *path);

/*
 * Free the global cfg struct. Idempotent.
 */
void cfg_release(void);

/*
 * Apply battery charge limit through sysfs. Validates the level is
 * in [20, 100] and writes to
 *   /sys/class/power_supply/BAT0/charge_control_end_threshold
 * directly (no shell). Returns EXIT_SUCCESS / EXIT_FAILURE.
 */
int limitar_carga_bateria(int nivel_bateria);

#endif /* ZBD_CONFIG_H */
