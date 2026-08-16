#include "stream_server.h"

#include <Arduino.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <lwip/sockets.h>

#include "camera.h"
#include "config.h"

// ===========================================================================
//  Two video transports, deliberately:
//
//    GET /stream   multipart/x-mixed-replace MJPEG. One response that never
//                  ends. Efficient - no per-frame HTTP overhead - and works in
//                  Chrome and Firefox.
//
//    GET /jpg      one JPEG per request. The client asks for the next frame as
//                  soon as the current one decodes. Costs a request per frame,
//                  but it is plain HTTP that every browser handles, including
//                  iOS Safari, which is unreliable with x-mixed-replace and is
//                  the only engine available on iOS.
//
//  The viewer page can switch between them at runtime so both can be compared
//  on the same device against the same server-side counters.
// ===========================================================================

// Arbitrary but must not appear in the JPEG payload. A long digit run is the
// conventional choice for exactly that reason.
#define PART_BOUNDARY "123456789000000000000987654321"

static const char *kStreamContentType = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *kBoundary = "\r\n--" PART_BOUNDARY "\r\n";

static httpd_handle_t s_httpd = nullptr;

// Socket of the connected WebSocket video client, or -1. Written by the httpd
// task on connect/disconnect, read by the push task.
static volatile int s_ws_fd = -1;

// ---------------------------------------------------------------------------
//  Shared frame statistics
//
//  Both transports feed the same counters, so switching transport changes only
//  one variable in the comparison. Splitting grab time from send time is the
//  whole diagnosis when the stream is slow: the camera and the radio need
//  completely different fixes.
// ---------------------------------------------------------------------------
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile float s_fps = 0.0f;
static volatile uint32_t s_grab_ms = 0;
static volatile uint32_t s_send_ms = 0;
static volatile uint32_t s_avg_bytes = 0;
static volatile uint32_t s_last_frame_ms = 0;

// Dead time between finishing one frame and being asked for the next. If grab
// and send are both small but fps is low, this is where the missing time is,
// and it points at the client or at the server refusing connections - never at
// the camera or the radio.
static volatile uint32_t s_gap_ms = 0;

static uint32_t w_start = 0, w_frames = 0, w_grab = 0, w_send = 0, w_bytes = 0, w_gap = 0;

static void note_frame(uint32_t grab_ms, uint32_t send_ms, size_t bytes) {
  const uint32_t now = millis();

  portENTER_CRITICAL(&s_stats_mux);
  // Gap since the previous frame finished. Ignore the first frame and any gap
  // long enough to mean the viewer simply went away.
  const uint32_t since = (s_last_frame_ms == 0) ? 0 : (now - s_last_frame_ms);
  w_gap += (since > 3000) ? 0 : since;

  w_frames++;
  w_grab += grab_ms;
  w_send += send_ms;
  w_bytes += bytes;
  s_last_frame_ms = now;

  if (now - w_start >= 1000) {
    s_fps = (w_frames * 1000.0f) / (now - w_start);
    s_grab_ms = w_grab / w_frames;
    s_send_ms = w_send / w_frames;
    s_avg_bytes = w_bytes / w_frames;
    s_gap_ms = w_gap / w_frames;
    w_frames = w_grab = w_send = w_bytes = w_gap = 0;
    w_start = now;
  }
  portEXIT_CRITICAL(&s_stats_mux);
}

// Nagle holds small writes back waiting for the previous ACK. Against the
// peer's delayed-ACK timer that costs hundreds of milliseconds per frame, which
// on a video stream is the difference between fluid and a slideshow.
static void set_nodelay(httpd_req_t *req) {
  // Deliberately a no-op for now. The reference sets no socket options at all
  // and reaches 15 fps, so this is one more difference to eliminate before
  // blaming anything. Re-enable only with a before/after measurement.
  (void)req;
}

// ---------------------------------------------------------------------------
//  Adaptive JPEG quality
//
//  The Arduino framework's prebuilt lwip fixes CONFIG_LWIP_TCP_SND_BUF_DEFAULT
//  at 5760 bytes. A frame that fits inside that goes out and waits for one ACK;
//  a frame twice that size costs two full window round trips, which on a link
//  where RTT dominates roughly halves the frame rate.
//
//  So frame SIZE matters more than frame quality here, and JPEG size swings
//  wildly with scene detail - the same camera produced 5 kB frames pointed at a
//  wall and 9 kB pointed at a cluttered desk. A fixed quality setting cannot
//  hold the budget; this walks quality to keep frames under the window.
// ---------------------------------------------------------------------------
#define TARGET_FRAME_BYTES 4000  // comfortably inside 5760 with headers
#define QUALITY_BEST 10
#define QUALITY_WORST 45

