// SPDX-License-Identifier: MIT
//
// nano_r4_usb_host_keyboard.ino — Arduino Nano R4 as a USB HOST for a HID
// keyboard. The raw 8-byte USB HID report is rendered on a SparkFun Qwiic
// "Narrow" OLED (0.91 in., 128x32) over the Qwiic I2C connector.
//
// Screen
//   row 1: LC LS LA LG RC RS RA RG   a label appears while that modifier bit is set
//   row 2: six USB keycodes, two hex digits each, in report order
//   row 3: "kbd ok"   a boot keyboard, confirmed in HID boot protocol
//          "kbd decl" mounted but not a boot keyboard — shown as it declared
//          "no kbd"   nothing mounted
//
// Hardware
//   Arduino Nano R4 (Renesas RA4M1)                     https://www.arduino.cc/
//   SparkFun Qwiic OLED Display, 0.91 in, 128x32        https://www.sparkfun.com/products/17153
//   USB OTG cable, USB-C (board) -> USB-A host port     <photo TODO>
//   USB power splitter cable (the R4 does not source VBUS) <photo TODO>
//
// Libraries
//   UnoR4-tinyusb — host mode on the RA4M1 RUSB2 port
//   SparkFun Qwiic OLED fork, branch remove_printf (upstream calls Serial.printf)

#include <UnoR4TinyUSB.h>
#include <SparkFun_Qwiic_OLED.h>

QwiicNarrowOLED myOLED;

// Report byte 0, bit order per the HID spec: bit0 L-Ctrl .. bit7 R-GUI
static const char *kModLabels[8] = {"LC", "LS", "LA", "LG", "RC", "RS", "RA", "RG"};

// No HID interface owns the display yet.
#define KBD_IDX_NONE  0xFF

// Which HID interface is rendered on the OLED. TinyUSB claims any HID-class
// interface, so on a composite keyboard index 0 is just whatever enumerated
// first — the keyboard is chosen by interface protocol, never by index.
static uint8_t g_kbd_idx = KBD_IDX_NONE;

// True only once that interface is confirmed to be in HID boot protocol. False
// means the bytes on screen are whatever that interface declared, which is the
// case the OLED reports as "kbd decl".
static bool g_kbd_boot = false;

static uint8_t g_kbd_addr = 0;       // device address owning the display
static bool g_kbd_boot_itf = false;  // that interface is a HID boot keyboard
static bool g_kbd_started = false;   // protocol switch / first arm issued
static uint32_t g_itf_mount_ms = 0;  // when it mounted, for the loop() timeout

static uint8_t g_report[8] = {0};

// Row geometry, in pixels. The 5x7 default font advances 6 px per glyph, so
// a two-character cell is 11 px of ink; the cell pitch adds the gap.
static const uint8_t kModCellPitch = 14;
static const uint8_t kKeyCellPitch = 16;

static void drawFrame(void)
{
  uint8_t w = myOLED.getWidth();
  uint8_t h = myOLED.getHeight();
  uint8_t fontH = myOLED.getStringHeight("X");

  int16_t xMod = ((int16_t)w - 8 * kModCellPitch) / 2;
  int16_t xKey = ((int16_t)w - 6 * kKeyCellPitch) / 2;
  uint8_t yMod = 0;
  uint8_t yKey = h / 2;
  uint8_t yStatus = h - fontH;

  // erase() blanks the dirty regions of the local buffer; display() then ships
  // the erase rects and the new ink in one I2C round trip. Order matters.
  myOLED.erase();

  uint8_t mod = g_report[0];
  for (uint8_t i = 0; i < 8; i++)
  {
    if (mod & (1 << i))
    {
      myOLED.text(xMod + i * kModCellPitch + 1, yMod, kModLabels[i], COLOR_WHITE);
    }
  }

  for (uint8_t i = 0; i < 6; i++)
  {
    char hex[3];
    snprintf(hex, sizeof(hex), "%02X", g_report[2 + i]);
    myOLED.text(xKey + i * kKeyCellPitch + 2, yKey, hex, COLOR_WHITE);
  }

  const char *status = "no kbd";
  if (g_kbd_idx != KBD_IDX_NONE)
  {
    status = g_kbd_boot ? "kbd ok" : "kbd decl";
  }
  myOLED.text(xKey, yStatus, status, COLOR_WHITE);

  myOLED.display();
}

