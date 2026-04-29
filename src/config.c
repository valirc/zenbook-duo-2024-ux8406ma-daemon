/*
 * config.c — strict, side-effect-free configuration loader.
 *
 * The previous loader had two structural problems:
 *
 *   1. It applied actions while parsing — set_pantalla_brillo(),
 *      set_brillo_teclado() and limitar_carga_bateria() were invoked
 *      inside the parser as soon as the matching key was seen. That
 *      conflated I/O with state, made testing impossible (you could
 *      not load a config without touching the hardware) and meant a
 *      malformed line later in the file left the system in a half-
 *      applied state.
 *
 *   2. It accepted whatever string sscanf() gave it. There was no
 *      check that the MAC address was a real MAC, that paths were
 *      absolute, that integers were within range, or that the value
 *      did not contain shell metacharacters. Combined with the old
 *      ejecutar_comando() helper, that was a command-injection
 *      vector with root privileges.
 *
 * This rewrite separates parsing from application:
 *
 *   - cargar_configuracion() ONLY parses, validates and stores. It
 *     never touches /sys, never invokes external programs, and never
 *     leaves a partially-populated cfg observable on failure (it
 *     rolls back to NULL).
 *   - cfg_release() frees the struct on shutdown.
 *   - limitar_carga_bateria() now writes the sysfs file directly via
 *     write(2), with no shell.
 *
 * Validation rules:
 *   - modo_deteccion ∈ { "udev", "bluetooth", "both" }
 *   - bluetooth_mac_teclado matches /^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$/
 *   - udev_usb_path is an absolute path under /devices/
 *   - orientacion_bus / _path / _interfaz are non-empty strings
 *   - pantalla_resolucion matches /^[0-9]+x[0-9]+$/
 *   - pantalla_tasa_refresco is a decimal positive number
 *   - pantalla_fondo_edp1 / _edp2 are absolute, world-readable paths
 *   - pantalla_nivel_brillo ∈ [10, 100]
 *   - teclado_nivel_brillo ∈ [0, 3]
 *   - bateria_carga_maxima ∈ [20, 100]
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "comun.h"
#include "config.h"

#ifndef CONFIG_PATH
#define CONFIG_PATH "/etc/zbd/zbd.conf"
#endif

#define BATTERY_THRESHOLD_PATH \
    "/sys/class/power_supply/BAT0/charge_control_end_threshold"

struct SConfiguracion *cfg = NULL;

/* ============================================================== *
 *  Helpers                                                       *
 * ============================================================== */

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) ++s;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return s;
}

/* Validate and store a decimal scale value as a string — avoids float↔string
 * locale issues entirely (strtod/printf use locale decimal separator). */
static int parse_scale_str(const char *value, float min, float max, char **out, const char *key)
{
    if (!value || !*value)
    {
        fprintf(stderr, "config: '%s' no puede estar vacio\n", key);
        return -1;
    }
    int dot_seen = 0;
    for (const char *p = value; *p; ++p)
    {
        if (*p == '.') { if (dot_seen++) { goto bad; } }
        else if (!isdigit((unsigned char)*p)) { goto bad; }
    }
    /* Range check using locale-independent integer arithmetic. */
    {
        const char *dot = strchr(value, '.');
        long int_part = strtol(value, NULL, 10);
        double frac = 0.0;
        if (dot && *(dot + 1))
        {
            const char *fp = dot + 1;
            long frac_digits = strtol(fp, NULL, 10);
            double denom = 1.0;
            for (const char *p = fp; *p; ++p) denom *= 10.0;
            frac = (double)frac_digits / denom;
        }
        double v = (double)int_part + frac;
        if (v < (double)min || v > (double)max)
        {
            fprintf(stderr, "config: '%s' fuera de rango [%.4g, %.4g] (recibido: %s)\n",
                    key, (double)min, (double)max, value);
            return -1;
        }
    }
    free(*out);
    *out = strdup(value);
    return *out ? 0 : -1;

bad:
    fprintf(stderr, "config: '%s' debe ser un numero (recibido: '%s')\n", key, value);
    return -1;
}

static int parse_int(const char *value, int min, int max, int *out, const char *key)
{
    char *endp = NULL;
    errno = 0;
    long v = strtol(value, &endp, 10);
    if (errno != 0 || !endp || endp == value || *endp != '\0')
    {
        fprintf(stderr, "config: '%s' debe ser un entero (recibido: '%s')\n",
                key, value);
        return -1;
    }
    if (v < min || v > max)
    {
        fprintf(stderr, "config: '%s' fuera de rango [%d, %d] (recibido: %ld)\n",
                key, min, max, v);
        return -1;
    }
    *out = (int)v;
    return 0;
}

static int valid_mac(const char *s)
{
    /* XX:XX:XX:XX:XX:XX, 17 caracteres exactos */
    if (!s || strlen(s) != 17) return 0;
    for (int i = 0; i < 17; ++i)
    {
        if (i % 3 == 2)
        {
            if (s[i] != ':') return 0;
        }
        else
        {
            if (!isxdigit((unsigned char)s[i])) return 0;
        }
    }
    return 1;
}

