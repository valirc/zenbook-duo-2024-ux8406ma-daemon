# Changelog

All notable changes to **zbd** are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