static void pushReport(const uint8_t *report, uint16_t len)
{
  if (len < 8)
  {
    char line[48];
    snprintf(line, sizeof(line), "[kbd] short report len=%u", len);
    Serial1.println(line);
    return;
  }

  if (memcmp(g_report, report, 8) == 0)
  {
    return;
  }

  memcpy(g_report, report, 8);

  char line[64];
  snprintf(line, sizeof(line), "[kbd] mod=%02X keys=%02X %02X %02X %02X %02X %02X",
           g_report[0], g_report[2], g_report[3], g_report[4], g_report[5], g_report[6], g_report[7]);
  Serial1.println(line);

  drawFrame();
}

// Arm the interrupt IN endpoint for one more report. A false return means the
// endpoint could not be claimed, and nothing else will ever arm it again: the
// keyboard goes permanently deaf with no other symptom. Never ignore it.
static void armReceive(uint8_t dev_addr, uint8_t idx)
{
  if (!tuh_hid_receive_report(dev_addr, idx))
  {
    char line[64];
    snprintf(line, sizeof(line), "[kbd] receive_report could not arm idx=%u", idx);
    Serial1.println(line);
  }
}

// Forget the display interface entirely.
static void clearKeyboard(void)
{
  g_kbd_idx = KBD_IDX_NONE;
  g_kbd_addr = 0;
  g_kbd_boot_itf = false;
  g_kbd_started = false;
  g_kbd_boot = false;
  g_itf_mount_ms = 0;
  memset(g_report, 0, sizeof(g_report));
}

// Issue the boot-protocol switch and the first receive.
//
// This must NOT be called from tuh_hid_mount_cb. That fires from
// config_driver_mount_complete(), i.e. from inside a control-transfer completion,
// while TinyUSB still owes every remaining interface its SET_IDLE and
// GET_REPORT_DESC. Issuing SET_PROTOCOL there derailed that pass on a Holtek
// 04d9:1203 keypad: the follow-on interface never configured, so tuh_mount_cb
// never fired and no report ever arrived. The device was nonetheless configured
// (SET_CONFIGURATION is an earlier enumeration step than driver configuration),
// and a receive had been requested — "boot protocol set" prints from the callback
// that requested it, though that code discarded the return value. Why a stalled
// driver pass should leave a configured device perfectly silent is not
// understood; this deferral rests on the A/B that fixed it, not on a mechanism.
// Do not re-derive an explanation from the symptom alone.
//
// tuh_mount_cb is the safe point: usbh.c reaches it only after every interface has
// configured (itf_num == CFG_TUH_INTERFACE_MAX).
static void startKeyboard(void)
{
  if (g_kbd_idx == KBD_IDX_NONE || g_kbd_started)
  {
    return;
  }
  g_kbd_started = true;

  if (!g_kbd_boot_itf)
  {
    Serial1.println("[kbd] starting without boot protocol, receiving as declared");
    armReceive(g_kbd_addr, g_kbd_idx);
    return;
  }

  // A false return means the control transfer was never queued, so no completion
  // callback will follow and reception has to be armed here instead.
  if (!tuh_hid_set_protocol(g_kbd_addr, g_kbd_idx, HID_PROTOCOL_BOOT))
  {
    Serial1.println("[kbd] set_protocol not queued, receiving as declared");
    armReceive(g_kbd_addr, g_kbd_idx);
  }
}

