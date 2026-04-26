# Contributing to zbd

Thanks for your interest in **zbd** (Zenbook Duo Daemon). This document
captures the conventions and workflow used in this repository so new
contributions land cleanly.

## Scope

`zbd` automates the Linux user experience for the **ASUS ZenBook Duo
2024 (UX8406MA)** — a laptop with two stacked OLED panels and a
detachable keyboard. The project focuses on functionality that the
upstream kernel and userspace stack do not handle automatically on
this exact hardware:

- Detecting the keyboard transition between attached (USB) and detached
  (Bluetooth), and reconfiguring the second internal display
  (`eDP-2`) accordingly.
- Driving the keyboard backlight (custom HID protocol).
- Capping the battery charge at a configurable threshold.
- Wiring the Intel Smart Sound DMIC into the default PulseAudio /
  PipeWire source.
- Reacting to accelerometer orientation changes.

Patches that broaden support to other ASUS dual-screen models are
welcome but should be opt-in (config flag or runtime detection) so
the default behaviour for the UX8406MA remains stable.

## Development environment

- **OS**: Ubuntu 24.04 LTS (`noble`) or 26.04 LTS (`resolute`).
- **Compiler**: GCC ≥ 13 or Clang ≥ 18, C17.
- **Build deps**: `gtk+-3.0`, `libayatana-appindicator3-0.1`,
  `libusb-1.0`, `glib-2.0`, `gio-2.0`, `libudev`, `pkg-config`, `make`.
- **Runtime deps**: `feh`, `bluez` (`bluetoothctl`),
  `pulseaudio-utils` (`pactl`), and one of: `x11-xserver-utils`
  (`xrandr`) **or** `gnome-control-center` (`gdctl`) depending on the
  active display backend.

Install everything in one go on Debian/Ubuntu:

```bash
sudo apt install build-essential pkg-config \
    libgtk-3-dev libayatana-appindicator3-dev libusb-1.0-0-dev \
    libudev-dev libglib2.0-dev \
    feh bluez pulseaudio-utils x11-xserver-utils
```

## Code style

- **Indentation**: 4 spaces, no tabs (except `Makefile`).
- **Line endings**: LF.
- **Final newline** required.
- **No trailing whitespace** (`.editorconfig` enforces this).
- **Function naming**: `snake_case`; module prefix where ambiguity
  exists (`display_*`, `monitor_*`, `hw_*`).
- **Header guards**: `#ifndef ZBD_<MODULE>_H` / `#define ...` /
  `#endif`.
- **Public API**: declared in `src/include/<module>.h` with a one-line
  comment per function describing pre/postconditions, ownership and
  thread-safety.
- **Internal helpers**: `static` inside the `.c` file.
- **No `system()` or `popen()`** with externally-influenced strings —
  use the `exec_cmd_argv()` helper from `common/exec.h`, which forks
  and `execve`'s an explicit argv array without spawning a shell.

Run `clang-format` (style file `.clang-format`) and `clang-tidy` before
submitting:

```bash
make format
make lint
```

## Commit messages

We use [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>): <imperative summary, ≤ 72 chars>

<body explaining the *why* of the change. Wrap at 72 cols.>

<optional footer: BREAKING CHANGE / Refs #N / Co-Authored-By: ...>
```

Allowed `<type>`: `feat`, `fix`, `refactor`, `perf`, `docs`, `style`,
`test`, `build`, `ci`, `chore`, `revert`.

Example:

```
fix(monitor_usb): correct inverted on-attach / on-detach logic

The udev event-loop branch had the actions of the initial-state branch
swapped: on `add` we were turning eDP-2 *off* and on `remove` we were
turning it *on*. Restore the same semantics as the initialisation
block: attached keyboard => disable eDP-2, detached => enable.

Refs #<issue-or-pr>
```

## Branching & PRs

- `main` is always shippable.
- Work on topic branches: `feat/...`, `fix/...`, `refactor/...`.
- Open a PR against `main` once the topic is complete and CI is green.
- Squash-merge for small topics; rebase-merge for larger refactors so
  the per-step history is preserved.

## Tests

Where feasible, accompany changes with unit tests under `tests/`. The
configuration parser, validation helpers and the `exec_cmd_argv`
escape behaviour are the high-value targets — anything that touches
real hardware is covered manually on a UX8406MA development unit.

## Reporting issues

Please include:

1. The exact laptop model and firmware version
   (`sudo dmidecode -s system-product-name` and
   `sudo dmidecode -s bios-version`).
2. Output of `lsb_release -a`, `uname -a`, `lsusb`.
3. The `[Unit]` line of the active session (`echo $XDG_SESSION_TYPE`,
   `echo $XDG_CURRENT_DESKTOP`).
4. The contents of `/etc/zbd/zbd.conf` (with sensitive values redacted).
5. The relevant journal output: `journalctl --user -u zbd-tray.service`
   and/or `journalctl -u zbd-system.service`.

## License

By contributing, you agree that your contributions will be licensed
under the **GNU General Public License v3.0 or later** (GPL-3.0-or-later),
the same license as the rest of the project (see `LICENSE`).
