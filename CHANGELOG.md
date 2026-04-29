# Changelog

All notable changes to **zbd** are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased] — 2026-04-29 (refactor/c-modernization)

### Added

- **`src/dash_to_panel.c` + `src/include/dash_to_panel.h`** — integration
  with the `dash-to-panel@jderose9.github.com` GNOME Shell extension:
  - `d2p_apply_visibility_settings()` forces D2P's `intellihide`,
    `intellihide-hide-from-windows` and (when present)
    `intellihide-hide-from-monitor-windows` to `false`, removes
    `hidetopbar@mathieu.bidon.ca` from `org.gnome.shell.enabled-extensions`
    and additionally sets its `enable-intellihide` to `false` so the bug
    cannot reappear if the user manually re-enables the extension. The
    `hidetopbar` extension was hiding the GNOME top bar — which D2P
    substitutes — on app maximisation, producing a bug indistinguishable
    from D2P's own intellihide.
  - `d2p_set_primary_monitor()` translates a DRM connector name to D2P's
    monitor id (`${vendor}-${serial}` with connector and index fall-backs
    on collision, faithfully reproducing
    `panelSettings.js::_saveMonitors()`), then writes
    `primary-monitor=<id>` and `multi-monitors=false`. Called whenever
    the user picks a primary monitor in the zbd-tray submenu.
  - Schema discovery uses `g_settings_schema_source_new_from_directory()`
    against `g_get_user_data_dir()/gnome-shell/extensions/<UUID>/schemas`
    plus every `g_get_system_data_dirs()` candidate, so user-local
    extension installs (the common case on Ubuntu 26.04) work without
    extra environment setup.
- **`dash_to_panel_gestionar`** config key in `/etc/zbd/zbd.conf`
  (default `true`). Set to `false` to leave the user's GNOME extension
  settings untouched.

### Fixed

- **"Monitor principal" submenu radio marker stuck on the previous
  selection** after a successful primary swap — labels reflected
  reality only after a DRM hotplug rebuilt the submenu.  Root cause:
  `gui_daemon.c::on_set_primary_monitor()` mutated the display backend
  but never updated the per-item `toggle-state`, while dbusmenu's
  `radio` toggle-type is only a rendering hint and the server must
  enforce mutual exclusion explicitly (the same pattern the
  brightness/keyboard radio groups already follow).  Fixed by adding
  `primary_submenu_sync_toggle()` and calling it after every click.
- **dash-to-panel left on the wrong monitor after primary swap** (e.g. eDP-1 → DP-2 click left D2P invisible / on eDP-1 with the menu marked DP-2).  Root cause: D2P listens on two signals that both rebuild the panel —
  `changed::primary-monitor` (synchronous) and `monitors-changed` (async, awaits `setMonitorsInfo` Mutter D-Bus round-trip before refreshing
  `monitorIdToIndex`).  Calling `gdctl set --primary` first and writing
  `primary-monitor` afterwards meant the synchronous handler ran while
  `Main.layoutManager.monitors` had already been re-ordered by Mutter
  but D2P's `monitorIdToIndex` cache had not — index lookup pointed at
  the wrong entry of the new array.  Fixed by inverting the order in
  `gui_daemon.c::on_set_primary_monitor()`: write `primary-monitor`
  first (both views still pre-gdctl, internally consistent), then run
  gdctl (`monitors-changed` triggers a second rebuild with both views
  refreshed, also internally consistent).
- **Stale comment in `systemd/user/zbd-tray.service`** still
  referencing `libayatana-appindicator-glib` — the tray uses a custom
  `org.kde.StatusNotifierItem` implementation (`src/tray_sni.c`) + 
  `libdbusmenu-glib` since commit `b74c937`. Comment refreshed.

### Wired

- `gui_daemon.c::on_set_primary_monitor()` now calls
  `d2p_set_primary_monitor(output)` BEFORE `display_set_primary(output)`
  (see Fixed for why the order matters), gated by
  `cfg->dash_to_panel_gestionar`, and `primary_submenu_sync_toggle()`
  after both to keep the radio-button marker in sync with the actual
  primary.
