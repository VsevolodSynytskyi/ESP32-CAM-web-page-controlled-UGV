#pragma once

#include <stdint.h>

// The page and the control link, on one esp_http_server instance:
//
//   GET /          the viewer page  (web_page.h)
//   WS  /control   two signed bytes, left then right, each -100..+100
//
// Video lives on a separate instance one port up - see stream_server.h for why
// that separation is load-bearing.
//
// Handlers here must return promptly. They share a single dispatch task with
// each other, so a slow one stalls the control link.
bool web_server_begin(uint16_t port);
