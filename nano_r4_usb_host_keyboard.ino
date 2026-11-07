// SPDX-License-Identifier: MIT
//
// nano_r4_usb_host_keyboard.ino — Arduino Nano R4 as a USB HOST for a HID
// keyboard. The raw 8-byte USB HID report is rendered on a SparkFun Qwiic
// "Narrow" OLED (0.91 in., 128x32) over the Qwiic I2C connector.
//
// Screen
//   row 1: LC LS LA LG RC RS RA RG   a label appears while that modifier bit is set
//   row 2: six USB keycodes, two hex digits each, in report order
//   row 3: mount status ("no kbd" until the keyboard is mounted)
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

static uint8_t g_report[8] = {0};
static bool g_mounted = false;

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

  myOLED.text(xKey, yStatus, g_mounted ? "kbd ok" : "no kbd", COLOR_WHITE);

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

extern "C"
{

void tuh_mount_cb(uint8_t dev_addr)
{
  uint16_t vid = 0;
  uint16_t pid = 0;
  tuh_vid_pid_get(dev_addr, &vid, &pid);

  char line[64];
  snprintf(line, sizeof(line), "[kbd] mount dev=%u vid=%04X pid=%04X", dev_addr, vid, pid);
  Serial1.println(line);
}

void tuh_umount_cb(uint8_t dev_addr)
{
  (void) dev_addr;
  g_mounted = false;
  memset(g_report, 0, sizeof(g_report));
  Serial1.println("[kbd] device unmounted");
  drawFrame();
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report_desc, uint16_t desc_len)
{
  (void) report_desc;

  char line[64];
  snprintf(line, sizeof(line), "[kbd] hid mount dev=%u idx=%u desc=%u", dev_addr, idx, desc_len);
  Serial1.println(line);

  g_mounted = true;
  drawFrame();

  // Force the HID boot protocol so every report is the canonical 8-byte
  // keyboard report, whatever report descriptor this keyboard declared.
  if (idx == 0)
  {
    if (!tuh_hid_set_protocol(dev_addr, idx, 0))
    {
      Serial1.println("[kbd] set_protocol failed, receiving as declared");
      tuh_hid_receive_report(dev_addr, idx);
    }
  }
  else
  {
    tuh_hid_receive_report(dev_addr, idx);
  }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx)
{
  (void) dev_addr;
  (void) idx;
  memset(g_report, 0, sizeof(g_report));
  Serial1.println("[kbd] hid unmounted");
  drawFrame();
}

void tuh_hid_set_protocol_complete_cb(uint8_t dev_addr, uint8_t idx, uint8_t protocol)
{
  char line[48];
  snprintf(line, sizeof(line), "[kbd] boot protocol set dev=%u idx=%u", dev_addr, idx);
  Serial1.println(line);
  (void) protocol;

  tuh_hid_receive_report(dev_addr, idx);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report, uint16_t len)
{
  if (idx == 0)
  {
    pushReport(report, len);
  }

  // Re-arm: without this the keyboard reports exactly once.
  tuh_hid_receive_report(dev_addr, idx);
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
}
