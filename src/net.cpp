#include "net.h"

#include <Arduino.h>
#include <WiFi.h>

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

bool net_begin_ap() {
  Serial.printf("[net] starting SoftAP \"%s\" on channel %d\n", AP_SSID, AP_CHANNEL);

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
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_CONN)) {
    Serial.println(F("[net] softAP failed to start"));
    return false;
  }

  WiFi.setSleep(false);

  Serial.printf("[net] AP up. Join \"%s\" and open http://%s/\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());
  Serial.println(F("[net] Your phone will warn about 'no internet' - that is"));
  Serial.println(F("[net] expected. Turn mobile data off, or tell Android to"));
  Serial.println(F("[net] stay connected, or it will silently fall back to cell."));
  return true;
}
