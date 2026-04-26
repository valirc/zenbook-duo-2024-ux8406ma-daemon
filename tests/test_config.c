/*
 * tests/test_config.c — unit tests for the configuration parser.
 *
 * Uses Criterion (https://github.com/Snaipe/Criterion). Each TestSuite
 * helper creates a temporary config file and feeds it to
 * cargar_configuracion_desde(). The global cfg struct is the
 * observable side effect.
 */

#include <criterion/criterion.h>
#include <criterion/redirect.h>

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "comun.h"
#include "config.h"

/* Each test gets its own temporary config file. */
static char tmp_path[PATH_MAX];
static FILE *tmp_fp = NULL;

static void make_tmpfile(void)
{
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/zbd-test-XXXXXX");
    int fd = mkstemp(tmp_path);
    cr_assert_geq(fd, 0, "mkstemp failed");
    tmp_fp = fdopen(fd, "w");
    cr_assert_not_null(tmp_fp);
}

static void close_and_unlink(void)
{
    if (tmp_fp) { fclose(tmp_fp); tmp_fp = NULL; }
    if (tmp_path[0]) { unlink(tmp_path); tmp_path[0] = '\0'; }
    cfg_release();
}

/* Wallpaper paths used in valid fixtures need to actually exist on
 * disk so the file_readable() validation passes. We touch a couple
 * under /tmp at suite setup. */
static char wallpaper1[PATH_MAX];
static char wallpaper2[PATH_MAX];

static void touch_wallpapers(void)
{
    snprintf(wallpaper1, sizeof(wallpaper1), "/tmp/zbd-wp1-XXXXXX.jpg");
    snprintf(wallpaper2, sizeof(wallpaper2), "/tmp/zbd-wp2-XXXXXX.jpg");
    int fd1 = mkstemps(wallpaper1, 4);
    int fd2 = mkstemps(wallpaper2, 4);
    cr_assert_geq(fd1, 0);
    cr_assert_geq(fd2, 0);
    close(fd1); close(fd2);
}

static void unlink_wallpapers(void)
{
    if (wallpaper1[0]) unlink(wallpaper1);
    if (wallpaper2[0]) unlink(wallpaper2);
}

TestSuite(config, .init = touch_wallpapers, .fini = unlink_wallpapers);

/* ----------------------------------------------------------------- *
 *  Valid configuration                                              *
 * ----------------------------------------------------------------- */

Test(config, valid_minimal, .fini = close_and_unlink)
{
    make_tmpfile();
    fprintf(tmp_fp,
        "modo_deteccion=udev\n"
        "bluetooth_mac_teclado=AA:BB:CC:DD:EE:FF\n"
        "udev_usb_path=/devices/pci0000:00/0000:00:14.0/usb3/3-6\n"
        "orientacion_bus=net.hadess.SensorProxy\n"
        "orientacion_path=/net/hadess/SensorProxy\n"
        "orientacion_interfaz=net.hadess.SensorProxy\n"
        "pantalla_resolucion=2880x1800\n"
        "pantalla_tasa_refresco=120\n"
        "pantalla_fondo_edp1=%s\n"
        "pantalla_fondo_edp2=%s\n"
        "pantalla_nivel_brillo=10\n"
        "teclado_nivel_brillo=1\n"
        "bateria_carga_maxima=80\n",
        wallpaper1, wallpaper2);
    fclose(tmp_fp); tmp_fp = NULL;

    cr_redirect_stderr();
    int rc = cargar_configuracion_desde(tmp_path);
    cr_assert_eq(rc, 0, "expected success");
    cr_assert_not_null(cfg);
    cr_assert_str_eq(cfg->modo_deteccion, "udev");
    cr_assert_str_eq(cfg->bluetooth_mac_teclado, "AA:BB:CC:DD:EE:FF");
    cr_assert_str_eq(cfg->pantalla_resolucion, "2880x1800");
    cr_assert_eq(cfg->pantalla_nivel_brillo, 10);
    cr_assert_eq(cfg->teclado_nivel_brillo, 1);
    cr_assert_eq(cfg->bateria_carga_maxima, 80);
}

Test(config, comments_and_blank_lines_skipped, .fini = close_and_unlink)
{
    make_tmpfile();
    fprintf(tmp_fp,
        "# leading comment\n"
        "\n"
        "modo_deteccion=bluetooth\n"
        "   # indented comment\n"
        "\n"
        "bluetooth_mac_teclado=00:11:22:33:44:55\n");
    fclose(tmp_fp); tmp_fp = NULL;

    cr_redirect_stderr();
    int rc = cargar_configuracion_desde(tmp_path);
    cr_assert_eq(rc, 0);
    cr_assert_str_eq(cfg->modo_deteccion, "bluetooth");
    cr_assert_str_eq(cfg->bluetooth_mac_teclado, "00:11:22:33:44:55");
}

Test(config, whitespace_around_equals_is_trimmed, .fini = close_and_unlink)
{
    make_tmpfile();
    fprintf(tmp_fp,
        "modo_deteccion   =\tudev\n"
        "bluetooth_mac_teclado= AA:BB:CC:DD:EE:FF \n");
    fclose(tmp_fp); tmp_fp = NULL;

    cr_redirect_stderr();
    int rc = cargar_configuracion_desde(tmp_path);
    cr_assert_eq(rc, 0);
    cr_assert_str_eq(cfg->modo_deteccion, "udev");
    cr_assert_str_eq(cfg->bluetooth_mac_teclado, "AA:BB:CC:DD:EE:FF");
}

