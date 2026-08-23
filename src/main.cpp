// ===========================================================================
//  ESP32-CAM web page controlled UGV
//
//  Live MJPEG video from an AI-Thinker ESP32-CAM over its own SoftAP, with
//  hold-to-run motor buttons on the same page.
//
//  Boot order is deliberate. Motors are parked before anything else can take
//  time, and the camera comes up before the radio so its result is in the log
//  before the network noise. A camera fault is reported, not fatal - losing the
//  radio too would leave nothing to diagnose it with.
// ===========================================================================

#include <Arduino.h>
#include <WiFi.h>

#include "board.h"
#include "camera.h"
#include "config.h"
#include "motors.h"
#include "net.h"
#include "stream_server.h"
#include "web_server.h"

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

  board_begin("camera + streaming + motors");
  board_led_begin();

  // Only now, with Serial up so the failsafe can say so. The pins have been
  // parked since motors_begin() above; this starts enforcing that.
  motors_start_task();

  // Deliberately not fatal. The radio is how you reach the vehicle - to see the
  // error, to drive it off whatever it is stuck on, to reflash it. Halting here
  // for a sensor fault means a board that looks completely dead, which is what
  // a failed cold-boot camera init used to produce.
  if (camera_begin()) {
    camera_warmup();
  } else {
    Serial.println(F("[cam] no camera - continuing so the page and controls still work"));
  }

  if (net_begin_sta()) {
    g_ap_mode = false;
  } else {
    Serial.println(F("[net] falling back to SoftAP"));
    if (!net_begin_ap()) halt_blinking("no network");
    g_ap_mode = true;
  }

  if (!web_server_begin(HTTP_PORT)) halt_blinking("web server failed");
  if (!stream_server_begin(HTTP_PORT + 1)) halt_blinking("video server failed");

  const IPAddress ip = g_ap_mode ? WiFi.softAPIP() : WiFi.localIP();
  Serial.printf("\n  open http://%s/ in a browser", ip.toString().c_str());
  if (g_ap_mode) Serial.printf(" (join \"%s\" first)", AP_SSID);
  Serial.println("\n");
}

void loop() {
  board_led_heartbeat(stream_server_has_client() ? 250 : 1500);
  delay(10);
}
