#pragma once

#include <stdint.h>

// MJPEG video over HTTP, on two esp_http_server instances:
//
//   page_port      GET /        full-screen viewer
//   page_port + 1  GET /stream  multipart/x-mixed-replace
//
// Two instances is not optional. An MJPEG response never ends, and each
// instance dispatches every handler from a single task - so a stream sharing an
// instance with the UI owns that task forever and nothing else gets served.
bool stream_server_begin(uint16_t page_port);

// True while a client is consuming the stream.
bool stream_server_has_client();
