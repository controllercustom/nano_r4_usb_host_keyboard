# AGENTS.md

One Arduino sketch, no build system: `nano_r4_usb_host_keyboard.ino` — an Arduino Nano R4
(Renesas RA4M1) as a **USB host** for a HID keyboard, rendering the raw 8-byte report on a
SparkFun Qwiic "Narrow" OLED (128×32) over the Qwiic I²C connector.
`compile` is the only verification available; there are no tests, linters, or CI. Uploading
works when the board is plugged in, but the only real check is looking at the OLED.

## Build

```bash
arduino-cli compile --fqbn unor4tinyusb:renesas_uno:nanor4 .
arduino-cli upload  --fqbn unor4tinyusb:renesas_uno:nanor4 -p /dev/ttyACM0 .
```

Verified: **70,116 B flash (26 % of 262,144) / 10,240 B RAM (31 % of 32,768)**. RAM is the
binding constraint; the OLED's own graphics buffer is 512 B of it.

Upload is DFU over the **runtime** port: `lsusb` shows `2341:0074`, `dfu-util --list` reports
`name="DFU-RT Port"`, load address `0x00010000`. No RESET dance. A successful upload ends with
`Download done.` then `New upload port: /dev/ttyACM0 (serial)`. Kill any serial monitor first —
a held port blocks DFU enumeration.

**Verified on hardware** (2026-11-05, bench): with the USB power splitter cable attached, a real
keyboard's reports render correctly — the modifier row lights per bit and the six keycode cells
track the report. Not tested without the splitter.

**Do NOT add `sketch.yaml`.** With one present, arduino-cli 1.5.1 stops adding `-I` for user
libraries: the build fails at `#include <UnoR4TinyUSB.h>` with `Alternatives for UnoR4TinyUSB.h: []`
even though the library is installed. Passing `--fqbn` on the command line (as above) is the
working shape, and it is what the sibling project `~/nano_r4_sparkfun_narrow_oled_qwiic` does too.

## The two libraries this builds against

Both are already in `~/Arduino/libraries` — do not reinstall.

- `UnoR4-tinyusb` (0.5.0), a symlink-free copy of <https://github.com/controllercustom/UnoR4-tinyusb>.
  Build against the **`unor4tinyusb:renesas_uno:nanor4`** board package, not the patched stock
  `arduino:renesas_uno:nanor4`. The package's `platform.txt` adds `-iquote{build.source.path}`, and
  `-iquote` directories are searched before `-I`, so **`tusb_config.h` next to the `.ino` is live**
  for every translation unit including the core's own TinyUSB sources. On the stock core that file
  is invisible and you would have to run `~/UnoR4-tinyusb/scripts/config.sh set <Example>`, which
  mutates global core state — that is why the package is the default here.
- `SparkFun_Qwiic_OLED_Arduino_Library` (1.0.15) is the **fork**, branch `remove_printf`, of
  <https://github.com/controllercustom/SparkFun_Qwiic_OLED_Arduino_Library>. Verified fork-installed:
  no `printf` anywhere in `src/`. Upstream 1.0.14/1.0.15 fail with
  `fatal error: 'class _SerialUSB' has no member named 'printf'` — the Renesas core has no
  `PrintStream.h` at all. `arduino-cli lib install` cannot fetch a branch; install by cloning the
  branch into the libraries folder and dropping `.git`. Keep backups **outside** `~/Arduino/libraries`:
  a second folder containing `SparkFun_Qwiic_OLED.h` makes include resolution ambiguous.

Do **not** install a separate Wire library: `Wire`/`Wire1` come from the `Wire` library bundled
inside the core (`.../renesas_uno/1.6.0/libraries/Wire`), which is what `SparkFun_Qwiic_OLED.h` includes.

## Sketch notes

- `tusb_config.h` is trimmed to one host HID slot (`CFG_TUH_HID 1`, no HUB, no host CDC/MSC).
  `CFG_TUD_ENABLED 1` and `CFG_TUD_CDC 1` are **floors, not choices**: `USB.cpp` builds a
  `TUD_CDC_DESCRIPTOR` outside any `#if` and `__USBStart()` calls `tud_rhport_init()`, and
  `SerialUSB.cpp`'s `_SerialUSB` object calls `tud_cdc_*`. Zeroing them fails to compile the **core**.
  The file must stay self-contained — the core's TinyUSB TUs get five include dirs, none of them
  the sketch folder or `src/`, so a config that `#include`s a helper header fails with
  "No such file or directory" inside `cdc_device.c`.
- `tuh_hid_set_protocol(daddr, idx, 0)` forces the **HID boot protocol**, so reports are the
  canonical 8 bytes whatever the keyboard declared. On failure the sketch falls back to receiving
  as declared and says so. Exactly one `tuh_hid_receive_report()` arming per interface: `mount_cb`
  arms only when `set_protocol` failed, otherwise `tuh_hid_set_protocol_complete_cb` arms it.
- `tuh_hid_report_received_cb` must end with `tuh_hid_receive_report()` — without it the keyboard
  reports exactly once.
- **OLED redraw idiom:** there is no `clear()`. `erase()` blanks the dirty regions of the local
  buffer, then `display()` ships the erase rects and the new ink in one I²C round trip. `erase()`
  before drawing, `display()` after. Frames are only pushed when the report actually changed.
- Layout is derived, never hard-coded: `getWidth()`/`getHeight()`/`getStringHeight("X")`. The default
  font is `QW_FONT_5X7` (5×7, advance 6 px), so a 2-char cell is 11 px of ink; row 1 is eight 14 px
  cells (112 px), row 2 six 16 px cells (96 px), row 3 is the mount status at `y = height - fontH`.
- The Nano R4's **Qwiic I²C port is `Wire1`** (pins 26/27); `Wire` is the A4/A5 pair. `begin()`
  defaults to `Wire`, so the sketch must call `Wire1.begin()` **and** pass it: `myOLED.begin(Wire1)`.
- The console is `Serial1` (hardware UART, D0 = RX, D1 = TX, 3.3 V). `Serial` is the USB CDC port
  TinyUSB owns in host mode — calling `Serial.begin()` deadlocks `setup()` in `Print::write`.
- The R4 does **not** source VBUS from its USB connector: bus-powered keyboards need the power
  splitter cable, self-powered ones work direct.
- Never override `__USBStart()`, and never define `tud_cdc_line_state_cb` /
  `tud_cdc_line_coding_cb` / `tud_dfu_runtime_reboot_to_dfu_cb` — the core owns those strongly.

## Bench test without a keyboard

`~/UnoR4-tinyusb/emulators/KeyA_Loop` (RP2040, PID `239A:CAFE`) sends `HID_KEY_A` at 250/500 ms:
row 2 should alternate `04 00 00 00 00 00` / all zeros, row 1 stay blank (it sends no modifier).
Flash with `~/UnoR4-tinyusb/emulators/upload.sh key`. After changing a PID, reset the host (`J-Link r; g`).

## Style

Arduino community consensus, matching `~/UnoR4-tinyusb`: Allman braces, `else` on its own line,
2-space indent, camelCase functions/variables, UPPER_SNAKE_CASE defines. `extern "C"` host callbacks
live in the `.ino` (that is what the `r4_*` examples do); the warning about the prototype generator
mangling `extern "C"` applies to the **descriptor** callbacks in device sketches, which belong in a
separate `.cpp`.
