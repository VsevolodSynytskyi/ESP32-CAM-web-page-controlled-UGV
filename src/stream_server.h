#pragma once

#include <stdint.h>

// MJPEG video: GET /stream, multipart/x-mixed-replace, on its own
// esp_http_server instance.
//
// Its own instance is not optional. An MJPEG response never ends, and each
// instance dispatches every handler from a single task - so a stream sharing an
// instance with the page and the control socket would own that task forever and
// nothing else would ever be served.
bool stream_server_begin(uint16_t port);

// True while a client is consuming the stream.
bool stream_server_has_client();
