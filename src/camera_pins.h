#pragma once

// ===========================================================================
//  OV2640 pin map for the AI-Thinker ESP32-CAM.
//
//  Verified against the map bundled with Arduino-ESP32 core 2.0.17 at
//  framework-arduinoespressif32/libraries/ESP32/examples/Camera/
//    CameraWebServer/camera_pins.h  (CAMERA_MODEL_AI_THINKER block).
//
//  Kept as our own copy rather than reaching into the framework's examples
//  directory, which is not on the include path and moves between core versions.
//
//  These 15 pins plus GPIO1/3 (serial) and GPIO16 (PSRAM chip-select) are why
//  only GPIO 2, 12, 13, 14, 15 are left for the motor driver.
// ===========================================================================

#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1  // no reset line broken out on this module
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26  // SCCB (I2C-like) data
#define SIOC_GPIO_NUM 27  // SCCB clock

#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5

#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22
