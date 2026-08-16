// ===========================================================================
//  ESP32-CAM web page controlled UGV
//
//  CURRENT SCOPE: camera bring-up and streaming.
//
//  BLOCKED: the OV2640 module on this board is faulty. It runs too hot to
//  touch and collapses WiFi throughput whenever the camera subsystem is active
//  - 132 kbps running against 5204 kbps with esp_camera_deinit() called, same
//  board, same client, same minute. Streaming numbers are meaningless until it
//  is replaced. See the README for what was eliminated and how to validate the
//  replacement.
//
//  Sequence, ordered so a fault is never ambiguous:
//
//      1. park the motor driver inputs - the driver is still wired
//      2. bring up the camera and report one frame over serial. If this fails
//         the radio never starts, so a camera fault cannot be mistaken for a
//         network fault.
//      3. join WiFi: station mode if include/secrets.h has credentials,
//         otherwise host a SoftAP
//      4. serve video and a viewer page
//
//  Motors are parked and stay parked. No motor control in this build.
//
//  Endpoints:
//      /          viewer page, switchable between the three transports
//      /ws        WebSocket frame push
//      /jpg       one JPEG per request
//      /stream    multipart MJPEG
//      /speed     1 MB throughput test, no camera data in the payload
//
//  Serial keys:
//      1..5   frame size: QVGA / VGA / SVGA / XGA / SXGA
//      q / Q  JPEG quality worse / better (takes adaptive off)
//      a      toggle adaptive quality
//      c      stop / restart the camera entirely
//      v / h  vertical flip / horizontal mirror
//      p      status
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

// Driver input levels sampled before anything reconfigures them. Kept as a
// permanent guard: if a pin ever changes behaviour - a microSD card in the
// slot, a rewire - this prints *** DRIVE COMMAND *** at startup rather than
// letting you discover it as a runaway motor.
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

// Signal strength of the client as seen by our own access point. Useful
// context, though never the limit here: this board has delivered 5 Mbit/s at
// -55 dBm with the camera stopped.
static int ap_client_rssi() {
  wifi_sta_list_t list;
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK || list.num == 0) return 0;
  return list.sta[0].rssi;
}

static void print_status() {
  uint32_t grab_ms = 0, send_ms = 0, avg_bytes = 0, gap_ms = 0;
  stream_server_timing(&grab_ms, &send_ms, &avg_bytes, &gap_ms);

  // The three timings blame three different things: grab the camera, send the
  // radio, gap the client or a server refusing connections.
  const int rssi = g_ap_mode ? ap_client_rssi() : WiFi.RSSI();
  const float kbps = stream_server_fps() * avg_bytes * 8.0f / 1000.0f;

  Serial.printf("[status] %5.1f fps | grab %3lu send %4lu GAP %5lu ms | %5lu B/f q%-2d | %5.0f kbps "
                "| rssi %d | %s\n",
                stream_server_fps(), grab_ms, send_ms, gap_ms, avg_bytes,
                stream_server_quality(), kbps, rssi,
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

      // Stop the camera outright - sensor, XCLK and DMA - so /speed measures the
      // radio alone. This is the test that isolated the faulty module, and it is
      // how you validate a replacement.
      case 'c':
        if (camera_is_running()) {
          camera_end();
          Serial.println(F("\n*** camera STOPPED *** /speed now measures the radio alone"));
        } else {
          camera_begin();
          camera_warmup();
          Serial.println(F("\n*** camera restarted ***"));
        }
        break;

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
  // Sample the reset-default levels, then park the driver. The motor wiring is
  // untouched by this build, but the driver is still connected and still awake,
  // so it is held in a known state rather than left to its reset defaults.
  g_boot_ain1 = digitalRead(PIN_AIN1);
  g_boot_ain2 = digitalRead(PIN_AIN2);
  g_boot_bin1 = digitalRead(PIN_BIN1);
  g_boot_bin2 = digitalRead(PIN_BIN2);
  motors_begin();

  board_begin("camera + streaming");
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

  Serial.println();
  if (net_begin_sta()) {
    g_ap_mode = false;
  } else {
    Serial.println(F("[net] falling back to SoftAP"));
    if (!net_begin_ap()) halt_blinking("no network");
    g_ap_mode = true;
  }

  if (!stream_server_begin(80, /*with_index=*/true)) halt_blinking("stream server failed");

  Serial.println();
  Serial.printf("  open  %s  in a browser\n", stream_url().c_str());
  if (g_ap_mode) Serial.printf("  (join the \"%s\" network first)\n", AP_SSID);
  Serial.println(F("  also /speed for a 1 MB throughput test"));
  Serial.println(F("  keys: 1-5 size | q/Q quality | a adaptive | c camera | v/h flip | p status"));
  Serial.println();
}

void loop() {
  poll_serial();

  board_led_heartbeat(stream_server_has_client() ? 250 : 1500);

  static uint32_t last = 0;
  if (millis() - last >= 2000) {
    last = millis();
    print_status();
  }

  delay(10);
}
