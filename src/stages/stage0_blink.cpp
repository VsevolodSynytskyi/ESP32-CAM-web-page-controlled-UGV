// ===========================================================================
//  Stage 0 - toolchain and hardware verification
//
//  Proves the upload path and the wiring before any real firmware exists.
//  No camera, no WiFi, no motors: if this does not work, nothing later will,
//  and you want to find that out now rather than while debugging a video
//  stream.
//
//  Flash it twice:
//
//    1. With NOTHING attached to the motor driver pins. Confirms the port,
//       baud rate, auto-reset and the whole `pio run -t upload` path.
//
//    2. With the TB6612FNG wired up. This is the strapping-pin proof. GPIO2
//       and GPIO12 are both strapping pins; if the driver's inputs pull either
//       one high at boot, things break in confusing ways:
//
//         GPIO2 high  -> the chip refuses download mode, uploads fail
//         GPIO12 high -> VDD_SDIO drops to 1.8V and the board appears dead
//
//       TB6612FNG inputs are internally pulled down, so this normally passes.
//       If upload now fails, change PIN_BIN2 in config.h from 2 to 12 and move
//       that one wire.
//
//  Expected: banner over serial, red LED beside the antenna blinking at 1 Hz,
//  and a once-per-second line showing uptime and free heap.
//
//  Note: once GPIO15 is wired to the driver its internal pull-down silences the
//  ROM bootloader's own startup chatter. That is cosmetic and expected - our
//  banner still prints.
// ===========================================================================

#include <Arduino.h>

#include "../board.h"
#include "config.h"

void setup() {
  board_begin("Stage 0: blink + serial");
  board_led_begin();

  Serial.println(F("Motor driver pins configured in config.h:"));
  Serial.printf("  AIN1 = GPIO%-2d   AIN2 = GPIO%d   (left track)\n", PIN_AIN1, PIN_AIN2);
  Serial.printf("  BIN1 = GPIO%-2d   BIN2 = GPIO%d   (right track)\n", PIN_BIN1, PIN_BIN2);
  Serial.println(F("PWMA, PWMB, STBY and VCC must be tied to 3.3V."));
  Serial.println(F("GPIO12 and GPIO16 must be left unconnected."));
  Serial.println();
  Serial.println(F("If you can read this with the driver wired, the strapping"));
  Serial.println(F("pins are safe and every later stage is on solid ground."));
  Serial.println();

  // Deliberately left as inputs. Stage 0 must not drive the motor pins - the
  // wheels may be on the ground and nothing has current-limited the driver yet.
}

void loop() {
  board_led_heartbeat(1000);

  static uint32_t last = 0;
  if (millis() - last >= 1000) {
    last = millis();
    Serial.printf("[%6lus] alive, heap %u KB free\n", millis() / 1000,
                  (unsigned)(ESP.getFreeHeap() / 1024));
  }
}
