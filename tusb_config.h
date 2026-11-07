// SPDX-License-Identifier: MIT
/*
 * tusb_config.h — TinyUSB configuration for THIS SKETCH ONLY.
 *
 * Live because the sketch is built against the unor4tinyusb board package,
 * whose platform.txt adds -iquote{build.source.path}. -iquote directories are
 * searched before -I, so this copy wins for every translation unit, including
 * the core's own vendored TinyUSB sources. Nothing global is touched.
 *
 * It must stay SELF-CONTAINED: the core's TinyUSB translation units get five
 * include directories (cores/arduino/tinyusb, two api/deprecated dirs,
 * cores/arduino, variants/<V>) — not src/, not this folder. A config that
 * #included a helper header would resolve for the sketch and fail with
 * "No such file or directory" inside cdc_device.c.
 *
 * Trimmed to what this demo needs: one host HID slot, no hub, no host CDC/MSC.
 * The device stack is NOT trimmed, because two core files are compiled
 * unconditionally and require it (see the floor note below).
 */

#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

// Board check. UnoR4TinyUSB/board_auto.h is NOT reachable from here for the
// same include-path reason as above; derive the board from the core's own
// command-line macro, which is present in every TU.
#if !defined(ARDUINO_ARCH_RENESAS)
  #error "Unsupported board: this config is for the Arduino Renesas RA4M1 core (renesas_uno). No ARDUINO_ARCH_RENESAS on the command line."
#endif
#ifndef UNOR4_TINYUSB_BOARD_R4
  #define UNOR4_TINYUSB_BOARD_R4 1
#endif

// ---- Device stack ----
// CFG_TUD_ENABLED and CFG_TUD_CDC are floors, not choices:
//   USB.cpp        __SetupUSBDescriptor() builds a TUD_CDC_DESCRIPTOR outside
//                  any #if, and __USBStart() calls tud_rhport_init().
//   SerialUSB.cpp  the _SerialUSB object calls tud_cdc_connected/peek/
//                  available/read_char/write/write_available/write_flush.
// Setting CFG_TUD_CDC 0 fails to compile THE CORE, not just the sketch.
#define CFG_TUD_ENABLED                      1
#define CFG_TUD_CDC                          1

// ---- Host stack ----
#define CFG_TUH_ENABLED                      1
#define CFG_TUH_HID                          1  // one keyboard, no hub

// ---- MCU / RHPort ----------------------------------------------------------
// CFG_TUSB_MCU is NOT defined here: boards.txt passes
// -DCFG_TUSB_MCU=OPT_MCU_RAXXX for every renesas_uno target.
//
// CFG_TUSB_RHPORT0_MODE must stay OPT_MODE_DEVICE. Do NOT "helpfully" add
// OPT_MODE_HOST or OPT_MODE_FULL_SPEED: CFG_TUH_ENABLED above is what
// switches the host stack on, and the RUSB2 HCD is full-speed only and picks
// its PHY setup from rusb2_is_highspeed_rhport(), not from this macro.
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE)

#ifndef CFG_TUSB_OS
  #define CFG_TUSB_OS           OPT_OS_NONE
#endif

// rhport 0 is the only RUSB2 port. BOARD_TUH_RHPORT must stay UNDEFINED: the
// core guards its host block on it, and defining it makes __USBStart() call
// tuh_rhport_init() before the sketch has chosen a role, which hangs.
#ifndef BOARD_TUD_RHPORT
  #define BOARD_TUD_RHPORT     0
#endif

// The R4's USBFS endpoint-0 max packet size is 64 in both directions.
#ifndef CFG_TUD_ENDPOINT0_SIZE
  #define CFG_TUD_ENDPOINT0_SIZE    64
#endif

// ---- Event queues ----------------------------------------------------------
// Deep, and NOT to be trimmed. Enumeration on the R4 is bursty enough to
// overflow the upstream default of 16, which silently drops SETUP /
// xfer-complete events: the device ACKs the setups in hardware, software never
// sees them, and the host sees endless NAKs. ~768 B of the 32 KB SRAM. Keep it.
#define CFG_TUD_TASK_QUEUE_SZ   64
#define CFG_TUH_TASK_QUEUE_SZ   64

// ---- Host limits (hardware, not preference) --------------------------------
// The RUSB2 HCD hard-asserts dev_addr < 6 and has 5 bulk pipes. Do NOT raise
// CFG_TUH_DEVICE_MAX past 4-5: at 8 enumeration dies entirely.
#ifndef CFG_TUH_DEVICE_MAX
  #define CFG_TUH_DEVICE_MAX        4
#endif

// ---- Debug ------------------------------------------------------------------
// Off by default. TU_LOG runs inside the USB ISR, and a ~40-char line at
// 115200 baud blocks for ~3.5 ms. -DCFG_TUSB_DEBUG=2 re-enables it for bring-up.
#ifndef CFG_TUSB_DEBUG
  #define CFG_TUSB_DEBUG           0
#endif
// Outside the guard on purpose: arduino_debug_printf() (src/UnoR4TinyUSB/
// bsp_common.cpp) must resolve even while logging is off, or the opt-in fails
// at link.
#define CFG_TUSB_DEBUG_PRINTF    arduino_debug_printf

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */
