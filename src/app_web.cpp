#include "app_web.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <WebServer.h>

#include <cstring>

#include "app_build_config.h"
#include "app_network.h"
#include "app_state.h"
#include "app_storage.h"
#include "app_types.h"

namespace app {
namespace {

WebServer g_web_server(80);
bool g_reboot_pending = false;
uint32_t g_reboot_after_ms = 0;

String jsonEscaped(const char *text) {
  String escaped;
  if (text == nullptr) {
    return escaped;
  }

  escaped.reserve(std::strlen(text) + 8);
  for (const char *cursor = text; *cursor != '\0'; ++cursor) {
    switch (*cursor) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped += *cursor;
        break;
    }
  }
  return escaped;
}

void appendJsonString(String *json, const char *text) {
  if (json == nullptr) {
    return;
  }
  *json += '"';
  *json += jsonEscaped(text);
  *json += '"';
}

const char *contentTypeForPath(const String &path) {
  if (path.endsWith(".html")) {
    return "text/html; charset=utf-8";
  }
  if (path.endsWith(".css")) {
    return "text/css; charset=utf-8";
  }
  if (path.endsWith(".js")) {
    return "application/javascript; charset=utf-8";
  }
  if (path.endsWith(".json")) {
    return "application/json; charset=utf-8";
  }
  if (path.endsWith(".png")) {
    return "image/png";
  }
  if (path.endsWith(".svg")) {
    return "image/svg+xml";
  }
  return "text/plain; charset=utf-8";
}

bool serveStaticFile(String path) {
  if (!isFileSystemReady()) {
    return false;
  }

  if (path.endsWith("/")) {
    path += "index.html";
  }
  if (!path.startsWith("/")) {
    path = "/" + path;
  }
  if (!LittleFS.exists(path)) {
    return false;
  }

  File file = LittleFS.open(path, "r");
  if (!file) {
    return false;
  }

  g_web_server.streamFile(file, contentTypeForPath(path));
  file.close();
  return true;
}

void sendJsonResponse(const String &payload, const int status_code = 200) {
  g_web_server.sendHeader("Cache-Control", "no-store");
  g_web_server.send(status_code, "application/json; charset=utf-8", payload);
}

void sendJsonError(const int status_code, const char *message) {
  String payload = "{\"error\":";
  appendJsonString(&payload, message);
  payload += '}';
  sendJsonResponse(payload, status_code);
}

void scheduleRestart(const uint32_t delay_ms) {
  g_reboot_pending = true;
  g_reboot_after_ms = millis() + delay_ms;
}

void appendChannelConfigJson(String *json, const ChannelConfig &channel,
                             const size_t index) {
  if (json == nullptr) {
    return;
  }

  *json += "{\"index\":";
  *json += String(index + 1);
  *json += ",\"mode\":";
  appendJsonString(json, channelModeName(channel.mode));
  *json += ",\"modeLabel\":";
  appendJsonString(json, channelModeLabel(channel.mode));
  *json += ",\"source\":";
  const String source_name = channelSourceName(channel.source_index);
  appendJsonString(json, source_name.c_str());
  *json += ",\"invert\":";
  *json += channel.invert ? "true" : "false";
  *json += ",\"failsafePulse\":";
  *json += String(channel.failsafe_pulse_us);
  *json += ",\"stepper\":{\"upButton\":";
  const String up_button_name = channelSourceName(channel.stepper.up_source_index);
  appendJsonString(json, up_button_name.c_str());
  *json += ",\"downButton\":";
  const String down_button_name =
      channelSourceName(channel.stepper.down_source_index);
  appendJsonString(json, down_button_name.c_str());
  *json += ",\"stepPulse\":";
  *json += String(channel.stepper.step_pulse_us);
  *json += ",\"minPulse\":";
  *json += String(channel.stepper.min_pulse_us);
  *json += ",\"maxPulse\":";
  *json += String(channel.stepper.max_pulse_us);
  *json += ",\"initialPulse\":";
  *json += String(channel.stepper.initial_pulse_us);
  *json += "},\"latch\":{\"button\":";
  const String latch_button_name =
      channelSourceName(channel.latch.button_source_index);
  appendJsonString(json, latch_button_name.c_str());
  *json += ",\"activePulse\":";
  *json += String(channel.latch.active_pulse_us);
  *json += ",\"resetPulse\":";
  *json += String(channel.latch.reset_pulse_us);
  *json += "},\"selector\":{\"defaultPulse\":";
  *json += String(channel.selector.default_pulse_us);
  *json += ",\"entries\":[";
  const size_t entry_count =
      (channel.selector.entry_count <=
       static_cast<uint8_t>(kChannelSelectorEntryCount))
          ? channel.selector.entry_count
          : kChannelSelectorEntryCount;
  for (size_t entry_index = 0; entry_index < entry_count; ++entry_index) {
    if (entry_index > 0) {
      *json += ',';
    }
    *json += "{\"button\":";
    const String entry_button_name = channelSourceName(
        channel.selector.entries[entry_index].button_source_index);
    appendJsonString(json, entry_button_name.c_str());
    *json += ",\"pulse\":";
    *json += String(channel.selector.entries[entry_index].pulse_us);
    *json += '}';
  }
  *json += "]}";
  *json += '}';
}

String channelDetailLabel(const ChannelConfig &channel,
                          const ChannelOutputState &output) {
  switch (channel.mode) {
    case ChannelMode::Stepper:
      return String("Up ") +
             channelSourceLabel(channel.stepper.up_source_index) +
             " | Down " +
             channelSourceLabel(channel.stepper.down_source_index) +
             " | step " + String(channel.stepper.step_pulse_us) + " us | " +
             String(channel.stepper.min_pulse_us) + ".." +
             String(channel.stepper.max_pulse_us) + " us";
    case ChannelMode::Latch:
      return String(channelSourceLabel(channel.latch.button_source_index)) +
             " | " + (output.latched ? "attivo" : "reset") + " | " +
             String(channel.latch.active_pulse_us) + "/" +
             String(channel.latch.reset_pulse_us) + " us";
    case ChannelMode::Selector: {
      String detail = String(channel.selector.entry_count) + " voci";
      if (output.active_selector_index >= 0 &&
          output.active_selector_index <
              static_cast<int>(channel.selector.entry_count)) {
        const ChannelSelectorEntryConfig &entry =
            channel.selector.entries[static_cast<size_t>(
                output.active_selector_index)];
        detail += " | ";
        detail += channelSourceLabel(entry.button_source_index);
        detail += " -> ";
        detail += String(entry.pulse_us);
        detail += " us";
      } else {
        detail += " | default ";
        detail += String(channel.selector.default_pulse_us);
        detail += " us";
      }
      return detail;
    }
    case ChannelMode::Source:
    default:
      return channelSourceLabel(channel.source_index);
  }
}

String buildConfigJson(const AppConfig &config, const bool storage_ok,
                       const uint32_t revision) {
  const NetworkSnapshot network = copyNetworkSnapshot();

  String json;
  json.reserve(6144);
  json += "{\"ap\":{\"ssid\":";
  appendJsonString(&json, APP_AP_SSID);
  json += ",\"passwordSet\":";
  json += (std::strlen(APP_AP_PASSWORD) >= 8) ? "true" : "false";
  json += "},\"network\":{\"devMode\":";
  json += (APP_DEV_MODE != 0) ? "true" : "false";
  json += ",\"mode\":";
  appendJsonString(&json, network.is_station ? "sta" : "ap");
  json += ",\"ssid\":";
  appendJsonString(&json, network.ssid.c_str());
  json += ",\"ip\":";
  appendJsonString(&json, network.ip.c_str());
  json += "},\"app\":{\"pollMs\":";
  json += String(APP_UI_POLL_MS);
  json += ",\"ppmChannelCount\":";
  json += String(kPpmChannelCount);
  json += ",\"buttonCount\":";
  json += String(kButtonCount);
  json += ",\"analogInputCount\":";
  json += String(kAnalogInputCount);
  json += ",\"selectorEntryCount\":";
  json += String(kChannelSelectorEntryCount);
  json += "},\"storageOk\":";
  json += storage_ok ? "true" : "false";
  json += ",\"revision\":";
  json += String(revision);
  json += ",\"axes\":[";
  for (size_t i = 0; i < kAxisCount; ++i) {
    if (i > 0) {
      json += ',';
    }
    json += "{\"id\":";
    appendJsonString(&json, axisDescriptorAt(i).name);
    json += ",\"label\":";
    appendJsonString(&json, axisDescriptorAt(i).label);
    json += ",\"analog\":";
    json += axisDescriptorAt(i).analog_input_index >= 0 ? "true" : "false";
    json += ",\"trim\":";
    json += String(config.axes[i].trim_percent);
    json += ",\"deadZone\":";
    json += String(config.axes[i].dead_zone_percent);
    json += ",\"expo\":";
    json += String(config.axes[i].expo_percent);
    json += ",\"calibrationMin\":";
    json += String(config.axes[i].calibration_min);
    json += ",\"calibrationMax\":";
    json += String(config.axes[i].calibration_max);
    json += '}';
  }
  json += "],\"channels\":[";
  for (size_t i = 0; i < kPpmChannelCount; ++i) {
    if (i > 0) {
      json += ',';
    }
    appendChannelConfigJson(&json, config.channels[i], i);
  }
  json += "]}";
  return json;
}

String buildStateJson(const DeviceSnapshot &snapshot, const AppConfig &config,
                      const uint32_t config_revision,
                      const uint32_t analog_sequence) {
  String json;
  json.reserve(7168);
  json += "{\"connected\":";
  json += snapshot.connected ? "true" : "false";
  json += ",\"sequence\":";
  json += String(snapshot.sequence);
  json += ",\"analogSequence\":";
  json += String(analog_sequence);
  json += ",\"configRevision\":";
  json += String(config_revision);
  json += ",\"device\":{\"address\":";
  json += String(snapshot.address);
  json += ",\"interface\":";
  json += String(snapshot.interface_number);
  json += ",\"protocol\":";
  appendJsonString(&json, snapshot.protocol);
  json += "},\"axes\":[";

  AxisRuntimeSample samples[kAxisCount] = {};
  for (size_t i = 0; i < kAxisCount; ++i) {
    buildAxisSample(snapshot, config, i, &samples[i]);
    if (i > 0) {
      json += ',';
    }
    json += "{\"id\":";
    appendJsonString(&json, axisDescriptorAt(i).name);
    json += ",\"label\":";
    appendJsonString(&json, axisDescriptorAt(i).label);
    json += ",\"present\":";
    json += samples[i].present ? "true" : "false";
    json += ",\"raw\":";
    json += String(samples[i].raw);
    json += ",\"logicalMin\":";
    json += String(samples[i].logical_min);
    json += ",\"logicalMax\":";
    json += String(samples[i].logical_max);
    json += ",\"normalized\":";
    json += String(samples[i].normalized, 4);
    json += ",\"processed\":";
    json += String(samples[i].processed, 4);
    json += '}';
  }

  json += "],\"buttons\":[";
  for (size_t i = 0; i < kButtonCount; ++i) {
    if (i > 0) {
      json += ',';
    }
    json += (snapshot.has_state &&
             (snapshot.state.buttons & (1UL << static_cast<uint32_t>(i))))
                ? "true"
                : "false";
  }

  json += "],\"channels\":[";
  ChannelOutputState outputs[kPpmChannelCount] = {};
  buildChannelOutputState(snapshot, config, config_revision, outputs);
  bool failsafe_active = false;
  for (size_t i = 0; i < kPpmChannelCount; ++i) {
    failsafe_active = failsafe_active || outputs[i].using_failsafe;
    if (i > 0) {
      json += ',';
    }

    json += "{\"index\":";
    json += String(i + 1);
    json += ",\"mode\":";
    appendJsonString(&json, channelModeName(config.channels[i].mode));
    json += ",\"modeLabel\":";
    appendJsonString(&json, channelModeLabel(config.channels[i].mode));
    json += ",\"source\":";
    const String source_name = channelSourceName(config.channels[i].source_index);
    appendJsonString(&json, source_name.c_str());
    json += ",\"sourceLabel\":";
    const String source_label =
        channelSourceLabel(config.channels[i].source_index);
    appendJsonString(&json, source_label.c_str());
    json += ",\"detailLabel\":";
    const String detail_label = channelDetailLabel(config.channels[i], outputs[i]);
    appendJsonString(&json, detail_label.c_str());
    json += ",\"invert\":";
    json += config.channels[i].invert ? "true" : "false";
    json += ",\"failsafePulse\":";
    json += String(config.channels[i].failsafe_pulse_us);
    json += ",\"usingFailsafe\":";
    json += outputs[i].using_failsafe ? "true" : "false";
    json += ",\"latched\":";
    json += outputs[i].latched ? "true" : "false";
    json += ",\"activeSelectorIndex\":";
    json += String(outputs[i].active_selector_index);
    json += ",\"pulse\":";
    json += String(outputs[i].pulse_us);
    json += '}';
  }
  json += "],\"failsafeActive\":";
  json += failsafe_active ? "true" : "false";
  json += "}";
  return json;
}

void handleConfigGet() {
  AppConfig config = {};
  uint32_t revision = 0;
  copyConfigSnapshot(&config, &revision);
  sendJsonResponse(buildConfigJson(config, isFileSystemReady(), revision));
}

void handleConfigPost() {
  AppConfig config = {};
  uint32_t revision = 0;
  copyConfigSnapshot(&config, &revision);

  const String body = g_web_server.arg("plain");
  if (body.isEmpty() || !parseConfigJson(body, &config)) {
    sendJsonError(400, "Invalid config payload");
    return;
  }

  setConfigSnapshot(config, true);
  const bool storage_ok = saveConfigToFilesystem(config);
  copyConfigSnapshot(&config, &revision);
  sendJsonResponse(buildConfigJson(config, storage_ok, revision));
}

void handleConfigBackup() {
  AppConfig config = {};
  copyConfigSnapshot(&config);

  g_web_server.sendHeader("Cache-Control", "no-store");
  g_web_server.sendHeader(
      "Content-Disposition",
      "attachment; filename=\"esp32-s3-joy-config-backup.json\"");
  g_web_server.send(200, "application/json; charset=utf-8",
                    buildBackupConfigJson(config));
}

void handleConfigRestore() {
  const String body = g_web_server.arg("plain");
  if (body.isEmpty()) {
    sendJsonError(400, "Backup payload missing");
    return;
  }

  String validation_error;
  if (!validateBackupConfigJson(body, &validation_error)) {
    sendJsonError(400, validation_error.c_str());
    return;
  }

  AppConfig restored_config = {};
  resetConfigToDefaults(&restored_config);
  if (!parseConfigJson(body, &restored_config)) {
    sendJsonError(400, "Backup payload invalid");
    return;
  }

  if (!saveConfigToFilesystem(restored_config)) {
    sendJsonError(500, "Backup save failed");
    return;
  }

  setConfigSnapshot(restored_config, true);
  scheduleRestart(700);
  sendJsonResponse("{\"ok\":true,\"rebooting\":true}");
}

void handleConfigZero() {
  DeviceSnapshot snapshot = {};
  AppConfig config = {};
  uint32_t revision = 0;
  copyActiveDeviceSnapshot(&snapshot);
  copyConfigSnapshot(&config, &revision);

  zeroConfigFromCurrentState(snapshot, &config);
  setConfigSnapshot(config, true);
  const bool storage_ok = saveConfigToFilesystem(config);
  copyConfigSnapshot(&config, &revision);
  sendJsonResponse(buildConfigJson(config, storage_ok, revision));
}

void handleConfigReset() {
  AppConfig config = {};
  uint32_t revision = 0;
  resetConfigToDefaults(&config);
  setConfigSnapshot(config, true);
  const bool storage_ok = saveConfigToFilesystem(config);
  copyConfigSnapshot(&config, &revision);
  sendJsonResponse(buildConfigJson(config, storage_ok, revision));
}

void handleStateGet() {
  DeviceSnapshot snapshot = {};
  AnalogInputSnapshot analog_snapshot = {};
  AppConfig config = {};
  uint32_t revision = 0;
  copyActiveDeviceSnapshot(&snapshot);
  copyAnalogInputSnapshot(&analog_snapshot);
  copyConfigSnapshot(&config, &revision);
  sendJsonResponse(
      buildStateJson(snapshot, config, revision, analog_snapshot.sequence));
}

void handleNotFound() {
  if (serveStaticFile(g_web_server.uri())) {
    return;
  }
  g_web_server.send(404, "text/plain; charset=utf-8", "Not found");
}

}  // namespace