/* ----------------------------------------------------------------- *
 *  Invalid configuration: must fail and roll cfg back to NULL       *
 * ----------------------------------------------------------------- */

Test(config, rejects_invalid_mac, .fini = close_and_unlink)
{
    make_tmpfile();
    fprintf(tmp_fp,
        "bluetooth_mac_teclado=NOT-A-MAC\n");
    fclose(tmp_fp); tmp_fp = NULL;

    cr_redirect_stderr();
    int rc = cargar_configuracion_desde(tmp_path);
    cr_assert_neq(rc, 0, "MAC invalid should fail");
    cr_assert_null(cfg, "cfg must be rolled back on failure");
}

Test(config, rejects_non_absolute_udev_path, .fini = close_and_unlink)
{
    make_tmpfile();
    fprintf(tmp_fp, "udev_usb_path=relative/path\n");
    fclose(tmp_fp); tmp_fp = NULL;

    cr_redirect_stderr();
    int rc = cargar_configuracion_desde(tmp_path);
    cr_assert_neq(rc, 0);
    cr_assert_null(cfg);
}

Test(config, rejects_resolution_without_x, .fini = close_and_unlink)
{
    make_tmpfile();
    fprintf(tmp_fp, "pantalla_resolucion=2880-1800\n");
    fclose(tmp_fp); tmp_fp = NULL;

    cr_redirect_stderr();
    int rc = cargar_configuracion_desde(tmp_path);
    cr_assert_neq(rc, 0);
    cr_assert_null(cfg);
}

Test(config, rejects_brightness_out_of_range, .fini = close_and_unlink)
{
    make_tmpfile();
    fprintf(tmp_fp, "pantalla_nivel_brillo=200\n");
    fclose(tmp_fp); tmp_fp = NULL;

    cr_redirect_stderr();
    int rc = cargar_configuracion_desde(tmp_path);
    cr_assert_neq(rc, 0);
    cr_assert_null(cfg);
}

Test(config, rejects_battery_threshold_below_minimum, .fini = close_and_unlink)
{
    make_tmpfile();
    fprintf(tmp_fp, "bateria_carga_maxima=10\n");
    fclose(tmp_fp); tmp_fp = NULL;

    cr_redirect_stderr();
    int rc = cargar_configuracion_desde(tmp_path);
    cr_assert_neq(rc, 0);
    cr_assert_null(cfg);
}

Test(config, rejects_keyboard_brightness_too_high, .fini = close_and_unlink)
{
    make_tmpfile();
    fprintf(tmp_fp, "teclado_nivel_brillo=4\n");
    fclose(tmp_fp); tmp_fp = NULL;

    cr_redirect_stderr();
    int rc = cargar_configuracion_desde(tmp_path);
    cr_assert_neq(rc, 0);
    cr_assert_null(cfg);
}

Test(config, rejects_modo_deteccion_unknown, .fini = close_and_unlink)
{
    make_tmpfile();
    fprintf(tmp_fp, "modo_deteccion=ouija\n");
    fclose(tmp_fp); tmp_fp = NULL;

    cr_redirect_stderr();
    int rc = cargar_configuracion_desde(tmp_path);
    cr_assert_neq(rc, 0);
    cr_assert_null(cfg);
}

Test(config, rejects_line_without_equals_sign, .fini = close_and_unlink)
{
    make_tmpfile();
    fprintf(tmp_fp,
        "modo_deteccion=udev\n"
        "this is not a valid line\n");
    fclose(tmp_fp); tmp_fp = NULL;

    cr_redirect_stderr();
    int rc = cargar_configuracion_desde(tmp_path);
    cr_assert_neq(rc, 0);
    cr_assert_null(cfg);
}

Test(config, missing_file_returns_error, .fini = close_and_unlink)
{
    cr_redirect_stderr();
    int rc = cargar_configuracion_desde("/tmp/this-file-does-not-exist-XXXXX");
    cr_assert_neq(rc, 0);
    cr_assert_null(cfg);
}

/* ----------------------------------------------------------------- *
 *  Unknown keys are non-fatal warnings                              *
 * ----------------------------------------------------------------- */

Test(config, unknown_key_is_warning_not_fatal, .fini = close_and_unlink)
{
    make_tmpfile();
    fprintf(tmp_fp,
        "modo_deteccion=udev\n"
        "key_que_no_existe=algo\n");
    fclose(tmp_fp); tmp_fp = NULL;

    cr_redirect_stderr();
    int rc = cargar_configuracion_desde(tmp_path);
    cr_assert_eq(rc, 0, "unknown keys must not fail the load");
    cr_assert_not_null(cfg);
}

/* ----------------------------------------------------------------- *
 *  cfg_release is idempotent                                        *
 * ----------------------------------------------------------------- */

Test(config, cfg_release_is_idempotent)
{
    cfg_release();
    cfg_release();
    cr_assert_null(cfg);
}
