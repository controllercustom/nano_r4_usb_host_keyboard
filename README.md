# Nano R4 USB host keyboard

An Arduino Nano R4 acting as a **USB host** for a normal USB keyboard. The raw
eight-byte USB HID report is printed on a SparkFun Qwiic OLED — no parsing into
characters, the report itself.

```
row 1   LC LS LA LG RC RS RA RG     a label appears while that modifier bit is set
row 2   04 1E 00 00 00 00           the six keycodes, two hex digits each, in report order
row 3   kbd ok                      mount status ("no kbd" until the keyboard mounts)
```

Report byte 0 is the modifier bitmap (bit 0 L-Ctrl, bit 1 L-Shift, bit 2 L-Alt,
bit 3 L-GUI, bit 4 R-Ctrl, bit 5 R-Shift, bit 6 R-Alt, bit 7 R-GUI). Byte 1 is
the reserved field. Bytes 2–7 are the six keycodes.

Typing `A` gives `LS` lit in row 1 and `04 00 00 00 00 00` in row 2 — the
keyboard sends the left-shift and the `a` scancode as one report, and the
modifier stays lit until you release the key.

## Hardware

| Item | Notes |
|---|---|
| Arduino Nano R4 | Renesas RA4M1, USB-C connector is the host port |
| SparkFun Qwiic OLED Display, 0.91 in, 128×32 | SSD1306, I²C only — attaches to the Qwiic connector |
| USB OTG cable, Type-C → Type-A host | the keyboard plugs into the Type-A end |
| USB power splitter cable | supplies VBUS to the keyboard's Type-A branch |

The Nano R4 **does not source VBUS from its USB connector**. Self-powered
keyboards work straight off the OTG cable; a bus-powered keyboard needs the
power splitter (or a powered hub) or it never sees a powered bus.

Verified on the bench with a real keyboard and the power splitter: the modifier
row and the six keycode cells both track the report.

![Photo of a USB keyboard plugged into a Nano R4 in USB host mode.](./images/nano_r4_usb_host_keyboard.jpg)

"USB power splitter cable" is the search term to find the cable with three USB
connectors but when used here it is more accurately described as an "USB power
injection cable".

The cables are unplugged to make it easier to see the connectors on each cable.
The small white adapter can be found by searching for "usb otg type c male to
type a female cable or adapter". I cannot guarantee every adapter and cable
works.

## Libraries

Both live in `~/Arduino/libraries` and are already installed on this machine.

- **UnoR4-tinyusb** (<https://github.com/controllercustom/UnoR4-tinyusb>) — TinyUSB
  device + host for the RA4M1 RUSB2 port. Installed as the **`unor4tinyusb` board
  package** (`Nano R4 (TinyUSB)`), which is what makes the `tusb_config.h` next to
  this sketch actually take effect.
- **SparkFun Qwiic OLED** — use the fork,
  <https://github.com/controllercustom/SparkFun_Qwiic_OLED_Arduino_Library>, branch
  **`remove_printf`**. Upstream 1.0.14/1.0.15 call `Serial.printf`, which does not
  exist on the Renesas core (`fatal error: 'class _SerialUSB' has no member named
  'printf'`). The fork also drops leftover debug output. It keeps the folder name
  and reports `version=1.0.15`, so the include is unchanged.

## Build

```bash
arduino-cli compile --fqbn unor4tinyusb:renesas_uno:nanor4 .
arduino-cli upload  --fqbn unor4tinyusb:renesas_uno:nanor4 -p /dev/ttyACM0 .
```

Upload is DFU over the runtime port (`2341:0074`); no RESET-button dance needed.

Verified sizes: **70,116 B flash (26 % of 262,144) / 10,240 B RAM (31 % of 32,768)**.

## Console (optional)

The board is the host, so the USB CDC `Serial` object is not a console — TinyUSB
owns that port. Debug lines (`mount`, VID:PID, every report) go to **`Serial1`**,
the hardware UART: pin **D0 = RX**, **D1 = TX**, 115200 8N1, 3.3 V. Wire any USB-UART
adapter into those two pins to watch; with nothing attached the sketch still runs
and the OLED still shows everything.

## Troubleshooting

- **OLED blank, sketch running** — the Qwiic connector is `Wire1` (pins 26/27),
  not `Wire` (the A4/A5 pair). `begin()` defaults to `Wire`, so the sketch must
  call `Wire1.begin()` **and** pass it: `myOLED.begin(Wire1)`.
- **Keyboard never mounts** — bus-powered keyboard without the power splitter.
  Check `lsusb` and the console `mount` line.
- **Only the first report shows** — the report callback must re-arm with
  `tuh_hid_receive_report()`; without it the keyboard reports exactly once.
- **Row 1 shows nothing while keys are held** — the keyboard may not implement
  the boot interface; the sketch falls back to whatever it declared and says so
  on the console.

## Acknowledgments

This sketch was developed with assistance from [OpenCode](https://opencode.ai).

## License

[MIT](./LICENSE) — see the `SPDX-License-Identifier: MIT` header at the top of
each source file.
