#include "board.h"

#include <Arduino.h>
#include <esp_system.h>

#include "config.h"

const char *board_reset_reason_str() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_EXT:      return "EXTERNAL";
    case ESP_RST_SW:       return "SOFTWARE";
    case ESP_RST_PANIC:    return "PANIC (crash)";
    case ESP_RST_INT_WDT:  return "INTERRUPT WDT";
    case ESP_RST_TASK_WDT: return "TASK WDT";
    case ESP_RST_WDT:      return "OTHER WDT";
    case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT  <-- power supply, not firmware";
    case ESP_RST_SDIO:     return "SDIO";
    default:               return "UNKNOWN";
  }
}

void board_begin(const char *stage_name) {
  Serial.begin(115200);

  // The USB-serial bridge on the MB shield needs a moment after reset before
  // the host reopens the port, otherwise the banner is printed into the void.
  delay(300);

  Serial.println();
  Serial.println(F("========================================"));
  Serial.print(F(" ESP32-CAM web page controlled UGV - "));
  Serial.println(stage_name);
  Serial.println(F("========================================"));
  Serial.printf(" reset reason : %s\n", board_reset_reason_str());
  Serial.printf(" chip         : %s rev %d, %d core(s) @ %u MHz\n",
                ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
                (unsigned)getCpuFrequencyMhz());
  Serial.printf(" flash        : %u KB\n", (unsigned)(ESP.getFlashChipSize() / 1024));
  Serial.printf(" heap free    : %u KB of %u KB\n",
                (unsigned)(ESP.getFreeHeap() / 1024),
                (unsigned)(ESP.getHeapSize() / 1024));

  // PSRAM is what makes VGA framebuffers possible. Zero here means either the
  // board definition lost -DBOARD_HAS_PSRAM, or GPIO16 got wired to something
  // (reportedly the PSRAM chip-select on 4 MB PSRAM modules).
  if (ESP.getPsramSize() > 0) {
    Serial.printf(" psram free   : %u KB of %u KB\n",
                  (unsigned)(ESP.getFreePsram() / 1024),
                  (unsigned)(ESP.getPsramSize() / 1024));
  } else {
    Serial.println(F(" psram        : NOT FOUND  <-- camera will be limited to tiny frames"));
    Serial.println(F("                check -DBOARD_HAS_PSRAM, and unwire GPIO16 if used"));
  }
  Serial.println(F("========================================"));
  Serial.println();
}

void board_led_begin() {
  pinMode(PIN_STATUS_LED, OUTPUT);
  board_led(false);
}

void board_led(bool on) {
  // Active low.
  digitalWrite(PIN_STATUS_LED, on ? LOW : HIGH);
}

void board_led_heartbeat(uint32_t period_ms) {
  static uint32_t last = 0;
  static bool on = false;

  const uint32_t half = period_ms / 2;
  const uint32_t now = millis();
  if (now - last >= half) {
    last = now;
    on = !on;
    board_led(on);
  }
}
