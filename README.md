# KMSCON

![Build Status](https://github.com/kmscon/kmscon/actions/workflows/meson.yml/badge.svg?branch=main)

Kmscon is a simple terminal emulator based on linux kernel mode setting (KMS).
It is an attempt to replace the in-kernel VT implementation with a userspace
console. See kmscon(1) man-page for usage information.

## Requirements

Kmscon requires the following software:
  - [libtsm](https://github.com/kmscon/libtsm): terminal emulator state machine
  - [libudev](https://www.freedesktop.org/software/systemd/man/libudev.html): providing input, video, etc. device hotplug support (>=v172)
  - [libxkbcommon](https://xkbcommon.org/): providing internationalized keyboard handling
  - [libdrm](https://gitlab.freedesktop.org/mesa/drm): graphics access to DRM/KMS subsystem
  - linux-headers: linux kernel headers for ABI definitions

Everything else is optional:

For video output at least one of the following is required:
- fbdev: For framebuffer video output the kernel headers must be installed and located in the default include path.
- DRM: For unaccelerated drm output the "libdrm" library must be installed and accessible via pkg-config.
- OpenGLES2: For accelerated video output via OpenGLESv2 the following must be installed: libdrm, libgbm, egl, glesv2 (i.e., mesa)

For font handling the following is required:
- 8x16: The 8x16 font is a static built-in font which does not require external dependencies.
- unifont: Static font without external dependencies.
- pango: drawing text with pango Pango requires: glib, pango, fontconfig, freetype2 and more

For multi-seat support you need the following packages:
- systemd: Actually only the systemd-logind daemon and library is required.

On Debian-based system, to install the systemd service files in the right location, you need to install systemd-dev.
```bash
sudo apt install systemd-dev
```

## Download

Released tarballs can be found at: https://github.com/kmscon/kmscon/releases

## Install

To compile the kmscon binary, run the standard meson commands:
```bash
meson setup builddir/
````

By default this will install into `/usr/local`, you can change your prefix with `--prefix=/usr`
(or `meson configure builddir/ -Dprefix=/usr` after the initial meson setup).

Then build and install. Note that this requires ninja.
```bash
meson install -C builddir/
```

The following meson options are available.
They can be used to select backends for several subsystems in kmscon.
If build-time dependencies cannot be satisfied, an option is automatically turned off, except if you
explicitly enable it via command line:

| option | default | description |
|:------|:-------:|:-----------|
|`extra_debug`| `false` | Additional debug outputs |
|`multi_seat`| `auto` | This requires the systemd-logind library to provide multi-seat support for kmscon |
|`video_fbdev`| `auto` | Linux fbdev video backend |
|`video_drm2d`| `auto` | Linux DRM software-rendering backend |
|`video_drm3d`| `auto` | Linux DRM hardware-rendering backend |
|`font_unifont`| `auto` | Static built-in non-scalable font (Unicode Unifont) |
|`font_pango`| `auto` | Pango based scalable font renderer |
|`renderer_bbulk`| `auto` | Simple 2D software-renderer (bulk-mode) |
|`renderer_gltex`| `auto` | OpenGLESv2 accelerated renderer |
|`session_dummy`| `auto` | Dummy fallback session |
|`session_terminal`| `auto` | Terminal-emulator sessions |

## Running

To get usage information, run:
```bash
kmscon --help
```
You can then run kmscon with:
```bash
kmscon [options]
```

### Locale

Kmscon queries and setups system locale settings before starting if systemd-localed is available.
Otherwise, you can change locale settings via `--xkb-{model,layout,variant,options}` command line options.
See `man kmscon` for more information.

### Config file

The default configuration file is `/etc/kmscon/kmscon.conf`. Any command line option can be put in the config file in
its long form without the leading `--` (double dash). See `man kmscon` for more information or look at [kmscon.com](scripts/etc/kmscon.conf)

## License

This software is licensed under the terms of an MIT-like license. Please see
[`COPYING`](./COPYING) for further information.

## History

This project was maintained in [Aetf's](https://github.com/Aetf/kmscon) fork for 11 years, before coming back here in 2025
