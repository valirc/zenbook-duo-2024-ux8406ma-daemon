#pragma once

/* Initialise the StatusNotifierItem D-Bus registration.
 * Call once, before g_main_loop_run().  Owns a bus name asynchronously;
 * the SNI is visible to the shell extension after the main loop starts. */
void tray_sni_init(const char *icon_name, const char *title);

/* D-Bus object path where the DbusmenuServer must be registered. */
const char *tray_sni_menu_path(void);
