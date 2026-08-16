#include "camera.h"

#include <Arduino.h>

#include "camera_pins.h"
#include "config.h"

static bool s_degraded = false;

bool camera_is_degraded() { return s_degraded; }

bool camera_begin() {
  camera_config_t c = {};

  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;

  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d0 = Y2_GPIO_NUM;

  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;

  c.xclk_freq_hz = CAM_XCLK_HZ;

  // The camera driver generates XCLK with its own LEDC timer and channel. These
  // must not collide with the motor PWM, which owns channels 0-3 on timers 0
  // and 1 (see LEDC_CH_* in config.h). Timer 3 / channel 7 keeps them apart.
  c.ledc_timer = LEDC_TIMER_3;
  c.ledc_channel = LEDC_CHANNEL_7;

  c.pixel_format = PIXFORMAT_JPEG;
  c.sccb_i2c_port = -1;

  c.sccb_i2c_port = 0;

  if (psramFound()) {
    // Initialise at the LARGEST size PSRAM can hold, then drop to the working
    // size immediately after. The driver sizes its DMA descriptors and frame
    // buffers once, at init, from this value - so starting large means any
    // later switch down always fits, with no reallocation. Taken from the
    // MJPEG2SD reference, which does the same thing for the same reason.
    framesize_t max_fs = FRAMESIZE_SVGA;
    if (ESP.getPsramSize() > 3 * 1024 * 1024) max_fs = FRAMESIZE_UXGA;

    c.frame_size = max_fs;
    c.jpeg_quality = 10;
    c.fb_count = CAM_FB_COUNT;
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.grab_mode = CAMERA_GRAB_LATEST;
    s_degraded = false;
  } else {
    // Degraded fallback. Internal DRAM is only ~320 KB and shared with WiFi, so
    // one small buffer is all that fits. GRAB_LATEST is meaningless with a
    // single buffer, hence WHEN_EMPTY.
    Serial.println(F("[cam] no PSRAM - falling back to a single QVGA DRAM buffer"));
    Serial.println(F("[cam] check -DBOARD_HAS_PSRAM and that GPIO16 is unwired"));
    c.frame_size = FRAMESIZE_QVGA;
    c.jpeg_quality = 15;
    c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_DRAM;
    c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    s_degraded = true;
  }

  const esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("[cam] esp_camera_init failed: 0x%04x (%s)\n", err, esp_err_to_name(err));
    if (err == ESP_ERR_CAMERA_NOT_DETECTED) {
      Serial.println(F("[cam] sensor not detected. Check the ribbon cable is fully"));
      Serial.println(F("[cam] seated and the latch is closed, and that the board is"));
      Serial.println(F("[cam] getting a solid 5V - the OV2640 browns out easily."));
    }
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s == nullptr) {
    Serial.println(F("[cam] init reported OK but sensor handle is null"));
    return false;
  }

  Serial.printf("[cam] sensor PID 0x%02x initialised at %s\n", s->id.PID,
                s_degraded ? "QVGA/DRAM (degraded)" : "max size in PSRAM");

  // Now drop to the working size. Buffers were already allocated for the
  // maximum above, so this is just a sensor register change.
  if (!s_degraded) {
    camera_set_framesize(CAM_FRAMESIZE);
    camera_set_quality(CAM_JPEG_QUALITY);
  }

  camera_set_vflip(CAM_VFLIP != 0);
  camera_set_hmirror(CAM_HMIRROR != 0);

  return true;
}

void camera_warmup(int frames) {
  for (int i = 0; i < frames; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb != nullptr) esp_camera_fb_return(fb);
  }
}

bool camera_set_framesize(framesize_t size) {
  sensor_t *s = esp_camera_sensor_get();
  return s != nullptr && s->set_framesize(s, size) == 0;
}

bool camera_set_quality(int quality) {
  sensor_t *s = esp_camera_sensor_get();
  if (s == nullptr) return false;
  if (quality < 10) quality = 10;
  if (quality > 63) quality = 63;
  return s->set_quality(s, quality) == 0;
}

bool camera_set_vflip(bool on) {
  sensor_t *s = esp_camera_sensor_get();
  return s != nullptr && s->set_vflip(s, on ? 1 : 0) == 0;
}

bool camera_set_hmirror(bool on) {
  sensor_t *s = esp_camera_sensor_get();
  return s != nullptr && s->set_hmirror(s, on ? 1 : 0) == 0;
}

bool camera_frame_looks_valid(const camera_fb_t *fb) {
  if (fb == nullptr || fb->format != PIXFORMAT_JPEG || fb->len < 4) return false;

  const bool soi = fb->buf[0] == 0xFF && fb->buf[1] == 0xD8;
  const bool eoi = fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9;
  return soi && eoi;
}
