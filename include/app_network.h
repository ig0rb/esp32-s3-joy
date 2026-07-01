#pragma once

#include <Arduino.h>

namespace app {

struct NetworkSnapshot {
  bool is_station = false;
  String ssid;
  String ip = "0.0.0.0";
};

void registerWiFiEventLogger();
void startNetwork();
void maintainStationMode();
NetworkSnapshot copyNetworkSnapshot();

}  // namespace app
