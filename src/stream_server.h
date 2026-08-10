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
// Serves GET /stream as multipart/x-mixed-replace.

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
