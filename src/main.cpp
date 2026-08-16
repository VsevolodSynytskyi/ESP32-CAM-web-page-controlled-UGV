// ===========================================================================
//  ESP32-CAM web page controlled UGV
//
//  CURRENT SCOPE: camera bring-up and MJPEG streaming.
//
//  Sequence, deliberately in this order so a fault is never ambiguous:
//
//      1. park the motor driver inputs (the driver is still wired)
//      2. bring up the camera and report one frame over serial - if this
//         fails, the radio never starts, so a camera fault can never be
//         mistaken for a network fault
//      3. join WiFi: station mode if include/secrets.h has credentials,
//         otherwise fall back to hosting a SoftAP
//      4. serve MJPEG at  /stream  with a bare viewer page at  /
//
//  Motors are parked and stay parked this whole time. No motor control here.
//
//  Serial keys, for tuning the camera once it is mounted:
//      1..5   frame size: QVGA / VGA / SVGA / XGA / SXGA
//      q / Q  JPEG quality worse / better
//      v      toggle vertical flip
//      h      toggle horizontal mirror
//      p      print status
// ===========================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "board.h"
#include "camera.h"
#include "config.h"
#include "motors.h"
#include "net.h"
#include "stream_server.h"

// Levels sampled off the driver inputs before anything reconfigures them.
// Kept as a permanent guard: if a pin ever changes behaviour - a microSD card
// in the slot, a rewire - this prints *** DRIVE COMMAND *** at startup rather
// than letting you discover it as a runaway motor.
static int g_boot_ain1, g_boot_ain2, g_boot_bin1, g_boot_bin2;

static bool g_ap_mode = false;
static int g_quality = CAM_JPEG_QUALITY;
static bool g_vflip = CAM_VFLIP != 0;
static bool g_hmirror = CAM_HMIRROR != 0;

static const char *pair_verdict(int in1, int in2) {
  if (in1 == in2) return in1 ? "(H,H) short brake - safe" : "(L,L) coast - safe";
  return "(H,L) or (L,H)  *** DRIVE COMMAND - motor runs during boot ***";
}

static void halt_blinking(const char *why) {
  Serial.printf("\n[fatal] %s - halting.\n", why);
  while (true) {
    board_led_heartbeat(200);
    delay(10);
  }
}

static String stream_url() {
  const IPAddress ip = g_ap_mode ? WiFi.softAPIP() : WiFi.localIP();
  return "http://" + ip.toString() + "/";
}

static void report_first_frame() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb == nullptr) {
    Serial.println(F("[cam] init succeeded but the first capture returned null"));
    return;
  }

  Serial.println();
  Serial.println(F("--- first frame ---"));
  Serial.printf("  size    : %u bytes\n", (unsigned)fb->len);
  Serial.printf("  dims    : %u x %u\n", (unsigned)fb->width, (unsigned)fb->height);
  Serial.printf("  format  : %s\n", fb->format == PIXFORMAT_JPEG ? "JPEG" : "NOT JPEG");
  Serial.printf("  markers : %s\n", camera_frame_looks_valid(fb)
                                        ? "SOI/EOI present, frame intact"
                                        : "INVALID - try lowering CAM_XCLK_HZ");
  Serial.println(F("-------------------"));
  esp_camera_fb_return(fb);
}

// Signal strength of the phone as seen by our own access point. This is the
// number that decides whether a slow stream is a radio problem: WiFi drops to
// its lowest PHY rates as RSSI falls, and at the bottom end throughput
// collapses far faster than the signal bars suggest.
//
//   > -50 dBm  excellent      -70 dBm  usable
//     -60 dBm  good         < -80 dBm  expect a slideshow
static int ap_client_rssi() {
  wifi_sta_list_t list;
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK || list.num == 0) return 0;
  return list.sta[0].rssi;
}

static void print_status() {
  uint32_t grab_ms = 0, send_ms = 0, avg_bytes = 0, gap_ms = 0;
  stream_server_timing(&grab_ms, &send_ms, &avg_bytes, &gap_ms);

  // grab vs send is the whole diagnosis when fps is low. A big grab_ms means
  // the sensor or the JPEG encoder is the bottleneck; a big send_ms means the
  // radio is. They need completely different fixes.
  const int rssi = g_ap_mode ? ap_client_rssi() : WiFi.RSSI();
  const float kbps = stream_server_fps() * avg_bytes * 8.0f / 1000.0f;

  Serial.printf("[status] %5.1f fps | grab %3lu send %4lu GAP %5lu ms | %5lu B/f q%-2d | %5.0f kbps "
                "| rssi %d | %s\n",
                stream_server_fps(), grab_ms, send_ms, gap_ms, avg_bytes, stream_server_quality(),
                kbps, rssi,
                stream_server_has_client() ? "streaming" : "idle");
}

