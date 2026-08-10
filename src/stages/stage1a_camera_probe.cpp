// ===========================================================================
//  Stage 1a - camera bring-up with no networking at all
//
//  This is the real "check the ESP" milestone. It proves the OV2640, the SCCB
//  bus, PSRAM, the I2S parallel capture path and the JPEG encoder, with WiFi
//  entirely out of the picture - so a camera fault can never be mistaken for a
//  network fault.
//
//  Expected output: a sensor PID (0x26 for OV2640), then one line per second
//  reporting frame count, average frame size, measured FPS and free PSRAM.
//
//  Troubleshooting:
//
//    "sensor not detected"  -> ribbon cable not fully seated or latch open;
//                              or the 5V rail is sagging (the OV2640 browns out
//                              more easily than the ESP32 does).
//
//    "no PSRAM"             -> the board definition lost -DBOARD_HAS_PSRAM, or
//                              something is wired to GPIO16, which is the PSRAM
//                              chip-select on this module.
//
//    "INVALID JPEG"         -> torn frames. The module cannot keep up with the
//                              XCLK. Drop CAM_XCLK_HZ in config.h to 16500000,
//                              then 10000000.
//
//  Reflash this stage after Stage 2 to confirm the camera still initialises
//  with the motor driver wired. With GPIO16 unconnected, PSRAM should stay clean
//  - this is the check that proves it.
// ===========================================================================

#include <Arduino.h>

#include "../board.h"
#include "../camera.h"
#include "config.h"

static uint32_t s_frames = 0;
static uint32_t s_bytes = 0;
static uint32_t s_bad = 0;
static uint32_t s_failed = 0;
static uint32_t s_window_start = 0;

void setup() {
  board_begin("Stage 1a: camera probe");
  board_led_begin();

  Serial.printf("[cam] xclk %d Hz, quality %d, %d framebuffer(s)\n", CAM_XCLK_HZ,
                CAM_JPEG_QUALITY, CAM_FB_COUNT);

  if (!camera_begin()) {
    Serial.println(F("\n[cam] INIT FAILED - halting. See the notes at the top of"));
    Serial.println(F("[cam] stage1a_camera_probe.cpp. The LED will blink fast."));
    while (true) {
      board_led_heartbeat(200);
      delay(10);
    }
  }

  // Let auto-gain and auto-white-balance settle before we judge frame sizes.
  camera_warmup();

  // One detailed frame report, then continuous statistics.
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb != nullptr) {
    Serial.println();
    Serial.println(F("--- first frame ---"));
    Serial.printf(" size    : %u bytes\n", (unsigned)fb->len);
    Serial.printf(" dims    : %u x %u\n", (unsigned)fb->width, (unsigned)fb->height);
    Serial.printf(" format  : %s\n", fb->format == PIXFORMAT_JPEG ? "JPEG" : "NOT JPEG");
    Serial.printf(" markers : %s\n",
                  camera_frame_looks_valid(fb) ? "SOI/EOI present, looks intact"
                                               : "INVALID JPEG - see xclk note above");
    Serial.print(F(" head    : "));
    for (size_t i = 0; i < 8 && i < fb->len; i++) Serial.printf("%02X ", fb->buf[i]);
    Serial.print(F("... tail: "));
    for (size_t i = (fb->len > 4 ? fb->len - 4 : 0); i < fb->len; i++)
      Serial.printf("%02X ", fb->buf[i]);
    Serial.println();
    Serial.println(F("-------------------"));
    Serial.println();
    esp_camera_fb_return(fb);
  } else {
    Serial.println(F("[cam] init succeeded but the first capture returned null"));
  }

  s_window_start = millis();
}

void loop() {
  board_led_heartbeat(1000);

  camera_fb_t *fb = esp_camera_fb_get();
  if (fb == nullptr) {
    s_failed++;
  } else {
    s_frames++;
    s_bytes += fb->len;
    if (!camera_frame_looks_valid(fb)) s_bad++;
    // Returning the buffer promptly is what lets the driver keep capturing.
    // Holding one across an iteration starves the pipeline down to fb_count-1.
    esp_camera_fb_return(fb);
  }

  const uint32_t now = millis();
  const uint32_t elapsed = now - s_window_start;
  if (elapsed >= 1000) {
    const float fps = (s_frames * 1000.0f) / elapsed;
    const uint32_t avg = s_frames ? (s_bytes / s_frames) : 0;

    Serial.printf("fps %5.1f | avg frame %6u B | bad %lu | failed grabs %lu | psram free %u KB\n",
                  fps, (unsigned)avg, s_bad, s_failed,
                  (unsigned)(ESP.getFreePsram() / 1024));

    s_frames = 0;
    s_bytes = 0;
    s_window_start = now;
  }
}
