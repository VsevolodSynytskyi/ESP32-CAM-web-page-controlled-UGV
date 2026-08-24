#include "stream_server.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>

#include "camera.h"
#include "config.h"
#include "net.h"

// Arbitrary, but must not appear in the JPEG payload. A long digit run is the
// conventional choice for exactly that reason.
#define PART_BOUNDARY "123456789000000000000987654321"

static const char *kContentType = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *kBoundary = "\r\n--" PART_BOUNDARY "\r\n";

#include <stdio.h>
#include <string.h>

static httpd_handle_t s_video = nullptr;
static volatile bool s_streaming = false;

// Last completed window, published for stream_server_stats(). Held as scaled
// integers rather than a shared string: the stream task writes these while the
// control socket's task reads them, and a 32-bit aligned store cannot tear the
// way a half-written buffer can.
static volatile uint32_t s_fps10 = 0;   // frames per second x10
static volatile uint32_t s_kb10 = 0;    // kB per frame x10
static volatile uint32_t s_kbps = 0;    // kB per second
static volatile uint32_t s_busy = 0;    // percent of wall clock blocked in send
static volatile int32_t s_rssi = 0;     // dBm

// Session tally, reset per client. The outages this counts last a second or two
// and arrive every ten or twenty, so a single window tells you nothing - you
// have to watch for a minute. Counting them here means the answer is one number
// on screen at the end, readable on a phone with no console attached.
static volatile uint32_t s_windows = 0;
static volatile uint32_t s_stalls = 0;
static volatile uint32_t s_best10 = 0;   // best fps x10 this session
static volatile uint32_t s_worst10 = 0;  // worst fps x10 this session

// Signal strength to whoever is watching, in dBm. SoftAP keeps it per station;
// station mode keeps it on the WiFi object. 0 means nobody is associated.
static int link_rssi() {
  wifi_sta_list_t list;
  if (esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num > 0) return list.sta[0].rssi;
  return WiFi.RSSI();
}

