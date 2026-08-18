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

static httpd_handle_t s_pages = nullptr;
static httpd_handle_t s_video = nullptr;
static uint16_t s_video_port = 81;
static volatile bool s_streaming = false;

static const char kViewerHtml[] = R"HTML(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TankCam</title><style>
html,body{margin:0;height:100%;background:#000;overflow:hidden}
img{position:fixed;inset:0;width:100%;height:100%;object-fit:contain}
</style></head><body>
<img id="v" alt="">
<script>
// Video is on the next port up - see the two-instance note in stream_server.h.
var v = document.getElementById('v');
var url = 'http://' + location.hostname + ':' + (Number(location.port || 80) + 1) + '/stream';
function start() { v.src = url + '?n=' + Date.now(); }
v.onerror = function () { setTimeout(start, 1000); };
start();
</script></body></html>)HTML";

static esp_err_t viewer_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, kViewerHtml, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t favicon_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, nullptr, 0);
}

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

bool stream_server_begin(uint16_t page_port) {
  httpd_config_t pconfig = HTTPD_DEFAULT_CONFIG();
  pconfig.server_port = page_port;
  pconfig.ctrl_port = 32768 + (page_port - 80);
  pconfig.max_uri_handlers = 4;

  if (httpd_start(&s_pages, &pconfig) != ESP_OK) {
    Serial.printf("[stream] page server failed on port %u\n", (unsigned)page_port);
    return false;
  }

  const httpd_uri_t viewer_uri = {"/", HTTP_GET, viewer_handler, nullptr};
  httpd_register_uri_handler(s_pages, &viewer_uri);
  const httpd_uri_t favicon_uri = {"/favicon.ico", HTTP_GET, favicon_handler, nullptr};
  httpd_register_uri_handler(s_pages, &favicon_uri);

  // Every httpd instance needs its own control socket; they all default to
  // 32768, so a second instance silently fails to start unless this is bumped.
  s_video_port = page_port + 1;
  httpd_config_t vconfig = HTTPD_DEFAULT_CONFIG();
  vconfig.server_port = s_video_port;
  vconfig.ctrl_port = 32768 + (s_video_port - 80);
  vconfig.max_uri_handlers = 2;

  if (httpd_start(&s_video, &vconfig) != ESP_OK) {
    Serial.printf("[stream] video server failed on port %u\n", (unsigned)s_video_port);
    return false;
  }

  const httpd_uri_t stream_uri = {"/stream", HTTP_GET, stream_handler, nullptr};
  httpd_register_uri_handler(s_video, &stream_uri);

  Serial.printf("[stream] page on :%u, video on :%u/stream\n", (unsigned)page_port,
                (unsigned)s_video_port);
  return true;
}

bool stream_server_has_client() { return s_streaming; }
