# zbd — Zenbook Duo Daemon

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Hardware](https://img.shields.io/badge/hardware-ASUS%20ZenBook%20Duo%202024%20%28UX8406MA%29-orange)](#hardware-target)

`zbd` is a small Linux userspace daemon that fills the functionality
gap between the upstream kernel/desktop stack and the **ASUS ZenBook
Duo 2024 (UX8406MA)** — a laptop with two stacked OLED 2880×1800 @
120 Hz panels and a detachable Bluetooth/USB keyboard.

It is split into two binaries on purpose:

| Binary       | Privileges       | Responsibility                                          |
|--------------|------------------|---------------------------------------------------------|
| `zbd-system` | root + caps      | Backlight, battery threshold, USB HID keyboard control  |
| `zbd-tray`   | regular user     | GUI tray, monitors (BT/USB/orientation), display config |

The two halves talk over a small D-Bus interface protected by polkit,
so the user-facing GUI never has to hold root privileges and the
daemon never has to know about the user’s display server, wallpapers
or rotation preferences.

---

## Table of contents

1. [Hardware target](#hardware-target)
2. [What it actually does](#what-it-actually-does)
3. [Architecture](#architecture)
4. [Build](#build)
5. [Install](#install)
6. [Configuration](#configuration)
7. [Display backends](#display-backends)
8. [Privilege model](#privilege-model)
9. [Troubleshooting](#troubleshooting)
10. [Development](#development)
11. [License](#license)

---

## Hardware target

| Component | Detail |
|---|---|
| Model | ASUS ZenBook Duo 2024, BIOS family `UX8406MA` |
| Displays | `eDP-1` (top, primary) + `eDP-2` (bottom) — both OLED 2880×1800 @ 120 Hz |
| DRM card | `card1` (Intel Meteor Lake / Lunar Lake iGPU) |
| Keyboard (attached, USB) | VID `0x0B05`, PID `0x1B2C`, sysfs path `usb3/3-6` |
| Keyboard (detached, BT) | configurable MAC, factory default unique per unit |
| Backlight | `intel_backlight`, max ≈ 400 |
| Battery | `BAT0`, supports `charge_control_end_threshold` |
| Microphone | Intel Smart Sound DMIC, ALSA `hw:0,6`, s16le, 2 channels |
| Accelerometer | exposed by `iio-sensor-proxy` over D-Bus `net.hadess.SensorProxy` |

Other ZenBook Duo revisions or stacked-display ASUS laptops may work
with adjusted configuration, but only the UX8406MA is exercised in
practice. Patches for related models are welcome.

---

## What it actually does

When **the keyboard is attached on top of the bottom panel** the user
typically wants the bottom display *off*: it is hidden under the
keyboard. When the keyboard is detached and reconnected over
Bluetooth, both displays should be active and showing one half of a
stacked desktop each.

`zbd-tray` watches the keyboard transition in two ways simultaneously
(or one of them, depending on `modo_deteccion` in the config):

- **udev**: a netlink monitor on the USB subsystem catches `add`/
  `remove` events on the configured port path. This handles the
  physical attach/detach event reliably.
- **bluez**: `bluetoothctl --monitor` is parsed for `Connected: yes`
  / `Connected: no` lines on the keyboard MAC. This handles power
  cycles of the detached keyboard.

When the keyboard transitions, `zbd-tray` asks the configured
**display backend** to enable or disable `eDP-2`, set the wallpapers
(one for one-display mode, two for two-display mode) and apply the
configured brightness levels. Brightness, battery threshold and
keyboard-backlight changes are forwarded to `zbd-system` over D-Bus
because they need root or `CAP_SYS_ADMIN`.

The accelerometer monitor listens to `org.freedesktop.SensorProxy`
property changes and, when configured, rotates the active output so
the top display is up.

The audio module loads a `module-alsa-source` for the DMIC and makes
it the default source — this is a one-shot operation invoked once at
tray startup, because PipeWire / PulseAudio do not always pick up
this DSP-routed device automatically on Meteor Lake / Lunar Lake.

---

## Architecture

```
                        +-----------------------------+
                        |        /etc/zbd/zbd.conf    |
                        +--------------+--------------+
                                       |
                                       v
   +---------------------+    D-Bus    +-----------------------+
   |     zbd-tray        | <---------> |     zbd-system        |
   |   (user session)    |   polkit    |  (PID 1, root+caps)   |
   +---------+-----------+             +-----------+-----------+
             |                                     |
             |  GTK3 + AppIndicator                | sysfs:
             |  - tray menu (brightness, mode)     |   /sys/class/backlight/...
             |  - threads:                         |   /sys/class/power_supply/...
             |     * monitor_bluetooth             | libusb HID:
             |     * monitor_usb                   |   keyboard backlight
             |     * monitor_orientation           |
             v                                     |
   +---------------------+                         |
   | display_backend     |                         |
   |  - xrandr           |  (X11)                  |
   |  - gdctl            |  (Wayland / GNOME)      |
   |  - mutter_dbus      |  (Wayland / GNOME)      |
   +---------------------+                         |
             |                                     |
             v                                     |
       compositor                                  |
                                                   v
                                              hardware
```

The display backend is selected at runtime based on
`XDG_SESSION_TYPE` and `XDG_CURRENT_DESKTOP`, with a config override
available for unusual setups.

---

## Build

### Dependencies (Ubuntu 24.04 / 26.04)

```bash
sudo apt install build-essential pkg-config \
    libgtk-3-dev libayatana-appindicator3-dev libusb-1.0-0-dev \
    libudev-dev libglib2.0-dev \
    feh bluez pulseaudio-utils x11-xserver-utils
```

For the GNOME / Wayland display backend, `gdctl` is provided by
`gnome-control-center` ≥ 47 (default on Ubuntu 26.04).

### Compile

```bash
make
```

Produces `bin/zbd-system` and `bin/zbd-tray`. Use `make DEBUG=1` for
`-O0 -g3` development builds and `make RELEASE=1` for `-O2 -DNDEBUG`.

### Install

```bash
sudo make install
```

Installs:

- `/usr/local/sbin/zbd-system`
- `/usr/local/bin/zbd-tray`
- `/etc/zbd/zbd.conf` (only if missing)
- `/usr/share/icons/zbd/zbd-tray.svg`
- `/lib/systemd/system/zbd-system.service`
- `/usr/lib/systemd/user/zbd-tray.service`
- `/usr/share/dbus-1/system.d/org.anexa.zbd.conf`
- `/usr/share/polkit-1/actions/org.anexa.zbd.policy`

Then enable both units:

```bash
sudo systemctl enable --now zbd-system.service
systemctl --user enable --now zbd-tray.service
```

Uninstall with `sudo make uninstall`.

### Build a Debian package

```bash
make deb
```

Produces a `.deb` you can drop on any compatible Ubuntu/Debian.

---

## Configuration

The config lives at `/etc/zbd/zbd.conf`. All values are validated at
load time; an invalid value fails the daemon’s startup with a clear
journal entry rather than silently doing the wrong thing.

```ini
# Detection mode: udev | bluetooth | both
modo_deteccion=both

# Bluetooth keyboard MAC (uppercase hex, colon-separated)
bluetooth_mac_teclado=C2:CE:E8:06:01:F9

# Sysfs path of the attached keyboard (run `udevadm info --query=path
# --name=/dev/bus/usb/<bus>/<dev>` to find it for your unit).
udev_usb_path=/devices/pci0000:00/0000:00:14.0/usb3/3-6

# iio-sensor-proxy
orientacion_bus=net.hadess.SensorProxy
orientacion_path=/net/hadess/SensorProxy
orientacion_interfaz=net.hadess.SensorProxy

# Display
pantalla_resolucion=2880x1800
pantalla_tasa_refresco=120
pantalla_fondo_edp1=/usr/share/backgrounds/zbd/bg_edp1.jpg
pantalla_fondo_edp2=/usr/share/backgrounds/zbd/bg_edp2.jpg
pantalla_nivel_brillo=10
pantalla_backend=auto      # auto | xrandr | gdctl | mutter_dbus

# Keyboard backlight (0..3)
teclado_nivel_brillo=1

# Battery charge cap (20..100)
bateria_carga_maxima=80

# Whether the orientation monitor should rotate the displays
rotacion_automatica=true
```

---

## Display backends

`zbd` ships with three interchangeable backends. The active backend
is chosen automatically at startup unless `pantalla_backend` is set
explicitly.

| Backend       | Works on                | Notes |
|---------------|-------------------------|-------|
| `xrandr`      | X11                     | Legacy. The lightest. |
| `gdctl`       | Wayland + GNOME ≥ 47    | Wraps the canonical CLI. Default on Ubuntu 26.04. |
| `mutter_dbus` | Wayland + GNOME ≥ 45    | Talks directly to `org.gnome.Mutter.DisplayConfig`. No external process. |

Each backend implements:

```c
struct display_backend {
    const char *name;
    int (*activate_output)(const char *output, const char *mode, int hz);
    int (*deactivate_output)(const char *output);
    int (*is_output_active)(const char *output);
    int (*set_rotation)(const char *output, enum rotation r);
    int (*set_wallpapers)(const char *bg1, const char *bg2);
};
```

Adding a new backend (say, for Sway / wlroots) is one new `.c` file
plus a registration line in `display.c`.

---

## Privilege model

| Operation                               | Component   | Permissions       |
|-----------------------------------------|-------------|-------------------|
| Read `/etc/zbd/zbd.conf`                | both        | world-readable    |
| Write `/sys/class/backlight/.../brightness` | `zbd-system` | root              |
| Write `/sys/class/power_supply/BAT0/charge_control_end_threshold` | `zbd-system` | root |
| `libusb_detach_kernel_driver` + `SET_REPORT` (keyboard) | `zbd-system` | root or `CAP_SYS_ADMIN` |
| `xrandr` / `gdctl` / Mutter D-Bus       | `zbd-tray`  | regular user      |
| `feh` to set wallpapers                 | `zbd-tray`  | regular user      |
| `bluetoothctl` and udev monitoring      | `zbd-tray`  | regular user      |
| `pactl` to load DMIC source             | `zbd-tray`  | regular user      |

The privileged operations are exposed by `zbd-system` over D-Bus
under `org.anexa.zbd1` and gated by polkit policies (see
`polkit/org.anexa.zbd.policy`). The user is prompted once per
session at most; trusted users (members of the `zbd` group) can be
granted password-less access via a polkit rule installed at
`/etc/polkit-1/rules.d/50-zbd.rules`.

---

## Troubleshooting

### Nothing happens when I detach the keyboard

```bash
# Confirm the udev path the kernel sees:
udevadm info --query=path --name=/dev/bus/usb/003/<dev>
# Confirm the daemon is listening:
journalctl --user -u zbd-tray.service -f
```

### `eDP-2` does not turn on / off on Wayland

You are probably on a backend that does not match your session.
Force `pantalla_backend=gdctl` (GNOME ≥ 47) or
`pantalla_backend=mutter_dbus` and restart `zbd-tray`.

### `zbd-system` cannot toggle the keyboard backlight

The active session must let `libusb` detach the kernel HID driver.
On a hardened system, ensure `zbd-system` has `CAP_SYS_ADMIN` (set
in the `.service` file with `AmbientCapabilities=CAP_SYS_ADMIN`).

### Permission denied writing battery threshold

Some firmwares only enable the threshold in `/sys` after a delay.
`zbd-system` retries up to 5 times with a 200 ms back-off; if it
still fails, check that your kernel exposes
`/sys/class/power_supply/BAT0/charge_control_end_threshold` (most
mainline kernels ≥ 6.5 do).

---

## Development

See [CONTRIBUTING.md](CONTRIBUTING.md) for the workflow, code style,
commit-message convention and how to run lint and tests.

Quick loop:

```bash
make DEBUG=1                 # debug build
sudo make install            # or run from bin/ via systemd-run --user
make format && make lint     # clang-format + clang-tidy
make test                    # unit tests for parser & validation
```

To run a single binary against a development tree without installing:

```bash
sudo bin/zbd-system --config $(pwd)/conf/zbd.conf --foreground
bin/zbd-tray --config $(pwd)/conf/zbd.conf --backend gdctl
```

---

## License

`zbd` is free software: you can redistribute it and/or modify it
under the terms of the **GNU General Public License v3.0 or later**
as published by the Free Software Foundation. See [LICENSE](LICENSE)
for the full text.

`zbd` is distributed in the hope that it will be useful, but
**WITHOUT ANY WARRANTY**; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
license for details.
