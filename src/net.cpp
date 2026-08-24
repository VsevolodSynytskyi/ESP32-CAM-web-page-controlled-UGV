#include "net.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "config.h"

// secrets.h is gitignored and only station mode needs it. Guarding the include
// lets a fresh checkout compile before anyone has created it.
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
// Does a multiple of the camera's pixel clock fall inside this channel?
//
// Every digital clock on the camera ribbon radiates at every multiple of
// itself, and that ribbon is close to a quarter wave at 2.4 GHz - so a harmonic
// inside the channel we are about to transmit on raises our own noise floor.
// Measured on this board: 20 MHz XCLK cost ~20x throughput, and 20 MHz is the
// worst possible choice because 2420 / 2440 / 2460 put a harmonic in all three
// non-overlapping channels at once.
//
// Arithmetic is in 0.1 MHz units so 16.5 MHz stays exact. Uses CAM_XCLK_HZ
// rather than the live value because the AP is brought up once at boot; the 'x'
// serial key can move XCLK afterwards without the channel following it.
static bool xclk_harmonic_in_channel(int channel) {
  const int centre = 2407 + 5 * channel;  // 802.11 2.4 GHz channel plan, MHz
  const int lo = (centre - 10) * 10;      // 20 MHz occupied bandwidth
  const int hi = (centre + 10) * 10;
  const int xclk = CAM_XCLK_HZ / 100000;  // 0.1 MHz units: 24 MHz -> 240
  if (xclk <= 0) return false;

  for (int k = lo / xclk; k <= hi / xclk + 1; k++) {
    const int harmonic = k * xclk;
    if (harmonic >= lo && harmonic <= hi) return true;
  }
  return false;
}

int net_channel() {
  uint8_t primary = 0;
  wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
  if (esp_wifi_get_channel(&primary, &second) != ESP_OK) return 0;
  return primary;
}

bool net_channel_jammed() {
  const int ch = net_channel();
  return ch >= 1 && ch <= 14 && xclk_harmonic_in_channel(ch);
}

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
    const bool jammed = xclk_harmonic_in_channel(c);

    // Deliberately larger than any congestion score the loop above can produce.
    // A neighbouring network shares the airtime; our own camera clock raises the
    // noise floor, and no amount of politeness recovers from that. Measured at
    // 20 MHz XCLK on this board: ~20x throughput loss, and it was indifferent to
    // congestion - channel score 0 was just as bad as channel score 56.
    const int total = score[c] + (jammed ? 1000 : 0);

    Serial.printf("[net]   channel %-2d congestion %-3d  %s\n", c, score[c],
                  jammed ? "XCLK HARMONIC LANDS HERE - avoid" : "harmonic clear");
    if (total < best_score) {
      best_score = total;
      best = c;
    }
  }

  if (best_score >= 1000) {
    Serial.println(F("[net] every channel has an XCLK harmonic in it - expect poor"));
    Serial.println(F("[net] throughput. Change CAM_XCLK_HZ; 20 MHz cannot avoid this."));
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

  // Modem sleep off is the only radio setting we touch. Everything else was
  // tried during the throughput hunt and every one of them added a variable
  // without adding speed - the fix was the pixel clock, not WiFi tuning.
  WiFi.setSleep(false);

  Serial.printf("[net] AP up. Join \"%s\" and open http://%s/\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());
  Serial.println(F("[net] Your phone will warn about 'no internet' - that is"));
  Serial.println(F("[net] expected. Turn mobile data off, or tell Android to"));
  Serial.println(F("[net] stay connected, or it will silently fall back to cell."));
  return true;
}

