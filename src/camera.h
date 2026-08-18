#pragma once

#include <esp_camera.h>

// OV2640 bring-up. Every setting comes from the CAM_* block in config.h.
//
// Returns false if the sensor could not be detected at all. Falls back to a
// small DRAM framebuffer if PSRAM is missing, so a PSRAM fault degrades the
// picture rather than preventing boot.
bool camera_begin();

// Discard a few frames so auto-gain and auto-white-balance settle. The first
// frames out of an OV2640 are dark and badly exposed.
void camera_warmup(int frames = 4);