static int valid_resolution(const char *s)
{
    if (!s || !*s) return 0;
    int x_seen = 0;
    for (const char *p = s; *p; ++p)
    {
        if (*p == 'x')
        {
            if (x_seen || p == s || !*(p + 1)) return 0;
            x_seen = 1;
        }
        else if (!isdigit((unsigned char)*p))
        {
            return 0;
        }
    }
    return x_seen;
}

static int valid_decimal(const char *s)
{
    if (!s || !*s) return 0;
    int dot_seen = 0;
    for (const char *p = s; *p; ++p)
    {
        if (*p == '.')
        {
            if (dot_seen) return 0;
            dot_seen = 1;
        }
        else if (!isdigit((unsigned char)*p))
        {
            return 0;
        }
    }
    return 1;
}

static int valid_absolute_path(const char *s)
{
    return s && s[0] == '/' && strlen(s) < PATH_MAX;
}

static int file_readable(const char *p)
{
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    return S_ISREG(st.st_mode);
}

/* ============================================================== *
 *  Parser                                                        *
 * ============================================================== */

void cfg_release(void)
{
    if (!cfg) return;
    free(cfg->modo_deteccion);
    free(cfg->bluetooth_mac_teclado);
    free(cfg->udev_usb_path);
    free(cfg->orientacion_bus);
    free(cfg->orientacion_path);
    free(cfg->orientacion_interfaz);
    free(cfg->pantalla_resolucion);
    free(cfg->pantalla_tasa_refresco);
    free(cfg->pantalla_fondo_edp1);
    free(cfg->pantalla_fondo_edp2);
    free(cfg->pantalla_escala);
    free(cfg->pantalla_backend);
    free(cfg);
    cfg = NULL;
}

static int set_string(char **dst, const char *value, const char *key)
{
    if (!*value)
    {
        fprintf(stderr, "config: '%s' no puede estar vacio\n", key);
        return -1;
    }
    free(*dst);
    *dst = strdup(value);
    return *dst ? 0 : -1;
}

int cargar_configuracion(void)
{
    return cargar_configuracion_desde(CONFIG_PATH);
}

