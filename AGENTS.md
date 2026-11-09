# AGENTS.md

One Arduino sketch, no build system: `nano_r4_usb_host_keyboard.ino` — an Arduino Nano R4
(Renesas RA4M1) as a **USB host** for a HID keyboard, rendering the raw 8-byte report on a
SparkFun Qwiic "Narrow" OLED (128×32) over the Qwiic I²C connector.
`compile` is the only verification available; there are no tests, linters, or CI. Uploading
works when the board is plugged in, but the only real check is looking at the OLED.

## Build

```bash
arduino-cli compile --fqbn unor4tinyusb:renesas_uno:nanor4 .
arduino-cli upload  --fqbn unor4tinyusb:renesas_uno:nanor4 --port 1-1 .
```

`--port 1-1` is the DFU port `arduino-cli board list` reports for this board; there is no serial
port to name, see below. Run `arduino-cli board list` to get the current topology path.

Verified: **71,176 B flash (27 % of 262,144) / 10,676 B RAM (32 % of 32,768)**. RAM is the
binding constraint; the OLED's own graphics buffer is 512 B of it.

**After editing `tusb_config.h`, build with `--clean`.** arduino-cli does not track that file as a
dependency of the core's TinyUSB objects, so a plain rebuild can reuse objects compiled with the
*previous* config and still print a plausible memory report. Seen directly when raising
`CFG_TUH_HID 1 -> 4`: the cached build reported 10,240 B and one-slot arrays (`_hidh_epbuf` =
`0x80`); `--clean` reported 10,668 B and four-slot arrays (`0x200`). Confirm with
`arm-none-eabi-nm -S <sketch>.elf | grep _hidh`.

**This is host firmware, so the usual Nano R4 upload story does not hold.** `tuh_arduino_init()`
makes rhport 0 a USB host, so once the app runs the board stops enumerating on the PC entirely: no
`2341:0074`, no `/dev/ttyACM0`, nothing in `lsusb`. 5 V still arrives from the port, so the sketch
and the OLED run blind. That is why there is no serial port to pass to `--port`.

Target DFU instead. `arduino-cli board list` shows the board under its USB topology path with
protocol `dfu` (observed `1-1`). The board comes back as the **bootloader** `2341:0374`, not the
runtime `2341:0074` / `name="DFU-RT Port"` — both are fine, because the core's recipe is
`dfu-util --device 0x2341:0x0074,:0x0374 -a0`, which matches either. A successful upload ends with
`Download done.` / `Done!` and then `New upload port: 1-1 (dfu)`, **not** `... ttyACM0 (serial)`.

**Re-flashing requires the bootloader back.** With this host build running the board never
re-enumerates, so a plain `arduino-cli upload` finds no port at all — double-tap RESET to enter the
DFU bootloader (`2341:0374`), then upload. The device-firmware case (runtime `2341:0074`, load
address `0x00010000`, "no RESET dance") no longer applies once host firmware is on the board. Kill
any serial monitor first; a held port blocks enumeration.

Verified uploading the `CFG_TUH_HID 4` build: board arrived in bootloader mode `2341:0374`,
`--port 1-1` wrote 70,744 B, ended `Download done.` → `Done!`, then the board disappeared from
`lsusb` exactly as expected above. The double-tap recovery is the standard Nano R4 route and was
**not** re-run in that session.

**Verified on hardware** (2026-11-08, bench, Holtek `04d9:1203` composite numeric keypad, bus
powered through the splitter): both HID interfaces mount, `kbd ok` is reached, and the six keycode
cells track the report — `KP7`/`KP8`/`KP9`/`KP5` show as `5F`/`60`/`61`/`5D` and return to all
zeros on release. Row 1 stays blank on this device **by design**: a numpad sends no modifiers, so an
empty modifier row here is not a fault. An earlier run (2026-11-05) covered a single-interface
keyboard. Not tested without the splitter. Still **not** exercised on hardware: the `kbd decl`
fallback branch, the takeover-from-a-vendor-interface path, and the `loop()` safety net, which never
fired because the deferred `set_protocol` is what makes enumeration complete.

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

