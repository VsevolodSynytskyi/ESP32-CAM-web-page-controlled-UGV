#pragma once

// WiFi bring-up. Two modes:
//
//   Station  - joins your home network. Handy on the bench: the laptop keeps
//              its internet connection and DevTools.
//   SoftAP   - the vehicle hosts its own network at 192.168.4.1, so no router
//              is involved and it works anywhere. This is the field mode.
//
// Both disable WiFi modem sleep, which is on by default and adds 100-200 ms of
// jitter - fatal for a video feed you are steering by.

// Joins WIFI_SSID / WIFI_PASSWORD from include/secrets.h. Returns false on
// timeout (STA_CONNECT_TIMEOUT_MS) or if secrets.h is missing.
bool net_begin_sta();

// Starts the SoftAP described by AP_* in config.h. Returns false if the AP
// could not be brought up.
bool net_begin_ap();