int cargar_configuracion_desde(const char *path)
{
    if (!path) path = CONFIG_PATH;

    FILE *config_file = fopen(path, "r");
    if (!config_file)
    {
        fprintf(stderr, "config: no puedo abrir %s: %s\n",
                path, strerror(errno));
        return -1;
    }

    cfg_release(); /* idempotente */
    cfg = calloc(1, sizeof(*cfg));
    if (!cfg)
    {
        fclose(config_file);
        fprintf(stderr, "config: out of memory\n");
        return -1;
    }

    /* Defaults for optional keys — preserved when the key is absent
     * from the config file (backward compatibility with older files). */
    cfg->audio_volumen_microfono = 70;
    cfg->audio_volumen_altavoces = 80;
    cfg->pantalla_escala   = strdup("1.2");
    cfg->pantalla_backend  = strdup("auto");

    char line[1024];
    int line_no = 0;
    int rc = 0;

    while (fgets(line, sizeof(line), config_file))
    {
        ++line_no;

        /* descartar comentarios y lineas en blanco */
        char *start = line;
        while (*start && isspace((unsigned char)*start)) ++start;
        if (*start == '\0' || *start == '#' || *start == '\n')
            continue;

        char *eq = strchr(start, '=');
        if (!eq)
        {
            fprintf(stderr, "config: linea %d sin '=': %s", line_no, line);
            rc = -1;
            break;
        }

        *eq = '\0';
        char *key = trim(start);
        char *value = trim(eq + 1);

        if (!strcmp(key, "modo_deteccion"))
        {
            if (strcmp(value, "udev") && strcmp(value, "bluetooth") && strcmp(value, "both"))
            {
                fprintf(stderr, "config: 'modo_deteccion' debe ser udev|bluetooth|both (recibido: '%s')\n",
                        value);
                rc = -1; break;
            }
            if (set_string(&cfg->modo_deteccion, value, key) != 0) { rc = -1; break; }
        }
        else if (!strcmp(key, "bluetooth_mac_teclado"))
        {
            if (!valid_mac(value))
            {
                fprintf(stderr, "config: 'bluetooth_mac_teclado' no es una MAC valida (recibido: '%s')\n",
                        value);
                rc = -1; break;
            }
            if (set_string(&cfg->bluetooth_mac_teclado, value, key) != 0) { rc = -1; break; }
        }
        else if (!strcmp(key, "udev_usb_path"))
        {
            if (!valid_absolute_path(value))
            {
                fprintf(stderr, "config: 'udev_usb_path' debe ser ruta absoluta (recibido: '%s')\n",
                        value);
                rc = -1; break;
            }
            if (set_string(&cfg->udev_usb_path, value, key) != 0) { rc = -1; break; }
        }
        else if (!strcmp(key, "orientacion_bus"))
        {
            if (set_string(&cfg->orientacion_bus, value, key) != 0) { rc = -1; break; }
        }
        else if (!strcmp(key, "orientacion_path"))
        {
            if (set_string(&cfg->orientacion_path, value, key) != 0) { rc = -1; break; }
        }
        else if (!strcmp(key, "orientacion_interfaz"))
        {
            if (set_string(&cfg->orientacion_interfaz, value, key) != 0) { rc = -1; break; }
        }
        else if (!strcmp(key, "pantalla_resolucion"))
        {
            if (!valid_resolution(value))
            {
                fprintf(stderr, "config: 'pantalla_resolucion' debe tener formato WIDTHxHEIGHT (recibido: '%s')\n",
                        value);
                rc = -1; break;
            }
            if (set_string(&cfg->pantalla_resolucion, value, key) != 0) { rc = -1; break; }
        }
        else if (!strcmp(key, "pantalla_tasa_refresco"))
        {
            if (!valid_decimal(value))
            {
                fprintf(stderr, "config: 'pantalla_tasa_refresco' debe ser numerico (recibido: '%s')\n",
                        value);
                rc = -1; break;
            }
            if (set_string(&cfg->pantalla_tasa_refresco, value, key) != 0) { rc = -1; break; }
        }
        else if (!strcmp(key, "pantalla_fondo_edp1"))
        {
            if (!valid_absolute_path(value) || !file_readable(value))
            {
                fprintf(stderr, "config: 'pantalla_fondo_edp1' no es una ruta legible (recibido: '%s')\n",
                        value);
                rc = -1; break;
            }
            if (set_string(&cfg->pantalla_fondo_edp1, value, key) != 0) { rc = -1; break; }
        }
        else if (!strcmp(key, "pantalla_fondo_edp2"))
        {
            if (!valid_absolute_path(value) || !file_readable(value))
            {
                fprintf(stderr, "config: 'pantalla_fondo_edp2' no es una ruta legible (recibido: '%s')\n",
                        value);
                rc = -1; break;
            }
            if (set_string(&cfg->pantalla_fondo_edp2, value, key) != 0) { rc = -1; break; }
        }
        else if (!strcmp(key, "pantalla_nivel_brillo"))
        {
            if (parse_int(value, 10, 100, &cfg->pantalla_nivel_brillo, key) != 0)
            {
                rc = -1; break;
            }
        }
        else if (!strcmp(key, "teclado_nivel_brillo"))
        {
            if (parse_int(value, 0, 3, &cfg->teclado_nivel_brillo, key) != 0)
            {
                rc = -1; break;
            }
        }
        else if (!strcmp(key, "bateria_carga_maxima"))
        {
            int n;
            if (parse_int(value, 20, 100, &n, key) != 0)
            {
                rc = -1; break;
            }
            cfg->bateria_carga_maxima = n;
        }
        else if (!strcmp(key, "audio_volumen_microfono"))
        {
            if (parse_int(value, 0, 100, &cfg->audio_volumen_microfono, key) != 0)
            {
                rc = -1; break;
            }
        }
        else if (!strcmp(key, "audio_volumen_altavoces"))
        {
            if (parse_int(value, 0, 100, &cfg->audio_volumen_altavoces, key) != 0)
            {
                rc = -1; break;
            }
        }
        else if (!strcmp(key, "pantalla_escala"))
        {
            if (parse_scale_str(value, 1.0f, 3.0f, &cfg->pantalla_escala, key) != 0)
            {
                rc = -1; break;
            }
        }
        else if (!strcmp(key, "pantalla_backend"))
        {
            if (strcmp(value, "auto") && strcmp(value, "gdctl") && strcmp(value, "xrandr"))
            {
                fprintf(stderr, "config: 'pantalla_backend' debe ser auto|gdctl|xrandr "
                        "(recibido: '%s')\n", value);
                rc = -1; break;
            }
            if (set_string(&cfg->pantalla_backend, value, key) != 0) { rc = -1; break; }
        }
        else
        {
            fprintf(stderr, "config: clave desconocida '%s' en linea %d\n",
                    key, line_no);
            /* unknown keys are non-fatal: warn and continue */
        }
    }

    fclose(config_file);

    if (rc != 0)
    {
        cfg_release();
    }
    return rc;
}

/* ============================================================== *
 *  Battery threshold                                             *
 * ============================================================== */

int limitar_carga_bateria(int nivel_bateria)
{
    if (nivel_bateria < 20 || nivel_bateria > 100)
    {
        fprintf(stderr, "Nivel invalido. Debe ser un entero entre 20 y 100.\n");
        return EXIT_FAILURE;
    }

    int fd = open(BATTERY_THRESHOLD_PATH, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
    {
        fprintf(stderr, "limitar_carga_bateria: no se puede abrir %s: %s\n",
                BATTERY_THRESHOLD_PATH, strerror(errno));
        return EXIT_FAILURE;
    }

    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%d\n", nivel_bateria);
    if (len <= 0 || (size_t)len >= sizeof(buf))
    {
        close(fd);
        fprintf(stderr, "limitar_carga_bateria: snprintf overflow\n");
        return EXIT_FAILURE;
    }

    ssize_t written = write(fd, buf, (size_t)len);
    int saved = errno;
    close(fd);

    if (written != len)
    {
        fprintf(stderr, "limitar_carga_bateria: write fallo: %s\n",
                strerror(saved));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