- `gui_daemon.c::main()` now calls `d2p_apply_visibility_settings()` and
  `d2p_set_primary_monitor(display_get_primary())` once after the
  dbusmenu is set up, gated by the same flag.

## [2.0.0] — 2026-04-26 (refactor/c-modernization)

A ground-up rework of the original 1.x daemon. The codebase is split,
hardened, tested, packaged and ready for both X11 and Wayland sessions
on Ubuntu 24.04 (`noble`) and 26.04 (`resolute`).

### Added

- **Two-binary privilege split**: `zbd-system` (root) handles the
  privileged operations (backlight, battery threshold, USB HID
  keyboard, DMIC), `zbd-tray` (user session) hosts the GTK tray
  and the runtime monitors (Bluetooth, USB, accelerometer).
- **D-Bus IPC** between tray and system service over the system bus
  (`org.anexa.zbd1`, interface `org.anexa.zbd1.System`). Implemented
  with `sd-bus` (libsystemd). The tray automatically routes through
  D-Bus when the system service is on the bus, and falls back to
  direct calls when running standalone (e.g. as root for debugging).
- **System bus policy** (`dbus/org.anexa.zbd.conf`): only root can
  own the bus name; only members of the `zbd` group can send.
- **Polkit policy** (`polkit/org.anexa.zbd.policy`) with four
  per-action authorisations (screen brightness, keyboard backlight,
  battery threshold, configure DMIC).
- **Pluggable display backends**: `xrandr` (X11) and `gdctl`
  (Wayland with GNOME ≥ 47, default on Ubuntu 26.04). Auto-detected
  from `XDG_SESSION_TYPE`/`XDG_CURRENT_DESKTOP`.
- **Functional accelerometer rotation**: orientation events now
  actually rotate the active outputs through the display backend
  (the previous implementation logged the orientation and did
  nothing).
- **Signal handling**: SIGINT/SIGTERM/SIGHUP trigger a graceful
  shutdown of the udev/Bluetooth monitors and the GMainLoop;
  SIGPIPE is ignored so a broken pipe (e.g. bluetoothctl exiting)
  no longer kills the daemon.
- **Strict configuration validation**: the loader checks every
  field (MAC regex, absolute paths, integer ranges, set membership
  for `modo_deteccion`) and rolls back the global `cfg` to NULL on
  any failure, so partial state is never observable.
- **Shell-free command execution**: a tiny `exec` module replaces
  every `system()`/`popen()` call with `fork+execvp`, removing the
  command-injection vector that the original `ejecutar_comando(fmt,
  ...)` helper exposed via /etc/zbd/zbd.conf.
- **systemd integration**: `systemd/zbd-system.service`
  (`Type=dbus`) and `systemd/user/zbd-tray.service` (bound to
  `graphical-session.target`), both heavily sandboxed
  (`ProtectSystem=strict`, `NoNewPrivileges`,
  `MemoryDenyWriteExecute`, `RestrictAddressFamilies=AF_UNIX`,
  capability bounding sets, syscall filters).
- **Modern Makefile**: targets `make`, `make release`, `make
  install`, `make uninstall`, `make clean`, `make test`,
  `make format`, `make lint`, `make check`, `make deb`. Release
  builds use `-O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2
  -fPIE -pie -Wl,-z,relro,-z,now,-z,noexecstack`. Strict
  warnings: `-Wall -Wextra -Wformat=2 -Wformat-security
  -Wshadow -Wstrict-prototypes -Wmissing-prototypes
  -Wpointer-arith -Wcast-align -Wnull-dereference`.
- **Unit tests** with Criterion (`tests/test_config.c`, 14 cases
  covering valid configs, MAC validation, path validation, integer
  range validation, modo_deteccion membership, comments, whitespace
  handling, and `cfg_release` idempotency).
- **GitHub Actions CI** (`.github/workflows/ci.yml`): build matrix
  for `ubuntu-24.04` × `{debug, release}` and `ubuntu-latest` ×
  `{debug, release}`, with `make test` and ELF hardening assertions
  on the release lanes; lint job (clang-format, clang-tidy,
  cppcheck); systemd-analyze validation of every shipped unit.
