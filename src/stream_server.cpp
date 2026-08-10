#include "stream_server.h"

#include <Arduino.h>
#include <esp_camera.h>
#include <esp_http_server.h>

#include "camera.h"
#include "config.h"

// Arbitrary but must not appear in the JPEG payload. A long digit run is the
// conventional choice for exactly that reason.
#define PART_BOUNDARY "123456789000000000000987654321"

static const char *kStreamContentType = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *kBoundary = "\r\n--" PART_BOUNDARY "\r\n";
static const char *kPartHeader = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static httpd_handle_t s_httpd = nullptr;
static volatile bool s_has_client = false;
static volatile float s_fps = 0.0f;

// Minimal viewer for bench work. The real mobile UI lives in web_page.h.
static const char kIndexHtml[] PROGMEM =
    "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>TankCam stream</title>"
    "<style>body{margin:0;background:#111;display:grid;place-items:center;min-height:100vh}"
    "img{max-width:100%;height:auto;image-rendering:auto}</style></head>"
    "<body><img src='/stream'></body></html>";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t res = httpd_resp_set_type(req, kStreamContentType);
  if (res != ESP_OK) return res;

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  // The stream is served from :81 while the page comes from :80, so without CORS
  // the <img> still renders but fetch()-based diagnostics would be blocked.
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Framerate", "60");

  s_has_client = true;

  char part[72];
  uint32_t window_start = millis();
  uint32_t window_frames = 0;
  int consecutive_nulls = 0;

  // A handful of retries rides out a transient miss; past that the camera is
  // genuinely wedged and holding the connection open would leave the pilot
  // staring at a frozen frame. Dropping it instead lets the page's onerror
  // handler reconnect, which is a recovery path rather than a dead end.
  const int kMaxConsecutiveNulls = 20;  // ~100 ms

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == nullptr) {
      if (++consecutive_nulls > kMaxConsecutiveNulls) {
        Serial.println(F("[stream] camera stopped producing frames, dropping client"));
        res = ESP_FAIL;
        break;
      }
      delay(5);
      continue;
    }
    consecutive_nulls = 0;

    // Standards-correct multipart ordering: boundary, then part headers, then
    // the payload. Some examples emit the first part with no leading boundary
    // and browsers tolerate it, but there is no reason to rely on that.
    const size_t hlen = snprintf(part, sizeof(part), kPartHeader, (unsigned)fb->len);

    res = httpd_resp_send_chunk(req, kBoundary, strlen(kBoundary));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);

    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;  // client closed the tab, or walked out of range

    window_frames++;
    const uint32_t now = millis();
    if (now - window_start >= 1000) {
      s_fps = (window_frames * 1000.0f) / (now - window_start);
      window_frames = 0;
      window_start = now;
    }
  }

  s_has_client = false;
  s_fps = 0.0f;
  return res;
}

bool stream_server_begin(uint16_t port, bool with_index) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;

  // Every httpd instance needs its own control socket. They all default to
  // 32768, so a second instance silently fails to start unless this is bumped.
  // Deriving it from the port keeps the two instances collision-free by
  // construction.
  config.ctrl_port = 32768 + (port - 80);

  config.max_open_sockets = 3;
  config.max_uri_handlers = 4;
  config.lru_purge_enable = true;  // evict a stale stream rather than refusing a new one

  if (httpd_start(&s_httpd, &config) != ESP_OK) {
    Serial.printf("[stream] httpd_start failed on port %u\n", (unsigned)port);
    return false;
  }

  const httpd_uri_t stream_uri = {
      .uri = "/stream",
      .method = HTTP_GET,
      .handler = stream_handler,
      .user_ctx = nullptr,
  };
  httpd_register_uri_handler(s_httpd, &stream_uri);

  if (with_index) {
    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(s_httpd, &index_uri);
  }

  Serial.printf("[stream] MJPEG on port %u at /stream\n", (unsigned)port);
  return true;
}

void stream_server_stop() {
  if (s_httpd != nullptr) {
    httpd_stop(s_httpd);
    s_httpd = nullptr;
  }
  s_has_client = false;
  s_fps = 0.0f;
}

float stream_server_fps() { return s_fps; }

bool stream_server_has_client() { return s_has_client; }