static bool s_adaptive = false;
static int s_quality = CAM_JPEG_QUALITY;

static void adapt_quality(size_t last_bytes) {
  if (!s_adaptive) return;

  // Adjust at most twice a second: JPEG size responds a frame or two later, and
  // chasing every frame just oscillates.
  static uint32_t last_adjust = 0;
  const uint32_t now = millis();
  if (now - last_adjust < 500) return;
  last_adjust = now;

  if (last_bytes > (size_t)(TARGET_FRAME_BYTES * 1.15f) && s_quality < QUALITY_WORST) {
    s_quality += 2;  // over budget: shed bytes quickly
    camera_set_quality(s_quality);
  } else if (last_bytes < (size_t)(TARGET_FRAME_BYTES * 0.70f) && s_quality > QUALITY_BEST) {
    s_quality -= 1;  // room to spare: recover detail gently
    camera_set_quality(s_quality);
  }
}

void stream_server_set_adaptive(bool on) { s_adaptive = on; }
bool stream_server_adaptive() { return s_adaptive; }
int stream_server_quality() { return s_quality; }

// ---------------------------------------------------------------------------
//  /ws  - WebSocket video push
//
//  The reason this exists: the Arduino framework's prebuilt lwip fixes
//  CONFIG_LWIP_TCP_SND_BUF_DEFAULT at 5760 bytes, which is about one JPEG
//  frame. Every send therefore blocks until the peer ACKs, so the frame rate is
//  set by round-trip time rather than by bandwidth.
//
//  Given that, the win is deleting round trips. Poll mode spends a whole one
//  per frame just being asked for the next; pushing over a persistent socket
//  removes it. It also sidesteps iOS Safari's unreliable handling of
//  multipart/x-mixed-replace, since WebSockets it handles properly.
// ---------------------------------------------------------------------------
static esp_err_t ws_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    // Called once, right after a successful handshake.
    s_ws_fd = httpd_req_to_sockfd(req);
    set_nodelay(req);
    Serial.printf("[ws] video client connected (fd %d)\n", s_ws_fd);
    return ESP_OK;
  }

  // The client has nothing to say; drain and discard whatever arrives so the
  // socket does not back up.
  uint8_t buf[32];
  httpd_ws_frame_t pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.payload = buf;
  httpd_ws_recv_frame(req, &pkt, sizeof(buf));
  return ESP_OK;
}

