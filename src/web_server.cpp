#include "web_server.h"

#include <Arduino.h>
#include <esp_http_server.h>

#include "config.h"
#include "motors.h"
#include "web_page.h"

static httpd_handle_t s_httpd = nullptr;

static esp_err_t viewer_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, kViewerHtml, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t favicon_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, nullptr, 0);
}

// The wire carries -100..+100 so a throttle fits in one signed byte; motors.h
// works in MOTOR_SCALE units. Sign is direction on both sides of the wire, so
// this is a pure change of units.
static int16_t from_wire(int8_t v) {
  int32_t scaled = ((int32_t)v * MOTOR_SCALE) / 100;
  if (scaled > MOTOR_SCALE) scaled = MOTOR_SCALE;
  if (scaled < -MOTOR_SCALE) scaled = -MOTOR_SCALE;
  return (int16_t)scaled;
}

static esp_err_t control_handler(httpd_req_t *req) {
  // The handshake arrives as a GET with no frame behind it.
  if (req->method == HTTP_GET) {
    Serial.println(F("[ctrl] client connected"));
    return ESP_OK;
  }

  // Length first, payload second - httpd_ws_recv_frame() with max_len 0 only
  // fills in the header. PING, PONG and CLOSE never reach here; the server
  // answers those itself.
  httpd_ws_frame_t frame = {};
  esp_err_t res = httpd_ws_recv_frame(req, &frame, 0);
  if (res != ESP_OK) return res;

  uint8_t buf[2];
  if (frame.len != sizeof(buf)) {
    // Not something our page sends. Returning ESP_FAIL drops the socket rather
    // than leaving a half-read frame desynchronising every command after it.
    Serial.printf("[ctrl] unexpected %u-byte frame, dropping client\n", (unsigned)frame.len);
    return ESP_FAIL;
  }

  frame.payload = buf;
  res = httpd_ws_recv_frame(req, &frame, sizeof(buf));
  if (res != ESP_OK) return res;

  motors_set(from_wire((int8_t)buf[0]), from_wire((int8_t)buf[1]));
  return ESP_OK;
}

bool web_server_begin(uint16_t port) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;
  // Every instance needs its own control socket; they all default to 32768, so
  // a second instance silently fails to start unless this is bumped.
  config.ctrl_port = 32768 + (port - 80);
  config.max_uri_handlers = 4;

  if (httpd_start(&s_httpd, &config) != ESP_OK) {
    Serial.printf("[web] failed to start on port %u\n", (unsigned)port);
    return false;
  }

  const httpd_uri_t viewer_uri = {"/", HTTP_GET, viewer_handler, nullptr};
  httpd_register_uri_handler(s_httpd, &viewer_uri);

  const httpd_uri_t favicon_uri = {"/favicon.ico", HTTP_GET, favicon_handler, nullptr};
  httpd_register_uri_handler(s_httpd, &favicon_uri);

  // is_websocket, then handle_ws_control_frames, then supported_subprotocol.
  const httpd_uri_t control_uri = {"/control", HTTP_GET, control_handler, nullptr,
                                   true,       false,    nullptr};
  httpd_register_uri_handler(s_httpd, &control_uri);

  Serial.printf("[web] page and control on :%u\n", (unsigned)port);
  return true;
}
