// ===========================================================================
//  ESP32-CAM web page controlled UGV
//
//  Live MJPEG video from an AI-Thinker ESP32-CAM over its own SoftAP.
//  Motor control from the web page is the next stage; the driver inputs are
//  parked here so the vehicle cannot move.
//
//  Boot order is deliberate. Motors are parked before anything else can take
//  time, and the camera comes up before the radio so a sensor fault halts with
//  an unambiguous error instead of looking like a network problem.
// ===========================================================================

#include <Arduino.h>
#include <WiFi.h>

#include "board.h"
#include "camera.h"
#include "config.h"
#include "motors.h"
#include "net.h"
#include "stream_server.h"

static bool g_ap_mode = false;

// The TB6612 is awake before our code runs, because STBY and PWMA/PWMB are tied
// to 3V3. Whatever level reset leaves on the direction pins IS a command to it,
// so the pins are paired by measured boot state - see config.h. This survey is
// the guard: if a pin ever changes behaviour, it says so at startup rather than
// letting you discover it as a motor running at full throttle.
static void report_boot_pin_levels() {
  const int a1 = digitalRead(PIN_AIN1), a2 = digitalRead(PIN_AIN2);
  const int b1 = digitalRead(PIN_BIN1), b2 = digitalRead(PIN_BIN2);
  motors_begin();

  Serial.printf("[motors] A GPIO%d=%d GPIO%d=%d   B GPIO%d=%d GPIO%d=%d\n", PIN_AIN1, a1, PIN_AIN2,
                a2, PIN_BIN1, b1, PIN_BIN2, b2);
  if (a1 != a2 || b1 != b2) {
    Serial.println(F("[motors] *** MISMATCHED PAIR - a motor runs during boot ***"));
    Serial.println(F("[motors] pair the pins by boot level, see config.h"));
  }
}

static void halt_blinking(const char *why) {
  Serial.printf("\n[fatal] %s - halting.\n", why);
  for (;;) {
    board_led_heartbeat(200);
    delay(10);
  }
}

void setup() {
  report_boot_pin_levels();

  board_begin("camera + streaming");
  board_led_begin();

  if (!camera_begin()) halt_blinking("camera init failed");
  camera_warmup();

  if (net_begin_sta()) {
    g_ap_mode = false;
  } else {
    Serial.println(F("[net] falling back to SoftAP"));
    if (!net_begin_ap()) halt_blinking("no network");
    g_ap_mode = true;
  }

  if (!stream_server_begin(HTTP_PORT)) halt_blinking("stream server failed");

  const IPAddress ip = g_ap_mode ? WiFi.softAPIP() : WiFi.localIP();
  Serial.printf("\n  open http://%s/ in a browser", ip.toString().c_str());
  if (g_ap_mode) Serial.printf(" (join \"%s\" first)", AP_SSID);
  Serial.println("\n");
}

void loop() {
  board_led_heartbeat(stream_server_has_client() ? 250 : 1500);
  delay(10);
}
