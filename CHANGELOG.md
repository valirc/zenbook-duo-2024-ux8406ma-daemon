# Changelog

All notable changes to **zbd** are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- `.gitignore` to keep build artifacts (`obj/`, `bin/`) and editor files out
  of the repository.
- GPL-3.0 license text (`LICENSE`).
- `.editorconfig` to enforce consistent style (4-space indent for C and
  config files, LF line endings, trailing-whitespace stripped).
- `CHANGELOG.md`, `CONTRIBUTING.md` and a comprehensive `README.md`
  documenting target hardware, architecture, build, install and usage.

### Changed
- Stopped tracking the precompiled binaries (`bin/zbd`, `bin/zbd-tray`) and
  intermediate object files (`obj/*.o`); they are now expected to be
  produced by `make` from source.

### Notes
This entry tracks the in-progress refactor toward `zbd 2.0`: a clean
two-binary split (`zbd-system` + `zbd-tray`) with a privilege boundary,
swappable display backends (xrandr / gdctl / Mutter D-Bus) for X11 *and*
Wayland support, hardened command execution (no shell evaluation of
external data), strict configuration validation, real screen rotation
driven by the accelerometer and proper signal-handled shutdown. Changes
will land in successive commits on the `refactor/c-modernization` branch.
