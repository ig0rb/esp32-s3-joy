#include "app_network.h"

#include "app_build_config.h"

#include <WiFi.h>

#include <cstring>

#include "esp_wifi.h"

namespace app {
namespace {

constexpr uint32_t kStationRetryIntervalMs = 5000;
constexpr uint32_t kStationWaitLogIntervalMs = 2000;

bool g_network_is_station = false;
String g_network_ssid = APP_AP_SSID;
String g_network_ip = "0.0.0.0";
wl_status_t g_last_station_status = WL_IDLE_STATUS;
uint32_t g_last_station_attempt_ms = 0;
uint32_t g_last_station_wait_log_ms = 0;

const char *wifiStatusName(const wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD:
      return "NO_SHIELD";
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SSID_AVAIL:
      return "NO_SSID";
    case WL_SCAN_COMPLETED:
      return "SCAN_DONE";
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_CONNECT_FAILED:
      return "CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    default:
      return "UNKNOWN";
  }
}

const char *wifiEventName(const WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_READY:
      return "READY";
    case ARDUINO_EVENT_WIFI_STA_START:
      return "STA_START";
    case ARDUINO_EVENT_WIFI_STA_STOP:
      return "STA_STOP";
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      return "STA_CONNECTED";
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      return "STA_DISCONNECTED";
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      return "STA_GOT_IP";
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      return "STA_LOST_IP";
    case ARDUINO_EVENT_WIFI_STA_GOT_IP6:
      return "STA_GOT_IP6";
    case ARDUINO_EVENT_WIFI_AP_START:
      return "AP_START";
    case ARDUINO_EVENT_WIFI_AP_STOP:
      return "AP_STOP";
    default:
      return "OTHER";
  }
}

bool stationHasIpAddress() {
  return WiFi.status() == WL_CONNECTED &&
         static_cast<uint32_t>(WiFi.localIP()) != 0;
}

void refreshNetworkStatus() {
  if (g_network_is_station) {
    g_network_ssid = APP_WIFI_SSID;
    g_network_ip = stationHasIpAddress() ? WiFi.localIP().toString() : "0.0.0.0";
    return;
  }

  g_network_ssid = APP_AP_SSID;
  g_network_ip = WiFi.softAPIP().toString();
}

void beginStationConnection(const char *reason) {
  (void)reason;
  g_last_station_attempt_ms = millis();
  g_last_station_wait_log_ms = millis();

  esp_wifi_set_max_tx_power(20);
  if (std::strlen(APP_WIFI_PASSWORD) == 0) {
    WiFi.begin(APP_WIFI_SSID);
  } else {
    WiFi.begin(APP_WIFI_SSID, APP_WIFI_PASSWORD);
  }
}

void logStationEvent(const WiFiEvent_t event, const WiFiEventInfo_t info) {
  (void)event;
  (void)info;
  if (!g_network_is_station) {
    return;
  }
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP);

  IPAddress local_ip;
  IPAddress gateway;
  IPAddress subnet;
  if (local_ip.fromString(APP_AP_IP) && gateway.fromString(APP_AP_GATEWAY) &&
      subnet.fromString(APP_AP_SUBNET)) {
    WiFi.softAPConfig(local_ip, gateway, subnet);
  }

  const size_t password_length = std::strlen(APP_AP_PASSWORD);
  bool ap_ok = false;
  if (password_length == 0) {
    ap_ok = WiFi.softAP(APP_AP_SSID, nullptr, APP_AP_CHANNEL, 0,
                        APP_AP_MAX_CLIENTS);
  } else if (password_length >= 8) {
    ap_ok = WiFi.softAP(APP_AP_SSID, APP_AP_PASSWORD, APP_AP_CHANNEL, 0,
                        APP_AP_MAX_CLIENTS);
  } else {
    Serial.println("AP password too short, starting open AP");
    ap_ok = WiFi.softAP(APP_AP_SSID, nullptr, APP_AP_CHANNEL, 0,
                        APP_AP_MAX_CLIENTS);
  }

  if (!ap_ok) {
    Serial.println("WiFi AP start failed");
    return;
  }

  g_network_is_station = false;
  g_network_ssid = APP_AP_SSID;
  g_network_ip = WiFi.softAPIP().toString();
  Serial.printf("AP ready: SSID=%s IP=%s channel=%d\n", APP_AP_SSID,
                WiFi.softAPIP().toString().c_str(), APP_AP_CHANNEL);
}

bool startStationMode() {
  if (std::strlen(APP_WIFI_SSID) == 0) {
    Serial.println("Dev mode enabled but wifi_ssid is empty");
    return false;
  }

  g_network_is_station = true;
  g_network_ssid = APP_WIFI_SSID;
  g_network_ip = "0.0.0.0";
  g_last_station_status = WL_IDLE_STATUS;
  g_last_station_attempt_ms = 0;
  g_last_station_wait_log_ms = 0;

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  const IPAddress no_ip(static_cast<uint32_t>(0U));
  if (!WiFi.config(no_ip, no_ip, no_ip)) {
    Serial.println("DEV MODE: failed to force DHCP mode");
  } else {
    Serial.println("DEV MODE: DHCP enabled");
  }

  WiFi.disconnect(false, false);
  delay(100);

  beginStationConnection("initial attempt");

  const uint32_t start_ms = millis();
  while (!stationHasIpAddress() &&
         (millis() - start_ms) < APP_WIFI_CONNECT_TIMEOUT_MS) {
    const wl_status_t status = WiFi.status();
    if (status != g_last_station_status) {
      g_last_station_status = status;
    }
    delay(250);
  }

  refreshNetworkStatus();
  if (!stationHasIpAddress()) {
    return false;
  }

  Serial.printf("STA ready: SSID=%s IP=%s\n", APP_WIFI_SSID,
                g_network_ip.c_str());
  return true;
}

}  // namespace

void registerWiFiEventLogger() {
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    logStationEvent(event, info);
    refreshNetworkStatus();
  });
}

void startNetwork() {
  if (APP_DEV_MODE != 0) {
    startStationMode();
    return;
  }

  startAccessPoint();
}

void maintainStationMode() {
  if (!g_network_is_station) {
    return;
  }

  refreshNetworkStatus();

  const wl_status_t status = WiFi.status();
  if (status != g_last_station_status) {
    g_last_station_status = status;
  }

  if (stationHasIpAddress()) {
    return;
  }

  const uint32_t now = millis();
  if ((now - g_last_station_wait_log_ms) >= kStationWaitLogIntervalMs) {
    g_last_station_wait_log_ms = now;
  }

  if ((now - g_last_station_attempt_ms) >= kStationRetryIntervalMs) {
    beginStationConnection("retry");
  }
}

NetworkSnapshot copyNetworkSnapshot() {
  refreshNetworkStatus();

  NetworkSnapshot snapshot = {};
  snapshot.is_station = g_network_is_station;
  snapshot.ssid = g_network_ssid;
  snapshot.ip = g_network_ip;
  return snapshot;
}

}  // namespace app
