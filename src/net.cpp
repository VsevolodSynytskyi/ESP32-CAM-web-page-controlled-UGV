#include "net.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "config.h"

// secrets.h is gitignored and only Stage 1b needs it. Guarding the include lets
// every other stage compile on a fresh checkout where it does not exist yet.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif

bool net_begin_sta() {
  if (WIFI_SSID[0] == '\0') {
    Serial.println(F("[net] no credentials. Copy include/secrets.h.example to"));
    Serial.println(F("[net] include/secrets.h and fill in your network details."));
    return false;
  }

  Serial.printf("[net] joining \"%s\"", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // Modem sleep parks the radio between beacons. It saves power and ruins
  // latency, so it goes off for anything interactive.
  WiFi.setSleep(false);

  const uint32_t deadline = millis() + STA_CONNECT_TIMEOUT_MS;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[net] failed to connect (status %d)\n", WiFi.status());
    Serial.println(F("[net] check the SSID/password, and note the ESP32 is 2.4 GHz"));
    Serial.println(F("[net] only - it cannot see a 5 GHz-only network."));
    return false;
  }

  Serial.printf("[net] connected, IP %s, RSSI %d dBm\n", WiFi.localIP().toString().c_str(),
                WiFi.RSSI());
  return true;
}

// Scans the 2.4 GHz band and returns the quietest of channels 1, 6 and 11.
//
// Only those three are considered: they are the non-overlapping set, so sitting
// between them means colliding with two neighbours instead of one. Each network
// found is scored against every channel it can interfere with - a 20 MHz
// carrier bleeds roughly +/-4 channels - weighted by how loud it is, since a
// strong neighbour steals far more airtime than a distant one.
static int scan_for_quietest_channel() {
  Serial.println(F("[net] scanning 2.4 GHz for the quietest channel..."));

  WiFi.mode(WIFI_STA);
  const int found = WiFi.scanNetworks(false, true, false, 120);
  if (found <= 0) {
    Serial.println(F("[net] scan found nothing, defaulting to channel 6"));
    WiFi.scanDelete();
    return 6;
  }

  int score[14] = {0};
  for (int i = 0; i < found; i++) {
    const int ch = WiFi.channel(i);
    const int rssi = WiFi.RSSI(i);
    if (ch < 1 || ch > 13) continue;

    // A neighbour 30 dB louder does not steal 30x the airtime, but it does
    // dominate, so the weighting is coarse and deliberately top-heavy.
    const int weight = (rssi > -50) ? 8 : (rssi > -65) ? 4 : (rssi > -80) ? 2 : 1;

    for (int c = 1; c <= 13; c++) {
      const int distance = abs(c - ch);
      if (distance <= 4) score[c] += weight * (5 - distance);
    }
  }
  WiFi.scanDelete();

  const int candidates[3] = {1, 6, 11};
  int best = 6;
  int best_score = INT32_MAX;
  for (int i = 0; i < 3; i++) {
    const int c = candidates[i];
    Serial.printf("[net]   channel %-2d congestion score %d\n", c, score[c]);
    if (score[c] < best_score) {
      best_score = score[c];
      best = c;
    }
  }

  Serial.printf("[net] %d networks in range, choosing channel %d\n", found, best);
  return best;
}

bool net_begin_ap() {
  const int channel = (AP_CHANNEL >= 1 && AP_CHANNEL <= 13) ? AP_CHANNEL
                                                            : scan_for_quietest_channel();

  Serial.printf("[net] starting SoftAP \"%s\" on channel %d\n", AP_SSID, channel);

  WiFi.mode(WIFI_AP);

  // Set the address before bringing the AP up so the DHCP server hands out
  // leases on the range we advertise, rather than relying on the default.
  const IPAddress ip(192, 168, 4, 1);
  const IPAddress gateway(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  if (!WiFi.softAPConfig(ip, gateway, subnet)) {
    Serial.println(F("[net] softAPConfig failed"));
    return false;
  }

  // ssid_hidden = 0, max_connection = AP_MAX_CONN. Extra clients would only
  // steal bandwidth from the pilot, so the cap is deliberate.
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD, channel, 0, AP_MAX_CONN)) {
    Serial.println(F("[net] softAP failed to start"));
    return false;
  }

  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  // Drop 802.11b. Its lowest rates are 1-2 Mbit/s, and a single b-rate frame
  // occupies airtime that would carry many times the data at n rates - which is
  // exactly the wrong trade on a contended channel. Every phone from the last
  // fifteen years speaks g/n.
  esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);

  Serial.printf("[net] AP up. Join \"%s\" and open http://%s/\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());
  Serial.println(F("[net] Your phone will warn about 'no internet' - that is"));
  Serial.println(F("[net] expected. Turn mobile data off, or tell Android to"));
  Serial.println(F("[net] stay connected, or it will silently fall back to cell."));
  return true;
}
