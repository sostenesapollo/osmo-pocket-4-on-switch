# Credits

## GimbalBar

`mac/ptzctl.c` and `mac/ptz.h` are derived from
[GimbalBar](https://github.com/amirmxt/gimbalbar) by amirmxt, MIT License.

That project worked out the hard part: the Pocket exposes UVC Pan/Tilt Absolute on
camera terminal 1, VideoControl interface 0, selector `0x0D`, and IOKit is the only
way to reach it on macOS (libusb cannot, because the system UVC driver owns the
interface). This repo reuses that code and wraps it in a stdin-driven helper so the
Python server does not fork a process per command.

## stb

- [`stb_image.h`](https://github.com/nothings/stb) — JPEG decoding on the Switch.
  Public domain / MIT.
- [`stb_easy_font.h`](https://github.com/nothings/stb) — bitmap font used by the
  settings overlay. Public domain / MIT.

Both by Sean Barrett and contributors.

## Switch homebrew

- [devkitPro](https://devkitpro.org) — devkitA64 toolchain.
- [libnx](https://github.com/switchbrew/libnx) — Switch userland library (sockets,
  framebuffer, threads, input).

## libmtp

The installer in `mac/mtp_install.c` uses
[libmtp](https://libmtp.sourceforge.net) to talk to DBI's MTP responder.
