#pragma once

#include <stdbool.h>

// The UI and control server, on port HTTP_PORT (80). The MJPEG stream lives on
// a separate esp_http_server instance (see stream_server.h) precisely so that a
// never-ending stream response cannot block this one.
//
// Endpoints:
//   GET /     the mobile control page from web_page.h
//   GET /ws   WebSocket control channel (only when with_control is true)
//
// The control protocol is deliberately tiny: a 2-byte binary frame carrying
// signed int8 left and right throttle in -100..+100, at 20 Hz. Sign is
// direction. Text frames from the server carry status ("fps=23.4").

// with_control - register /ws and accept throttle commands (Stage 4). False in
// Stage 3, where the page is served video-only.
bool web_server_begin(bool with_control);

void web_server_stop();

// True while a WebSocket control client is connected.
bool web_server_has_control_client();

// Push a status line to the control client. Safe to call from another task;
// does nothing if nobody is connected.
void web_server_push_status(float fps);