void startWebServer() {
  g_web_server.on("/", HTTP_GET, []() {
    if (!serveStaticFile("/index.html")) {
      g_web_server.send(503, "text/plain; charset=utf-8",
                        "Web assets unavailable");
    }
  });
  g_web_server.on("/api/state", HTTP_GET, handleStateGet);
  g_web_server.on("/api/config", HTTP_GET, handleConfigGet);
  g_web_server.on("/api/config", HTTP_POST, handleConfigPost);
  g_web_server.on("/api/config/backup", HTTP_GET, handleConfigBackup);
  g_web_server.on("/api/config/restore", HTTP_POST, handleConfigRestore);
  g_web_server.on("/api/config/zero", HTTP_POST, handleConfigZero);
  g_web_server.on("/api/config/reset", HTTP_POST, handleConfigReset);
  g_web_server.onNotFound(handleNotFound);
  g_web_server.begin();

  const NetworkSnapshot network = copyNetworkSnapshot();
  Serial.printf("Web server ready on http://%s/\n", network.ip.c_str());
}

void pollWebServer() {
  g_web_server.handleClient();
  if (g_reboot_pending &&
      static_cast<int32_t>(millis() - g_reboot_after_ms) >= 0) {
    g_reboot_pending = false;
    Serial.println("Reboot requested after config restore");
    delay(100);
    ESP.restart();
  }
}

}  // namespace app
