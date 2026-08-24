#pragma once

#include <stdint.h>
#include <stddef.h>

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

// The most recent stats window as a short line, e.g.
//
//   15.0fps  6.0kB  90kB/s  busy45%  rssi-44  ch1
//
// Empty when nothing is streaming. It goes to the page rather than only to
// serial because the MB shield occupies the very header pins the motors and the
// buck need: the vehicle can be on battery or on serial, never both, and the
// numbers are only true on battery.
void stream_server_stats(char *out, size_t len);
