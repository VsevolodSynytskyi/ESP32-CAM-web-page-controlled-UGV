// ===========================================================================
//  Stage 4 - the shipping firmware: live video plus real-time bidirectional
//  throttle from the phone.
//
//  Join AP_SSID and open http://192.168.4.1/
//
//  Control surface: two full-height vertical sliders, one per track.
//
//      top    = +100  full throttle FORWARD
//      middle =    0  stop (+/- MOTOR_DEADBAND)
//      bottom = -100  full throttle BACKWARD
//      release ->  0  springs back to centre
//
//  No mixing - each slider drives its own motor 1:1, which is the point of a
//  tracked vehicle. Both up to go forward, opposed to pivot in place.
//
//  Transport is a native WebSocket on port 80 (CONFIG_HTTPD_WS_SUPPORT is
//  enabled in this core's prebuilt sdkconfig, so no external library is
//  involved): a 2-byte binary frame carrying signed int8 left and right at
//  20 Hz, plus an out-of-band send the moment a slider is released.
//
//  Layered stopping, weakest assumption last:
//
//    1. Release a slider          -> zero sent immediately
//    2. Screen off / backgrounded -> visibilitychange zeroes and sends
//    3. Socket closes             -> close_fn stops the motors at once
//    4. Nothing arrives for       -> CMD_TIMEOUT_MS failsafe in motors_tick(),
//       CMD_TIMEOUT_MS               which trusts none of the above
//
//  Total stopping distance in the worst case is the 300 ms detection window plus
//  roughly 100 ms of slew-limited ramp-down. Tighten CMD_TIMEOUT_MS if you want
//  it shorter, but going much below ~200 ms invites nuisance stops from ordinary
//  SoftAP jitter.
//
//  Before trusting this stage, fill in the four MOTOR_MIN_MOVE_* values in
//  config.h from the Stage 2 sweeps. Without them the first ~30% of slider
//  travel may do nothing but whine.
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
  Serial.printf("\n[stage4] %s - halting.\n", why);
  while (true) {
    board_led_heartbeat(200);
    delay(10);
  }
}

// Dedicated fixed-rate motor task. It does not share the Arduino loop, because
// the loop also does serial reporting and would let jitter into the slew limiter
// and the failsafe timing.
//
// Priority 6 puts it above the httpd tasks (which default to 5), so streaming a
// heavy frame can never delay a throttle update or postpone a failsafe stop.
// Correct motor timing matters more than a frame of video.
static void motor_task(void *arg) {
  (void)arg;
  const TickType_t period = pdMS_TO_TICKS(1000 / MOTOR_TICK_HZ);
  TickType_t last_wake = xTaskGetTickCount();

  for (;;) {
    motors_tick();
    vTaskDelayUntil(&last_wake, period);
  }
}

void setup() {
  board_begin("Stage 4: video + WebSocket control");
  board_led_begin();

  // Motors first, and before the radio: the tracks must be in a known coasting
  // state before anything can possibly command them.
  motors_begin();

  if (!camera_begin()) halt_blinking("camera init failed, see Stage 1a notes");
  camera_warmup();

  if (!net_begin_ap()) halt_blinking("SoftAP failed to start");

  if (!web_server_begin(/*with_control=*/true)) halt_blinking("web server failed");
  if (!stream_server_begin(STREAM_PORT, /*with_index=*/false))
    halt_blinking("stream server failed");

  // Stack 3072 rather than 2048: the failsafe path calls Serial.printf, and
  // running out of stack inside the motor task would be the worst possible
  // place for it.
  const BaseType_t ok = xTaskCreatePinnedToCore(motor_task, "motor", 3072, nullptr,
                                                /*priority=*/6, nullptr, /*core=*/1);
  if (ok != pdPASS) halt_blinking("could not start the motor task");

  if (MOTOR_MIN_MOVE_L_FWD == 0 && MOTOR_MIN_MOVE_R_FWD == 0) {
    Serial.println(F("[warn] MOTOR_MIN_MOVE_* are all zero. Run the Stage 2 sweeps"));
    Serial.println(F("[warn] and record the four values, or low throttle will just whine."));
  }

  Serial.println();
  Serial.printf("  join \"%s\" then open  http://%s/\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());
  Serial.println();
}

void loop() {
  const bool driving = motors_applied_left() != 0 || motors_applied_right() != 0;
  board_led_heartbeat(driving ? 150 : (web_server_has_control_client() ? 600 : 1800));

  const uint32_t now = millis();

  // Feed the on-screen FPS readout. 2 Hz is plenty for a HUD and keeps the
  // control socket almost entirely free for throttle frames.
  static uint32_t last_status = 0;
  if (now - last_status >= 500) {
    last_status = now;
    web_server_push_status(stream_server_fps());
  }

  static uint32_t last_log = 0;
  if (now - last_log >= 1000) {
    last_log = now;
    Serial.printf("L %+5d>%+5d  R %+5d>%+5d | %s | %5.1f fps | rssi %d | heap %u KB\n",
                  motors_target_left(), motors_applied_left(), motors_target_right(),
                  motors_applied_right(),
                  motors_failsafe_active() ? "FAILSAFE"
                                           : (web_server_has_control_client() ? "linked  "
                                                                              : "no pilot"),
                  stream_server_fps(), WiFi.RSSI(), (unsigned)(ESP.getFreeHeap() / 1024));
  }

  delay(10);
}
