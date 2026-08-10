// ===========================================================================
//  Stage 1b - MJPEG stream over station mode, viewed on a laptop
//
//  Station mode is deliberate for this stage: the laptop keeps its internet
//  connection, DevTools and a real keyboard, which makes debugging the video
//  path far easier than doing it on a phone. The SoftAP arrives in Stage 3.
//
//  Setup: copy include/secrets.h.example to include/secrets.h and fill in your
//  2.4 GHz network details. The ESP32 cannot see a 5 GHz-only network.
//
//  Then read the IP off the serial monitor and open it on the laptop:
//      http://<ip>/          bare viewer page
//      http://<ip>/stream    the raw MJPEG stream
//
//  Log the FPS line for a few minutes. VGA at quality 12 should hold roughly
//  20-25 fps on a decent link. If it is far below that, check RSSI in the
//  banner - and check the antenna-select solder jumper next to the U.FL
//  connector, because a board set to "external" with no antenna fitted has
//  terrible range.
//
//  Exit criteria: live video in the browser, FPS recorded, no resets over five
//  minutes. Watch for BROWNOUT in the reset reason on any restart.
// ===========================================================================

#include <Arduino.h>
#include <WiFi.h>

#include "../board.h"
#include "../camera.h"
#include "../net.h"
#include "../stream_server.h"
#include "config.h"

static void halt_blinking(const char *why) {
  Serial.printf("\n[stage1b] %s - halting.\n", why);
  while (true) {
    board_led_heartbeat(200);
    delay(10);
  }
}

void setup() {
  board_begin("Stage 1b: MJPEG over STA");
  board_led_begin();

  // Camera first. If it fails there is no point bringing up the radio, and the
  // error is unambiguous rather than tangled up with a network problem.
  if (!camera_begin()) halt_blinking("camera init failed, see Stage 1a notes");
  camera_warmup();

  if (!net_begin_sta()) halt_blinking("WiFi failed");

  // Port 80 with the built-in viewer page: nothing else is listening this
  // stage, so the two-instance split is not needed until Stage 3.
  if (!stream_server_begin(80, /*with_index=*/true)) halt_blinking("stream server failed");

  Serial.println();
  Serial.printf("  open http://%s/  on your laptop\n", WiFi.localIP().toString().c_str());
  Serial.println();
}

void loop() {
  // The stream runs inside the httpd task, so this loop only reports. A slow
  // heartbeat when idle, fast when a client is watching.
  board_led_heartbeat(stream_server_has_client() ? 250 : 1500);

  static uint32_t last = 0;
  if (millis() - last >= 1000) {
    last = millis();
    if (stream_server_has_client()) {
      Serial.printf("streaming %5.1f fps | rssi %d dBm | heap %u KB | psram %u KB\n",
                    stream_server_fps(), WiFi.RSSI(),
                    (unsigned)(ESP.getFreeHeap() / 1024),
                    (unsigned)(ESP.getFreePsram() / 1024));
    } else {
      Serial.printf("idle, waiting for a client at http://%s/\n",
                    WiFi.localIP().toString().c_str());
    }
  }

  delay(10);
}