static esp_err_t stream_handler(httpd_req_t *req) {
  // Without this the page's 1 s reconnect would hammer a handler that can only
  // ever time out on null frames.
  if (!camera_ready()) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "no camera", HTTPD_RESP_USE_STRLEN);
  }

  esp_err_t res = httpd_resp_set_type(req, kContentType);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  // Nagle holds a small write back until the peer acknowledges the last one,
  // and chunked encoding puts a length line in front of every payload - which
  // is precisely the pattern it punishes, at a round trip per frame. Read the
  // old value before overwriting it: the SDK documents no default either way,
  // so this prints what was actually there rather than what we assume.
  const int fd = httpd_req_to_sockfd(req);
  int nodelay_was = -1;
  socklen_t optlen = sizeof(nodelay_was);
  getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay_was, &optlen);
  const int on = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

  Serial.printf("[stream] client connected (TCP_NODELAY was %d, now 1)\n", nodelay_was);
  s_streaming = true;
  s_windows = s_stalls = s_best10 = 0;
  s_worst10 = 0xFFFFFFFF;

  uint32_t frames = 0;
  int nulls = 0;
  char part[96];

  // Rolling window, reported every STREAM_STATS_MS. send_us is the one that
  // decides things: it is time this task spent blocked inside the stack, so as
  // a share of wall clock it says how close the link is to saturated. A
  // saturated link is where video latency comes from, because everything newer
  // waits behind whatever is already queued.
  uint32_t win_start = millis();
  uint32_t win_frames = 0, win_bytes = 0, win_send_us = 0, win_peak_us = 0;

  for (;;) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == nullptr) {
      // A few retries ride out a transient miss. Past that the sensor is wedged
      // and holding the connection open just leaves a frozen picture on screen,
      // so drop the client and let the page reconnect.
      if (++nulls > 20) {
        Serial.println(F("[stream] camera stopped producing frames"));
        res = ESP_FAIL;
        break;
      }
      delay(5);
      continue;
    }
    nulls = 0;

    // Boundary and part header go out as one write. Every extra small send is
    // another chance to stall waiting for the peer's ACK, and they are adjacent
    // bytes anyway.
    const size_t hlen =
        snprintf(part, sizeof(part), "%sContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                 kBoundary, (unsigned)fb->len);

    const size_t flen = fb->len;  // read it while the buffer is still ours

    const uint32_t t0 = micros();
    res = httpd_resp_send_chunk(req, part, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, flen);
    const uint32_t send_us = micros() - t0;
    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;
    frames++;

    win_frames++;
    win_bytes += hlen + flen;
    win_send_us += send_us;
    if (send_us > win_peak_us) win_peak_us = send_us;

    const uint32_t elapsed = millis() - win_start;
    if (elapsed >= STREAM_STATS_MS) {
      const int rssi = link_rssi();
      Serial.printf(
          "[stream] %.1f fps  %.1f kB/frame  %.0f kB/s  send %lu ms avg, %lu peak"
          "  link %lu%% busy  rssi %d dBm\n",
          win_frames * 1000.0f / elapsed, win_bytes / 1024.0f / win_frames,
          win_bytes / 1.024f / elapsed, (unsigned long)(win_send_us / win_frames / 1000),
          (unsigned long)(win_peak_us / 1000), (unsigned long)(win_send_us / (elapsed * 10)),
          rssi);
      const uint32_t fps10 = win_frames * 10000 / elapsed;
      if (fps10 > s_best10) s_best10 = fps10;
      // The stall count alone cannot tell a mild dip from a dead link, and the
      // difference decides where to look next: a floor near the healthy rate is
      // variance, a floor near zero is the link going away.
      if (fps10 < s_worst10) s_worst10 = fps10;
      s_windows++;
      // Less than half our own best rate. Relative rather than absolute so the
      // count keeps meaning the same thing if CAM_FRAMESIZE changes.
      if (fps10 * 2 < s_best10) s_stalls++;

      s_fps10 = fps10;
      s_kb10 = win_bytes * 10 / 1024 / win_frames;
      s_kbps = (uint32_t)((uint64_t)win_bytes * 1000 / 1024 / elapsed);
      s_busy = win_send_us / (elapsed * 10);
      s_rssi = rssi;

      win_start = millis();
      win_frames = win_bytes = win_send_us = win_peak_us = 0;
    }
  }

  s_streaming = false;
  Serial.printf("[stream] client gone after %lu frame(s)\n", (unsigned long)frames);
  return res;
}

bool stream_server_begin(uint16_t port) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  // Every instance needs its own control socket; they all default to 32768, so
  // a second instance silently fails to start unless this is bumped.
  config.ctrl_port = 32768 + (port - 80);
  config.max_uri_handlers = 2;

  if (httpd_start(&s_video, &config) != ESP_OK) {
    Serial.printf("[stream] failed to start on port %u\n", (unsigned)port);
    return false;
  }

  const httpd_uri_t stream_uri = {"/stream", HTTP_GET, stream_handler, nullptr};
  httpd_register_uri_handler(s_video, &stream_uri);

  Serial.printf("[stream] video on :%u/stream\n", (unsigned)port);
  return true;
}

bool stream_server_has_client() { return s_streaming; }

void stream_server_stats(char *out, size_t len) {
  if (len == 0) return;
  if (!s_streaming || s_fps10 == 0) {
    out[0] = '\0';
    return;
  }
  snprintf(out, len,
           "%lu.%lufps  %lu.%lukB  %lukB/s  busy%lu%%  rssi%ld  ch%d%s  stall%lu/%lu",
           (unsigned long)(s_fps10 / 10), (unsigned long)(s_fps10 % 10),
           (unsigned long)(s_kb10 / 10), (unsigned long)(s_kb10 % 10),
           (unsigned long)s_kbps, (unsigned long)s_busy, (long)s_rssi, net_channel(),
           net_channel_jammed() ? "-XCLKJAM" : "", (unsigned long)s_stalls,
           (unsigned long)s_windows);

  // Appended rather than folded in, so the main line stays the same shape
  // whether or not a session has produced a floor yet.
  if (s_worst10 != 0xFFFFFFFF) {
    const size_t at = strlen(out);
    if (at < len) {
      snprintf(out + at, len - at, "  worst%lu.%lufps", (unsigned long)(s_worst10 / 10),
               (unsigned long)(s_worst10 % 10));
    }
  }
}
