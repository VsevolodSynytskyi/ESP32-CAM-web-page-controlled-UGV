#pragma once

#include <esp_camera.h>

// OV2640 bring-up and runtime tuning. Shared by every stage from 1a onward.

// Initialise the sensor from the CAM_* settings in config.h. Falls back to a
// small DRAM framebuffer if PSRAM is missing, so a PSRAM fault degrades the
// picture instead of bricking the stage. Returns false if the sensor could not
// be detected at all.
bool camera_begin();

// Discard a few frames so the sensor's auto-gain and auto-white-balance settle.
// Worth calling once after init - the first frames out of an OV2640 are dark
// and can be badly exposed.
void camera_warmup(int frames = 4);

bool camera_set_framesize(framesize_t size);
bool camera_set_quality(int quality);  // 10..63, lower is better
bool camera_set_vflip(bool on);
bool camera_set_hmirror(bool on);

// True if the last camera_begin() had to fall back to a DRAM framebuffer.
bool camera_is_degraded();

// Cheap corruption check: a valid JPEG starts with FF D8 and ends with FF D9.
// Torn frames are the classic symptom of an XCLK the module cannot keep up
// with - see CAM_XCLK_HZ in config.h.
bool camera_frame_looks_valid(const camera_fb_t *fb);

// Fully stop the camera: sensor, XCLK and the I2S DMA all go away. Unlike
// pulling PWDN this leaves nothing running, so a network measurement taken
// afterwards reflects the radio alone rather than the radio plus a driver
// retrying failed captures.
void camera_end();

// False between camera_end() and the next camera_begin(). Callers must check
// this instead of relying on esp_camera_fb_get() returning null, which would
// otherwise spin the CPU on retries and corrupt the very measurement being made.
bool camera_is_running();
