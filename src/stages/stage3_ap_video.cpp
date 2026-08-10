// ===========================================================================
//  Stage 3 - SoftAP + web page + video on the phone. No motor control yet.
//
//  The vehicle hosts its own network, so no router is involved and it works
//  anywhere. Join the AP from your phone and open http://192.168.4.1/
//
//    SSID     AP_SSID     in config.h  (default "TankCam")
//    password AP_PASSWORD in config.h  (default "tankcam1234")
//
//  Two esp_http_server instances, not one:
//
//    :80  the UI page
//    :81  the MJPEG stream
//
//  An MJPEG response never ends. On a single instance it permanently occupies
//  the worker and the control endpoint stops answering - which is exactly the
//  failure you cannot afford while steering. Splitting them now means Stage 4
//  only has to add the WebSocket, with the concurrency already proven.
//
//  Phone-side gotchas, all expected:
//    - iOS and Android will both warn about "no internet".
//    - Android may silently fall back to cellular. Turn mobile data off, or
//      tell it to stay connected to this network.
//    - If range is poor, check the antenna-select solder jumper next to the
//      U.FL connector. A board set to "external" with no antenna fitted has
//      terrible range.
//
//  Exit criteria: page loads, video is live and full-screen, and it stays stable
//  for several minutes while you carry the vehicle around by hand.
// ===========================================================================

#include <Arduino.h>
#include <WiFi.h>

#include "../board.h"
#include "../camera.h"
#include "../motors.h"
#include "../net.h"
#include "../stream_server.h"
#include "../web_server.h"
#include "config.h"

static void halt_blinking(const char *why) {
  Serial.printf("\n[stage3] %s - halting.\n", why);
  while (true) {
    board_led_heartbeat(200);
    delay(10);
  }
}

void setup() {
  board_begin("Stage 3: SoftAP + video");
  board_led_begin();

  if (!camera_begin()) halt_blinking("camera init failed, see Stage 1a notes");
  camera_warmup();

  // The driver is wired by now, so park the motor pins in a defined coasting
  // state rather than leaving them floating. motors_tick() is never called this
  // stage, so the outputs cannot leave coast - there is no way for a
  // control-less build to drive the tracks.
  motors_begin();

  if (!net_begin_ap()) halt_blinking("SoftAP failed to start");

  if (!web_server_begin(/*with_control=*/false)) halt_blinking("web server failed");
  if (!stream_server_begin(STREAM_PORT, /*with_index=*/false))
    halt_blinking("stream server failed");

  Serial.println();
  Serial.printf("  join \"%s\" then open  http://%s/\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());
  Serial.println();
}

void loop() {
  board_led_heartbeat(stream_server_has_client() ? 250 : 1500);

  static uint32_t last = 0;
  if (millis() - last >= 1000) {
    last = millis();
    Serial.printf("clients %d | stream %5.1f fps | heap %u KB | psram %u KB\n",
                  WiFi.softAPgetStationNum(), stream_server_fps(),
                  (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
  }

  delay(10);
}