static void video_push_task(void *arg) {
  (void)arg;
  for (;;) {
    const int fd = s_ws_fd;
    if (fd < 0 || s_httpd == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    const uint32_t t0 = millis();
    camera_fb_t *fb = esp_camera_fb_get();
    const uint32_t t1 = millis();
    if (fb == nullptr) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = fb->buf;
    frame.len = fb->len;

    // Blocks while the socket is full, which is exactly the backpressure we
    // want: it paces us to whatever the link and the phone can actually take,
    // instead of queueing frames nobody is reading.
    const esp_err_t res = httpd_ws_send_frame_async(s_httpd, fd, &frame);
    const uint32_t t2 = millis();
    const size_t len = fb->len;
    esp_camera_fb_return(fb);

    if (res != ESP_OK) {
      Serial.printf("[ws] send failed after %lu ms: %s, dropping client\n",
                    (unsigned long)(t2 - t1), esp_err_to_name(res));
      s_ws_fd = -1;
      continue;
    }

    // Per-frame visibility. The one-second averages hide the shape of the
    // problem: a run of 10 ms frames with one 3 s outlier averages to something
    // that looks uniformly mediocre and points nowhere.
    if (t2 - t1 > 400) {
      Serial.printf("[ws] slow frame: %u bytes took %lu ms\n", (unsigned)len,
                    (unsigned long)(t2 - t1));
    }
    note_frame(t1 - t0, t2 - t1, len);
    adapt_quality(len);
  }
}

static void on_session_close(httpd_handle_t hd, int sockfd) {
  (void)hd;
  if (sockfd == s_ws_fd) {
    s_ws_fd = -1;
    Serial.println(F("[ws] video client gone"));
  }
  // No close() here: esp_http_server closes the socket itself after this
  // returns, and the descriptor may already be invalid.
}

// ---------------------------------------------------------------------------
//  GET /jpg  - one frame per request
// ---------------------------------------------------------------------------
static esp_err_t jpg_handler(httpd_req_t *req) {
  const uint32_t t0 = millis();
  camera_fb_t *fb = esp_camera_fb_get();
  const uint32_t t1 = millis();

  if (fb == nullptr) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  set_nodelay(req);
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  const esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  const uint32_t t2 = millis();
  const size_t len = fb->len;
  esp_camera_fb_return(fb);

  if (res == ESP_OK) {
    note_frame(t1 - t0, t2 - t1, len);
    adapt_quality(len);
  }
  return res;
}

// ---------------------------------------------------------------------------
//  GET /stream  - MJPEG
// ---------------------------------------------------------------------------
static esp_err_t stream_handler(httpd_req_t *req) {
  esp_err_t res = httpd_resp_set_type(req, kStreamContentType);
  if (res != ESP_OK) return res;

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  set_nodelay(req);

  Serial.println(F("[stream] MJPEG client connected"));

  uint32_t frames_sent = 0;
  int consecutive_nulls = 0;
  char part[128];

  // A handful of retries rides out a transient miss; past that the camera is
  // genuinely wedged and holding the connection open would leave the viewer
  // staring at a frozen frame. Dropping it lets the page reconnect instead.
  const int kMaxConsecutiveNulls = 20;  // ~100 ms

  while (true) {
    const uint32_t t0 = millis();
    camera_fb_t *fb = esp_camera_fb_get();
    const uint32_t t1 = millis();

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

    if (frames_sent == 0) {
      Serial.printf("[stream] first frame: %u bytes, grab %lu ms\n", (unsigned)fb->len,
                    (unsigned long)(t1 - t0));
    }

    // Boundary and part headers go out as ONE write. Every extra small send is
    // another chance to stall on the peer's ACK, and they are adjacent bytes.
    const size_t hlen = snprintf(part, sizeof(part),
                                 "%s"
                                 "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                                 kBoundary, (unsigned)fb->len);

    res = httpd_resp_send_chunk(req, part, hlen);
    if (res != ESP_OK) {
      Serial.printf("[stream] header send failed: %s\n", esp_err_to_name(res));
    } else {
      res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
      if (res != ESP_OK) {
        Serial.printf("[stream] jpeg send failed: %s (%u bytes)\n", esp_err_to_name(res),
                      (unsigned)fb->len);
      }
    }

    const uint32_t t2 = millis();
    const size_t len = fb->len;
    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;

    if (frames_sent == 0) {
      Serial.printf("[stream] first frame sent in %lu ms\n", (unsigned long)(t2 - t1));
    }
    frames_sent++;
    note_frame(t1 - t0, t2 - t1, len);
  }

  Serial.printf("[stream] MJPEG client gone after %lu frame(s)\n", (unsigned long)frames_sent);
  return res;
}

// ---------------------------------------------------------------------------
//  GET /  - viewer page with a transport A/B switch
// ---------------------------------------------------------------------------
static const char kIndexHtml[] = R"HTML(<!DOCTYPE html><html><head>
<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>TankCam</title><style>
html,body{margin:0;height:100%;background:#0b0d11;color:#e8eef7;font:14px -apple-system,sans-serif}
#v{position:fixed;inset:0;width:100%;height:100%;object-fit:contain;background:#000}
#hud{position:fixed;top:0;left:0;right:0;padding:8px 12px;z-index:2;
 background:linear-gradient(#000c,#0000);display:flex;gap:12px;align-items:center}
#m{padding:6px 12px;border-radius:8px;border:1px solid #fff3;background:#161a21;color:#e8eef7}
b{font-variant-numeric:tabular-nums}
</style></head><body>
<img id=v alt="">
<div id=hud><button id=m>mode</button><span>client <b id=f>--</b> fps</span><span id=t></span></div>
<script>
var v=document.getElementById('v'),f=document.getElementById('f'),
    t=document.getElementById('t'),m=document.getElementById('m');

// WebSocket push first. The send buffer in this build is about one frame, so
// every round trip costs a frame - and push is the only transport here that
// does not spend one per frame just being asked.
var modes=['mjpeg','ws','poll'], mi=0, gen=0, n=0, t0=Date.now(), ws=null, prev=null;

function tick(){
  n++; var dt=(Date.now()-t0)/1000;
  if(dt>=1){ f.textContent=(n/dt).toFixed(1); n=0; t0=Date.now(); }
}
function stopAll(){
  gen++;
  if(ws){ try{ws.onclose=null;ws.close();}catch(e){} ws=null; }
  v.onload=null; v.onerror=null;
}
function startWs(g){
  ws=new WebSocket('ws://'+location.host+'/ws');
  ws.binaryType='blob';
  ws.onmessage=function(e){
    if(g!==gen) return;
    var u=URL.createObjectURL(e.data), old=prev; prev=u;
    // Revoke the previous URL only once the new frame has decoded, otherwise
    // the image blanks between frames.
    v.onload=function(){ if(old) URL.revokeObjectURL(old); };
    v.src=u; tick();
  };
  ws.onclose=function(){ if(g===gen) setTimeout(function(){ if(g===gen) startWs(g); },1000); };
}
function poll(g){
  if(g!==gen) return;
  var i=new Image();
  i.onload=function(){ if(g!==gen)return; v.src=i.src; tick(); poll(g); };
  i.onerror=function(){ if(g!==gen)return; setTimeout(function(){poll(g)},500); };
  i.src='/jpg?n='+Date.now();
}
function start(){
  stopAll(); n=0; t0=Date.now(); f.textContent='--';
  var g=gen, mode=modes[mi];
  if(mode==='ws'){ t.textContent='WebSocket push'; v.src=''; startWs(g); }
  else if(mode==='poll'){ t.textContent='GET /jpg per frame'; v.src=''; poll(g); }
  else { t.textContent='multipart /stream'; v.src='/stream?n='+Date.now(); }
  m.textContent=mode;
}
m.onclick=function(){ mi=(mi+1)%modes.length; start(); };
start();
</script></body></html>)HTML";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

// Browsers request this unprompted. Without a handler it 404s and the socket
// lingers, which is wasted capacity on a server holding a long-lived stream.
static esp_err_t favicon_handler(httpd_req_t *req) {
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, nullptr, 0);
}

// ---------------------------------------------------------------------------
//  Public API
// ---------------------------------------------------------------------------
bool stream_server_begin(uint16_t port, bool with_index) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = port;

  // Every httpd instance needs its own control socket. They all default to
  // 32768, so a second instance silently fails to start unless this is bumped.
  config.ctrl_port = 32768 + (port - 80);

  // Everything else stays at HTTPD_DEFAULT_CONFIG, matching the MJPEG2SD
  // reference. Each override I added while debugging - socket counts, LRU
  // purging, send timeouts - fixed the symptom in front of me and introduced a
  // new variable. Defaults first; re-add individually only with a measurement
  // that justifies it.
  config.max_uri_handlers = 6;
  config.close_fn = on_session_close;

  if (httpd_start(&s_httpd, &config) != ESP_OK) {
    Serial.printf("[stream] httpd_start failed on port %u\n", (unsigned)port);
    return false;
  }

  const httpd_uri_t ws_uri = {"/ws", HTTP_GET, ws_handler, nullptr, true, false, nullptr};
  httpd_register_uri_handler(s_httpd, &ws_uri);

  const httpd_uri_t jpg_uri = {"/jpg", HTTP_GET, jpg_handler, nullptr};
  httpd_register_uri_handler(s_httpd, &jpg_uri);

  const httpd_uri_t stream_uri = {"/stream", HTTP_GET, stream_handler, nullptr};
  httpd_register_uri_handler(s_httpd, &stream_uri);

  const httpd_uri_t favicon_uri = {"/favicon.ico", HTTP_GET, favicon_handler, nullptr};
  httpd_register_uri_handler(s_httpd, &favicon_uri);

  if (with_index) {
    const httpd_uri_t index_uri = {"/", HTTP_GET, index_handler, nullptr};
    httpd_register_uri_handler(s_httpd, &index_uri);
  }

  // Priority 5 matches the httpd task, so pushing frames cannot starve request
  // handling. 4 kB of stack because the send path goes through lwip.
  xTaskCreatePinnedToCore(video_push_task, "vpush", 4096, nullptr, 5, nullptr, 1);

  Serial.printf("[stream] port %u: /ws (push), /jpg (per-frame), /stream (MJPEG)\n",
                (unsigned)port);
  return true;
}

void stream_server_stop() {
  if (s_httpd != nullptr) {
    httpd_stop(s_httpd);
    s_httpd = nullptr;
  }
}

// Both transports are counted the same way, so "a client is active" simply
// means frames went out recently. Poll mode has no persistent connection to
// track, so connection state would be the wrong thing to measure.
bool stream_server_has_client() {
  const uint32_t last = s_last_frame_ms;
  return last != 0 && (millis() - last) < 2000;
}

float stream_server_fps() { return stream_server_has_client() ? s_fps : 0.0f; }

void stream_server_timing(uint32_t *grab_ms, uint32_t *send_ms, uint32_t *avg_bytes,
                          uint32_t *gap_ms) {
  if (grab_ms != nullptr) *grab_ms = s_grab_ms;
  if (send_ms != nullptr) *send_ms = s_send_ms;
  if (avg_bytes != nullptr) *avg_bytes = s_avg_bytes;
  if (gap_ms != nullptr) *gap_ms = s_gap_ms;
}