- **`.clang-format`** with the project style (4-space indent,
  Allman braces, ColumnLimit=100, pointer-aligned right).
- **Debian packaging** (`debian/`): `make deb` produces a working
  `.deb` (≈ 491 KB) with proper Depends, Recommends, hardened
  build flags, postinst that enables the system service, postrm
  that cleans up on purge.
- **Documentation**: comprehensive `README.md` (hardware target,
  architecture diagram, build, install, configuration, display
  backends, privilege model, troubleshooting), `CONTRIBUTING.md`
  (workflow, code style, commit-message convention), `LICENSE`
  (canonical FSF GPL-3.0-or-later text), `.editorconfig`.

### Fixed

- `main.c`: removed a stray `pthread_join` on an uninitialised
  `pthread_t` *before* `pthread_create`. Both worker threads are
  now joined at the end of the daemon block.
- `monitor_teclado_usb.c`: corrected the inverted on-attach /
  on-detach branches in the udev event loop (they were the opposite
  of the initial-state logic and of the Bluetooth monitor — i.e. the
  primary user-visible bug of the 1.x daemon was that detaching the
  keyboard turned eDP-2 *on* instead of off).
- `audio.c` / `audio.h`: aligned return type (`int` everywhere);
  pactl errors now propagate as `EXIT_FAILURE`. `main.c` used to
  call `configurar_dmic_raw()` via implicit declaration.
- `gui_daemon.c`: only one keyboard monitor (Bluetooth or USB,
  depending on `modo_deteccion`) is spawned at a time; the previous
  implementation always launched both, causing them to fight for
  control of eDP-2.
- `monitor_orientacion.c`: pthread workers used to return
  `EXIT_FAILURE` cast to `void *`, which is undefined behaviour;
  now return `NULL` as pthreads expect.

### Changed

- The original `zbd` binary is renamed `zbd-system` and installed
  in `$(SBIN_DIR)`; `zbd-tray` is installed in `$(BIN_DIR)`.
- The icon path moves from `/usr/share/icons/gmam/icono.svg`
  (which leaked the author's local layout) to
  `/usr/share/icons/zbd/zbd-tray.svg`.
- The DRM `card1` hard-code remains for now (still correct on the
  UX8406MA), but the wallpaper backend abstraction makes a
  per-backend override easy to add later.
- The configuration loader is now pure: parsing no longer triggers
  any side effects on the hardware. Callers apply the loaded values
  explicitly afterwards.

### Removed

- `src/comun.c` / the `ejecutar_comando(fmt, ...)` helper. It was
  the last shell-injecting code path; every caller has migrated to
  `exec.h`'s `exec_cmd_argv` / `exec_cmd_pipe`.
- The `make all` target no longer implicitly installs into `/etc`
  and `/usr/local`; install is a separate, deliberate target.
- Compiled binaries and intermediate object files are no longer
  versioned; they are regenerated by `make`.

### Security notes

- Command-injection vector via `/etc/zbd/zbd.conf` removed
  (no shell is involved anywhere in the daemon).
- The privileged binary now runs as a sandboxed systemd service
  with `CAP_SYS_ADMIN` only, no other capabilities, no network,
  read-only filesystem except for two narrow `ReadWritePaths`.
- Configuration validation rejects malformed values before they
  reach the kernel sysfs interface or the keyboard HID protocol.
- Polkit governs which active session user can perform which
  privileged operation, with sticky admin auth required for
  battery threshold changes by default.

### Known follow-ups

- `polkit::CheckAuthorization` enforcement inside the C method
  handlers (the bus-level gate already restricts callers; this
  belt-and-braces enforcement is a follow-up).
- Per-output Wayland wallpapers via gsettings + a GNOME extension
  (the `gdctl` backend currently delegates wallpapers to `feh`,
  which still works correctly via XWayland).
- Make `card1` discoverable instead of hard-coded.
- Auto-detect the keyboard USB VID/PID instead of hard-coding it.
