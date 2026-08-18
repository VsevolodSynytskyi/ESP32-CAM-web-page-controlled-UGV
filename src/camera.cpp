#include "camera.h"

#include <Arduino.h>

#include "camera_pins.h"
#include "config.h"

// Push the CAM_* profile to the sensor. Return values are ignored: the OV2640
// does not implement every control the sensor_t interface exposes, and a driver
// saying "not supported on this part" is not a boot failure.
static void apply_profile(sensor_t *s) {
  s->set_brightness(s, CAM_BRIGHTNESS);
  s->set_contrast(s, CAM_CONTRAST);
  s->set_saturation(s, CAM_SATURATION);
  s->set_sharpness(s, CAM_SHARPNESS);
  s->set_special_effect(s, CAM_SPECIAL_EFFECT);

  s->set_whitebal(s, CAM_AWB);
  s->set_awb_gain(s, CAM_AWB_GAIN);
  s->set_wb_mode(s, CAM_WB_MODE);

  s->set_exposure_ctrl(s, CAM_AEC);
  s->set_aec2(s, CAM_AEC2);
  s->set_ae_level(s, CAM_AE_LEVEL);
  s->set_aec_value(s, CAM_AEC_VALUE);

  s->set_gain_ctrl(s, CAM_AGC);
  s->set_agc_gain(s, CAM_AGC_GAIN);
  s->set_gainceiling(s, (gainceiling_t)CAM_GAINCEILING);

  s->set_bpc(s, CAM_BPC);
  s->set_wpc(s, CAM_WPC);
  s->set_raw_gma(s, CAM_RAW_GMA);
  s->set_lenc(s, CAM_LENC);
  s->set_dcw(s, CAM_DCW);
  s->set_colorbar(s, CAM_COLORBAR);

  s->set_vflip(s, CAM_VFLIP);
  s->set_hmirror(s, CAM_HMIRROR);
}

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
  c.pixel_format = PIXFORMAT_JPEG;
  c.sccb_i2c_port = 0;

  // The camera driver generates XCLK with its own LEDC timer and channel. These
  // must not collide with the motor PWM, which owns channels 0-3 on timers 0
  // and 1 (see LEDC_CH_* in config.h).
  c.ledc_timer = LEDC_TIMER_3;
  c.ledc_channel = LEDC_CHANNEL_7;

  const bool psram = psramFound();
  if (psram) {
    // Initialise at the largest size PSRAM can hold, then drop to the working
    // size. The driver sizes its DMA descriptors and framebuffers once, from
    // this value, so starting large means any later switch down always fits.
    c.frame_size = ESP.getPsramSize() > 3 * 1024 * 1024 ? FRAMESIZE_UXGA : FRAMESIZE_SVGA;
    c.jpeg_quality = 10;
    c.fb_count = CAM_FB_COUNT;
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    // Internal DRAM is ~320 kB and shared with WiFi, so one small buffer is all
    // that fits. GRAB_LATEST is meaningless with a single buffer.
    Serial.println(F("[cam] no PSRAM - falling back to a single QVGA DRAM buffer"));
    c.frame_size = FRAMESIZE_QVGA;
    c.jpeg_quality = 15;
    c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_DRAM;
    c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  const esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("[cam] init failed: 0x%04x (%s)\n", err, esp_err_to_name(err));
    Serial.println(F("[cam] check the ribbon is fully seated and the latch closed"));
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s == nullptr) {
    Serial.println(F("[cam] init reported OK but the sensor handle is null"));
    return false;
  }

  if (psram) {
    // Buffers were allocated for the maximum above, so this is just a sensor
    // register change.
    s->set_framesize(s, CAM_FRAMESIZE);
    s->set_quality(s, CAM_JPEG_QUALITY);
  }
  apply_profile(s);

  Serial.printf("[cam] OV2640 (PID 0x%02x) up: xclk %d Hz, quality %d, %d buffer(s)\n", s->id.PID,
                CAM_XCLK_HZ, CAM_JPEG_QUALITY, c.fb_count);
  return true;
}

void camera_warmup(int frames) {
  for (int i = 0; i < frames; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb != nullptr) esp_camera_fb_return(fb);
  }
}