static void poll_serial() {
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    switch (c) {
      case '1': camera_set_framesize(FRAMESIZE_QVGA); Serial.println(F("QVGA 320x240")); break;
      case '2': camera_set_framesize(FRAMESIZE_VGA);  Serial.println(F("VGA 640x480")); break;
      case '3': camera_set_framesize(FRAMESIZE_SVGA); Serial.println(F("SVGA 800x600")); break;
      case '4': camera_set_framesize(FRAMESIZE_XGA);  Serial.println(F("XGA 1024x768")); break;
      case '5': camera_set_framesize(FRAMESIZE_SXGA); Serial.println(F("SXGA 1280x1024")); break;

      // Manual quality takes over from the adaptive controller, otherwise the
      // two fight and the setting silently springs back.
      case 'q':
        stream_server_set_adaptive(false);
        g_quality += 3;
        camera_set_quality(g_quality);
        Serial.printf("quality %d (higher = worse, smaller), adaptive OFF\n", g_quality);
        break;
      case 'Q':
        stream_server_set_adaptive(false);
        g_quality -= 3;
        camera_set_quality(g_quality);
        Serial.printf("quality %d (lower = better, bigger), adaptive OFF\n", g_quality);
        break;
      case 'a':
        stream_server_set_adaptive(!stream_server_adaptive());
        Serial.printf("adaptive quality %s\n", stream_server_adaptive() ? "ON" : "OFF");
        break;

      // Cycle WiFi transmit power. This is the supply test, and the logic is
      // backwards from intuition: at RSSI -17 dBm there is ~60 dB of margin, so
      // turning the radio DOWN cannot hurt the link - but it substantially cuts
      // the current spike on every transmit. If a weak supply is browning out
      // the PA mid-packet, less power means fewer lost packets means FASTER
      // throughput. An improvement here convicts the power supply; no change
      // exonerates it.
      case 't': {
        static const wifi_power_t levels[] = {WIFI_POWER_19_5dBm, WIFI_POWER_15dBm,
                                              WIFI_POWER_11dBm, WIFI_POWER_8_5dBm,
                                              WIFI_POWER_5dBm};
        static const char *names[] = {"19.5", "15", "11", "8.5", "5"};
        static int idx = 0;
        idx = (idx + 1) % 5;
        WiFi.setTxPower(levels[idx]);
        Serial.printf("\n*** tx power %s dBm *** watch the send time\n", names[idx]);
        break;
      }

      case 'v':
        g_vflip = !g_vflip;
        camera_set_vflip(g_vflip);
        Serial.printf("vflip %s\n", g_vflip ? "on" : "off");
        break;
      case 'h':
        g_hmirror = !g_hmirror;
        camera_set_hmirror(g_hmirror);
        Serial.printf("hmirror %s\n", g_hmirror ? "on" : "off");
        break;

      case 'p': print_status(); break;
      default: break;
    }
  }
}

void setup() {
  // Sample the reset-default levels first, then park the driver. The motor
  // wiring is untouched by this stage, but the driver is still connected and
  // still awake, so it stays parked rather than left to its reset defaults.
  g_boot_ain1 = digitalRead(PIN_AIN1);
  g_boot_ain2 = digitalRead(PIN_AIN2);
  g_boot_bin1 = digitalRead(PIN_BIN1);
  g_boot_bin2 = digitalRead(PIN_BIN2);
  motors_begin();

  board_begin("camera + MJPEG stream");
  board_led_begin();

  Serial.println(F("--- motor pins at reset ---"));
  Serial.printf("  A: GPIO%-2d=%d GPIO%-2d=%d  %s\n", PIN_AIN1, g_boot_ain1, PIN_AIN2, g_boot_ain2,
                pair_verdict(g_boot_ain1, g_boot_ain2));
  Serial.printf("  B: GPIO%-2d=%d GPIO%-2d=%d  %s\n", PIN_BIN1, g_boot_bin1, PIN_BIN2, g_boot_bin2,
                pair_verdict(g_boot_bin1, g_boot_bin2));
  Serial.println();

  // Camera before the radio. If the sensor is faulty we stop here, so the error
  // is unambiguous instead of tangled up with a WiFi problem.
  Serial.printf("[cam] xclk %d Hz, quality %d, %d framebuffer(s)\n", CAM_XCLK_HZ,
                CAM_JPEG_QUALITY, CAM_FB_COUNT);
  if (!camera_begin()) {
    Serial.println(F("[cam] Check the ribbon cable is fully seated and the latch"));
    Serial.println(F("[cam] closed, and that the 5V rail is solid - the OV2640"));
    Serial.println(F("[cam] browns out more easily than the ESP32 does."));
    halt_blinking("camera init failed");
  }
  camera_warmup();
  report_first_frame();

  // Station mode if credentials exist, SoftAP otherwise. Station is easier on
  // the bench - the laptop keeps its internet connection and DevTools - while
  // the AP needs no configuration at all and is what the vehicle will use in
  // the field.
  Serial.println();
  if (net_begin_sta()) {
    g_ap_mode = false;
  } else {
    Serial.println(F("[net] falling back to SoftAP"));
    if (!net_begin_ap()) halt_blinking("no network");
    g_ap_mode = true;
  }

  // One server for now: the viewer page and the stream share port 80, which is
  // fine while nothing else needs to answer. When the control channel arrives
  // the stream moves to its own instance on STREAM_PORT, because an MJPEG
  // response never ends and would otherwise occupy the only worker.
  if (!stream_server_begin(80, /*with_index=*/true)) halt_blinking("stream server failed");

  Serial.println();
  Serial.printf("  open  %s  in a browser\n", stream_url().c_str());
  if (g_ap_mode) {
    Serial.printf("  (join the \"%s\" network first)\n", AP_SSID);
  }
  Serial.println(F("  keys: 1-5 size | q/Q quality | a adaptive(off) | t TX power | v/h flip | p status"));
  Serial.println();
}

void loop() {
  poll_serial();

  // Fast blink while a client is pulling frames.
  board_led_heartbeat(stream_server_has_client() ? 250 : 1500);

  // Print unconditionally. Only printing while a client is attached hides the
  // connect/disconnect churn, which is exactly what you need to see when a
  // stream is failing and the browser is silently retrying.
  static uint32_t last = 0;
  if (millis() - last >= 2000) {
    last = millis();
    print_status();
  }

  delay(10);
}
