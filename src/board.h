#pragma once

#include <stdint.h>

// Board housekeeping shared by every stage: serial bring-up, a boot banner that
// makes power faults distinguishable from crashes, and the onboard status LED.

// Serial.begin(115200) plus a banner reporting the stage name, reset reason,
// heap and PSRAM. Call this first in every setup().
void board_begin(const char *stage_name);

// Onboard red LED beside the antenna (GPIO33). ACTIVE LOW - board_led(true)
// drives the pin low. Not the blinding white flash LED on GPIO4.
void board_led_begin();
void board_led(bool on);

// Non-blocking heartbeat; call from loop(). Toggles every period_ms / 2.
void board_led_heartbeat(uint32_t period_ms);

// Human-readable esp_reset_reason(). "BROWNOUT" here during hard acceleration
// means the power supply needs work - bulk capacitance and a star ground - not
// that the firmware is buggy.
const char *board_reset_reason_str();