- `tusb_config.h` is trimmed to four host HID slots (`CFG_TUH_HID 4`, no HUB, no host CDC/MSC).
  Four, not one, because `hidh_open()` claims **any** HID-class interface and a composite keyboard
  exposes several: with a single slot a vendor interface that enumerates first drives
  `hidh_set_config()` into `TU_ASSERT(p_hid, 0)` and the real keyboard interface is dropped
  without `tuh_hid_mount_cb` ever running for it. Do not lower it back.
  `CFG_TUD_ENABLED 1` and `CFG_TUD_CDC 1` are **floors, not choices**: `USB.cpp` builds a
  `TUD_CDC_DESCRIPTOR` outside any `#if` and `__USBStart()` calls `tud_rhport_init()`, and
  `SerialUSB.cpp`'s `_SerialUSB` object calls `tud_cdc_*`. Zeroing them fails to compile the **core**.
  The file must stay self-contained — the core's TinyUSB TUs get five include dirs, none of them
  the sketch folder or `src/`, so a config that `#include`s a helper header fails with
  "No such file or directory" inside `cdc_device.c`.
- **The keyboard is chosen by interface protocol, never by index.** Since TinyUSB claims any
  HID-class interface, `idx 0` is only "whatever enumerated first". `tuh_hid_mount_cb` reads
  `tuh_hid_interface_protocol(daddr, idx)` and claims the `HID_ITF_PROTOCOL_KEYBOARD` one; a
  non-boot interface is kept only as a fallback so a boot-interface-less keyboard still displays
  (`HID_ITF_PROTOCOL_MOUSE` never is), and a real boot keyboard appearing later takes the slot
  over. Reports from any other index are dropped and left **un**-armed, so they go quiet after the
  one report that identified them. Row 3 tells the states apart: `kbd ok` (confirmed boot
  protocol), `kbd decl` (mounted, rendered as declared), `no kbd`.
- **Never issue a control transfer from `tuh_hid_mount_cb`.** It runs from
  `config_driver_mount_complete()`, i.e. from inside a control-transfer completion, while TinyUSB is
  still walking the remaining interfaces in its driver-configuration pass. Calling
  `tuh_hid_set_protocol()` there derailed that pass on a Holtek `04d9:1203` keypad: the follow-on
  interface never configured, `tuh_mount_cb` never fired, and no report ever arrived. Symptom:
  `kbd ok` on the OLED, dead hex digits, and **no error on the console whatsoever**. Do it from
  `tuh_mount_cb` instead — `usbh.c` reaches it only after every interface has configured — which is
  what `startKeyboard()` exists for. Two tempting explanations are both **wrong**, so do not
  re-derive either. The device *was* configured: `SET_CONFIGURATION` is an earlier enumeration step
  than driver configuration (the next step after it is `ENUM_CONFIG_DRIVER`). And reception *was*
  requested: the protocol-complete callback ran — `boot protocol set` is printed from it — and
  called `tuh_hid_receive_report()`. Whether that call succeeded is genuinely unknown, because the
  code then discarded its return value (exactly the failure mode `armReceive()` now makes visible).
  Why a stalled driver pass should silence an endpoint on a configured device is **not understood**;
  the fix rests on the A/B, not on a mechanism.
- **Never ignore `tuh_hid_receive_report()`'s return value.** A false return means the IN endpoint
  was not claimed, and nothing re-arms it afterwards: the keyboard is permanently deaf with no other
  symptom. `armReceive()` logs it; that log is the only way this failure mode is visible.
- `tuh_hid_set_protocol(daddr, idx, HID_PROTOCOL_BOOT)` forces the **HID boot protocol**, so
  reports are the canonical 8 bytes whatever the interface declared. `set_protocol_complete()` in
  `hid_host.c` invokes `tuh_hid_set_protocol_complete_cb` **whether or not the transfer
  succeeded**, and only updates its own mode on success — so the `protocol` argument is the only
  honest signal, and "boot protocol set" must never be logged unconditionally. Exactly one
  `tuh_hid_receive_report()` arming: `mount_cb` arms only when `set_protocol` could not even queue
  the control transfer (then no callback will follow), otherwise the complete callback arms it.
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
