#include "web_server.h"

#include <Arduino.h>
#include <esp_http_server.h>
#include <lwip/sockets.h>

#include "config.h"
#include "motors.h"
#include "web_page.h"

static httpd_handle_t s_httpd = nullptr;
static volatile int s_ctrl_fd = -1;
static bool s_control_enabled = false;

// ---------------------------------------------------------------------------
//  GET /  - the control page
// ---------------------------------------------------------------------------
static esp_err_t page_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  // Inject the runtime flags ahead of the markup so the page's own script,
  // which runs after the body, already sees them. Cheaper than templating the
  // HTML, and it keeps web_page.h a single verbatim string.
  char cfg[96];
  const int n = snprintf(cfg, sizeof(cfg),
                         "<script>window.TANK_CONTROL=%d;window.TANK_STREAM_PORT=%d;</script>",
                         s_control_enabled ? 1 : 0, STREAM_PORT);

  esp_err_t res = httpd_resp_send_chunk(req, cfg, n);
  if (res == ESP_OK) {
    res = httpd_resp_send_chunk(req, kControlPageHtml, strlen(kControlPageHtml));
  }
  if (res == ESP_OK) {
    res = httpd_resp_send_chunk(req, nullptr, 0);  // terminate the chunked response
  }
  return res;
}

// ---------------------------------------------------------------------------
//  GET /ws  - WebSocket control channel
// ---------------------------------------------------------------------------
static esp_err_t ws_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    // Called once, immediately after a successful handshake.
    const int fd = httpd_req_to_sockfd(req);
    s_ctrl_fd = fd;

    // Without TCP_NODELAY, Nagle's algorithm holds our 2-byte throttle frames
    // back waiting for more data to coalesce. That adds tens of milliseconds to
    // every steering input - the single worst thing you can do to this control
    // loop, and invisible unless you go looking for it.
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    Serial.printf("[ws] control client connected (fd %d)\n", fd);
    return ESP_OK;
  }

  uint8_t buf[32];
  httpd_ws_frame_t pkt;
  memset(&pkt, 0, sizeof(pkt));

  // max_len 0 asks only for the length, so we can reject an oversized frame
  // before committing a buffer to it.
  esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
  if (ret != ESP_OK) return ret;

  if (pkt.len == 0) return ESP_OK;
  if (pkt.len > sizeof(buf)) {
    // Nothing in this protocol is longer than two bytes; anything bigger is a
    // protocol violation. Returning an error closes the socket.
    Serial.printf("[ws] oversized frame (%u bytes), dropping client\n", (unsigned)pkt.len);
    return ESP_FAIL;
  }

  pkt.payload = buf;
  ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
  if (ret != ESP_OK) return ret;

  if (pkt.type == HTTPD_WS_TYPE_BINARY && pkt.len >= 2) {
    // Signed int8 per track, -100..+100. The sign is the direction; there is no
    // separate direction field anywhere in the chain.
    const int8_t l8 = (int8_t)buf[0];
    const int8_t r8 = (int8_t)buf[1];

    const int16_t l = (int16_t)(((int32_t)l8 * MOTOR_SCALE) / 100);
    const int16_t r = (int16_t)(((int32_t)r8 * MOTOR_SCALE) / 100);

    // Only updates the targets and refreshes the failsafe timer. The motor task
    // is the sole writer to the PWM registers.
    motors_set(l, r);
  }

  return ESP_OK;
}

// ---------------------------------------------------------------------------
//  Session teardown
// ---------------------------------------------------------------------------
static void on_session_close(httpd_handle_t hd, int sockfd) {
  (void)hd;

  if (sockfd == s_ctrl_fd) {
    s_ctrl_fd = -1;
    // Stop the instant the socket dies rather than waiting out CMD_TIMEOUT_MS.
    // The deadman still covers the cases this cannot see - a phone that goes to
    // sleep mid-drive often leaves the socket looking open for a while.
    motors_stop(false);
    Serial.println(F("[ws] control client gone - motors stopped"));
  }

  // Deliberately no close() here: esp_http_server closes the socket itself
  // after this callback returns, and by now the descriptor may already be
  // invalid if the network stack tore it down.
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------
bool web_server_begin(bool with_control) {
  s_control_enabled = with_control;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = HTTP_PORT;

  // Each instance needs its own control socket; they all default to 32768, so
  // the second instance to start would silently fail. stream_server.cpp derives
  // its port the same way, which keeps the two collision-free by construction.
  config.ctrl_port = 32768 + (HTTP_PORT - 80);

  config.max_open_sockets = 4;
  config.max_uri_handlers = 6;
  config.lru_purge_enable = true;
  config.close_fn = on_session_close;

  if (httpd_start(&s_httpd, &config) != ESP_OK) {
    Serial.printf("[web] httpd_start failed on port %d\n", HTTP_PORT);
    return false;
  }

  const httpd_uri_t page_uri = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = page_handler,
      .user_ctx = nullptr,
      .is_websocket = false,
      .handle_ws_control_frames = false,
      .supported_subprotocol = nullptr,
  };
  httpd_register_uri_handler(s_httpd, &page_uri);

  if (with_control) {
    const httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = nullptr,
        .is_websocket = true,
        // Let the server answer PING and CLOSE itself. We learn about the
        // disconnect through close_fn, which also covers sockets the network
        // stack kills without a clean close.
        .handle_ws_control_frames = false,
        .supported_subprotocol = nullptr,
    };
    httpd_register_uri_handler(s_httpd, &ws_uri);
  }

  Serial.printf("[web] UI on port %d%s\n", HTTP_PORT,
                with_control ? ", control channel at /ws" : " (video only, no control)");
  return true;
}

void web_server_stop() {
  if (s_httpd != nullptr) {
    httpd_stop(s_httpd);
    s_httpd = nullptr;
  }
  s_ctrl_fd = -1;
}

bool web_server_has_control_client() { return s_ctrl_fd >= 0; }

void web_server_push_status(float fps) {
  const int fd = s_ctrl_fd;
  if (s_httpd == nullptr || fd < 0) return;

  char msg[48];
  const int n = snprintf(msg, sizeof(msg), "fps=%.1f", fps);

  httpd_ws_frame_t frame;
  memset(&frame, 0, sizeof(frame));
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = (uint8_t *)msg;
  frame.len = n;

  httpd_ws_send_frame_async(s_httpd, fd, &frame);
}
