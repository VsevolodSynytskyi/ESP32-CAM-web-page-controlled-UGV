#pragma once

#include <stdint.h>

// MJPEG streaming server, on its own esp_http_server instance.
//
// Why a separate instance from the UI/control server: an MJPEG response never
// ends. On a shared instance it permanently occupies the worker and the control
// endpoint stops answering - which is exactly the failure you cannot afford
// while steering. Two instances is why the official CameraWebServer is built
// this way.
//
// Two video transports are served, and the viewer page can switch between them
// at runtime so they can be compared on one device:
//
//   GET /stream   multipart/x-mixed-replace MJPEG. Efficient, no per-frame HTTP
//                 overhead. Works in Chrome and Firefox.
//   GET /jpg      one JPEG per request; the client asks for the next as soon as
//                 the current decodes. Costs a request per frame but is plain
//                 HTTP, which every browser handles - including iOS Safari,
//                 which is unreliable with x-mixed-replace and is the only
//                 engine available on iOS.

// port          - STREAM_PORT (81) alongside the web server, or 80 in Stage 1b
//                 where nothing else is listening.
// with_index    - also serve a bare-bones GET / page wrapping the stream in an
//                 <img>. Handy on a laptop in Stage 1b; leave false from
//                 Stage 3 on, where web_server owns the real UI.
bool stream_server_begin(uint16_t port, bool with_index);

void stream_server_stop();

// Frames per second pushed to the client over the last second, or 0 if nobody
// is watching.
float stream_server_fps();

// True while a client is consuming the stream.
bool stream_server_has_client();

// Per-frame averages over the last second. This is the diagnosis when the
// stream is slow, and the three numbers point at three different culprits:
//
//   grab_ms  the camera / JPEG encoder
//   send_ms  the radio
//   gap_ms   dead time waiting to be asked for the next frame - the client,
//            or the server refusing connections
//
// Any pointer may be null.
void stream_server_timing(uint32_t *grab_ms, uint32_t *send_ms, uint32_t *avg_bytes,
                          uint32_t *gap_ms);

// Adaptive JPEG quality. Frame SIZE is what matters on this link: the Arduino
// framework's prebuilt lwip fixes the TCP send buffer at 5760 bytes, so a frame
// that fits inside it costs one window round trip and a frame twice that size
// costs two. JPEG size swings widely with scene detail, so a fixed quality
// cannot hold that budget - this walks quality to keep frames under the window.
// On by default; the q/Q serial keys turn it off and take manual control.
void stream_server_set_adaptive(bool on);
bool stream_server_adaptive();
int stream_server_quality();