extern "C"
{

void tuh_mount_cb(uint8_t dev_addr)
{
  uint16_t vid = 0;
  uint16_t pid = 0;
  tuh_vid_pid_get(dev_addr, &vid, &pid);

  char line[96];
  snprintf(line, sizeof(line), "[kbd] mount dev=%u vid=%04X pid=%04X ready=%u",
           dev_addr, vid, pid, (unsigned int)tuh_ready(dev_addr));
  Serial1.println(line);

  startKeyboard();
}

void tuh_umount_cb(uint8_t dev_addr)
{
  (void) dev_addr;
  // Runs before the class drivers are closed, so this is the reset that matters:
  // by the time the per-interface callbacks arrive, g_kbd_idx is already
  // KBD_IDX_NONE and they stay quiet instead of pushing a second identical frame.
  clearKeyboard();
  Serial1.println("[kbd] device unmounted");
  drawFrame();
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report_desc, uint16_t desc_len)
{
  (void) report_desc;

  uint8_t itfProto = tuh_hid_interface_protocol(dev_addr, idx);

  char line[96];
  snprintf(line, sizeof(line), "[kbd] hid mount dev=%u idx=%u itf=%u desc=%u",
           dev_addr, idx, itfProto, desc_len);
  Serial1.println(line);

  if (itfProto != HID_ITF_PROTOCOL_KEYBOARD)
  {
    // A vendor, consumer-control or mouse interface. Keep one as a fallback so a
    // keyboard that exposes no boot interface still shows something; a real boot
    // keyboard mounting later takes the display over from it. A boot mouse is
    // never the fallback — arming it would only stream 4-byte reports that
    // cannot be displayed.
    if (g_kbd_idx == KBD_IDX_NONE && itfProto != HID_ITF_PROTOCOL_MOUSE)
    {
      clearKeyboard();
      g_kbd_idx = idx;
      g_kbd_addr = dev_addr;
      g_itf_mount_ms = millis();
      snprintf(line, sizeof(line),
               "[kbd] idx=%u not a boot keyboard - fallback, as declared", idx);
      Serial1.println(line);
      drawFrame();
    }
    return;
  }

  if (g_kbd_idx != KBD_IDX_NONE && g_kbd_idx != idx)
  {
    snprintf(line, sizeof(line), "[kbd] taking the display from idx=%u", g_kbd_idx);
    Serial1.println(line);
  }
  clearKeyboard();
  g_kbd_idx = idx;
  g_kbd_addr = dev_addr;
  g_kbd_boot_itf = true;
  g_itf_mount_ms = millis();

  // Deliberately no tuh_hid_set_protocol() and no tuh_hid_receive_report() here:
  // enumeration still has other interfaces to configure, and issuing a control
  // transfer from this callback races them. startKeyboard() does it at the
  // device-level mount instead.
  drawFrame();
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
  (void) dev_addr;

  if (idx != g_kbd_idx)
  {
    return;
  }

  clearKeyboard();
  Serial1.println("[kbd] hid unmounted");
  drawFrame();
}

void tuh_hid_set_protocol_complete_cb(uint8_t dev_addr, uint8_t idx, uint8_t protocol)
{
  char line[96];

  // hid_host.c's set_protocol_complete() invokes this whether the transfer
  // succeeded or not, and only updates its own mode on success — so this
  // argument is the whole story: HID_PROTOCOL_BOOT means the switch happened,
  // anything else means the interface is still in report protocol.
  if (protocol == HID_PROTOCOL_BOOT)
  {
    if (idx == g_kbd_idx)
    {
      g_kbd_boot = true;
    }
    snprintf(line, sizeof(line), "[kbd] boot protocol set dev=%u idx=%u", dev_addr, idx);
  }
  else
  {
    snprintf(line, sizeof(line), "[kbd] set_protocol FAILED dev=%u idx=%u, staying in report protocol",
             dev_addr, idx);
  }
  Serial1.println(line);

  if (idx == g_kbd_idx)
  {
    armReceive(dev_addr, idx);
    drawFrame();
  }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report, uint16_t len)
{
  // Anything that is not the interface on the display is left unarmed, so it
  // falls quiet after the one report that identified it.
  if (idx != g_kbd_idx)
  {
    return;
  }

  pushReport(report, len);

  // Re-arm: without this the keyboard reports exactly once.
  armReceive(dev_addr, idx);
}

}

void setup()
{
  // The board is the USB host, so the USB CDC Serial object is not a console:
  // it is the RUSB2 port TinyUSB owns. The console is the hardware UART.
  Serial1.begin(115200);

  // The Nano R4's Qwiic I2C port is Wire1 (pins 26/27); Wire is the A4/A5 pair.
  Wire1.begin();
  if (!myOLED.begin(Wire1))
  {
    Serial1.println("OLED begin failed - check the Qwiic I2C connector");
    while (true)
      delay(1000);
  }

  Serial1.println("Nano R4 USB host keyboard - waiting for a keyboard");
  drawFrame();

  tuh_arduino_init();
}

void loop()
{
  tuh_arduino_poll();

  // Safety net. If the device-level mount never arrives — an interface that stalls
  // configuration does exactly that — start anyway rather than sit permanently
  // deaf, and say so loudly: without it the OLED still reads "kbd ok" while no
  // report can ever arrive.
  if (g_kbd_idx != KBD_IDX_NONE && !g_kbd_started && g_itf_mount_ms != 0 &&
      millis() - g_itf_mount_ms > 500)
  {
    Serial1.println("[kbd] device-level mount did not complete, starting anyway");
    startKeyboard();
  }
}
