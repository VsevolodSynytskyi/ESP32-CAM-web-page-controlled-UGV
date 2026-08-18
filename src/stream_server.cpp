#include "stream_server.h"

#include <Arduino.h>
#include <esp_camera.h>
#include <esp_http_server.h>

#include "config.h"

// Arbitrary, but must not appear in the JPEG payload. A long digit run is the
// conventional choice for exactly that reason.
#define PART_BOUNDARY "123456789000000000000987654321"

static const char *kContentType = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *kBoundary = "\r\n--" PART_BOUNDARY "\r\n";

static httpd_handle_t s_video = nullptr;
static volatile bool s_streaming = false;

static esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t res = httpd_resp_set_type(req, kContentType);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  Serial.println(F("[stream] client connected"));
  s_streaming = true;

  uint32_t frames = 0;
  int nulls = 0;
  char part[96];

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

    res = httpd_resp_send_chunk(req, part, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;
    frames++;
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
