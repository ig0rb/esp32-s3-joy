#include "app_storage.h"

#include <LittleFS.h>

#include <cstring>

namespace app {
namespace {

constexpr char kConfigFilePath[] = "/config.json";
constexpr char kConfigDocumentType[] = "esp32-s3-joy-config";
constexpr uint32_t kConfigSchemaVersion = 3;
constexpr char kConfigBuildInfo[] = __DATE__ " " __TIME__;

bool g_fs_ready = false;

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

void setErrorMessage(String *error_message, const char *message) {
  if (error_message == nullptr) {
    return;
  }
  *error_message = (message != nullptr) ? String(message) : String();
}

void setErrorMessage(String *error_message, const String &message) {
  if (error_message == nullptr) {
    return;
  }
  *error_message = message;
}

bool findMatchingToken(const String &text, const int open_index,
                       const char open_token, const char close_token,
                       int *close_index) {
  if (close_index == nullptr || open_index < 0 || open_index >= text.length() ||
      text[open_index] != open_token) {
    return false;
  }

  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (int i = open_index; i < text.length(); ++i) {
    const char current = text[i];

    if (in_string) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (current == '\\') {
        escaped = true;
        continue;
      }
      if (current == '"') {
        in_string = false;
      }
      continue;
    }

    if (current == '"') {
      in_string = true;
      continue;
    }
    if (current == open_token) {
      ++depth;
      continue;
    }
    if (current == close_token) {
      --depth;
      if (depth == 0) {
        *close_index = i;
        return true;
      }
    }
  }

  return false;
}

bool isJsonWhitespace(const char value) {
  return value == ' ' || value == '\n' || value == '\r' || value == '\t';
}

bool findJsonKeyValueStart(const String &json, const char *key, int *value_index) {
  if (value_index == nullptr || key == nullptr) {
    return false;
  }

  const size_t key_length = std::strlen(key);
  if (key_length == 0) {
    return false;
  }

  bool in_string = false;
  bool escaped = false;
  for (int i = 0; i < json.length(); ++i) {
    const char current = json[i];

    if (in_string) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (current == '\\') {
        escaped = true;
        continue;
      }
      if (current == '"') {
        in_string = false;
      }
      continue;
    }

    if (current != '"') {
      continue;
    }

    bool matches = true;
    for (size_t j = 0; j < key_length; ++j) {
      const int key_char_index = i + 1 + static_cast<int>(j);
      if (key_char_index >= json.length() || json[key_char_index] != key[j]) {
        matches = false;
        break;
      }
    }

    const int closing_quote_index = i + 1 + static_cast<int>(key_length);
    if (!matches || closing_quote_index >= json.length() ||
        json[closing_quote_index] != '"') {
      in_string = true;
      continue;
    }

    int cursor = closing_quote_index + 1;
    while (cursor < json.length() && isJsonWhitespace(json[cursor])) {
      ++cursor;
    }
    if (cursor >= json.length() || json[cursor] != ':') {
      in_string = true;
      continue;
    }

    ++cursor;
    while (cursor < json.length() && isJsonWhitespace(json[cursor])) {
      ++cursor;
    }
    if (cursor >= json.length()) {
      return false;
    }

    *value_index = cursor;
    return true;
  }

  return false;
}

bool findJsonContainer(const String &json, const char *key,
                       const char open_token, const char close_token,
                       int *start_index, int *end_index) {
  if (start_index == nullptr || end_index == nullptr) {
    return false;
  }

  int open_index = 0;
  if (!findJsonKeyValueStart(json, key, &open_index)) {
    return false;
  }
  if (json[open_index] != open_token) {
    return false;
  }

  if (!findMatchingToken(json, open_index, open_token, close_token,
                         end_index)) {
    return false;
  }

  *start_index = open_index;
  return true;
}

bool extractJsonContainerValue(const String &json, const char *key,
                               const char open_token,
                               const char close_token, String *value) {
  if (value == nullptr) {
    return false;
  }

  int start_index = 0;
  int end_index = 0;
  if (!findJsonContainer(json, key, open_token, close_token, &start_index,
                         &end_index)) {
    return false;
  }

  *value = json.substring(start_index, end_index + 1);
  return true;
}

size_t collectTopLevelObjects(const String &array_json, String *objects,
                              const size_t max_objects) {
  if (objects == nullptr || max_objects == 0) {
    return 0;
  }

  size_t count = 0;
  int object_start = -1;
  int depth = 0;
  bool in_string = false;
  bool escaped = false;

  for (int i = 0; i < array_json.length(); ++i) {
    const char current = array_json[i];

    if (in_string) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (current == '\\') {
        escaped = true;
        continue;
      }
      if (current == '"') {
        in_string = false;
      }
      continue;
    }

    if (current == '"') {
      in_string = true;
      continue;
    }
    if (current == '{') {
      if (depth == 0) {
        object_start = i;
      }
      ++depth;
      continue;
    }
    if (current == '}') {
      --depth;
      if (depth == 0 && object_start >= 0) {
        if (count < max_objects) {
          objects[count++] = array_json.substring(object_start, i + 1);
        }
        object_start = -1;
      }
    }
  }

  return count;
}

bool extractJsonInt(const String &json, const char *key, int *value) {
  if (value == nullptr) {
    return false;
  }

  int cursor = 0;
  if (!findJsonKeyValueStart(json, key, &cursor)) {
    return false;
  }

  int end = cursor;
  if (end < json.length() && json[end] == '-') {
    ++end;
  }
  while (end < json.length() && isDigit(json[end])) {
    ++end;
  }
  if (end == cursor || (end == cursor + 1 && json[cursor] == '-')) {
    return false;
  }

  *value = json.substring(cursor, end).toInt();
  return true;
}

bool extractJsonBool(const String &json, const char *key, bool *value) {
  if (value == nullptr) {
    return false;
  }

  int value_index = 0;
  if (!findJsonKeyValueStart(json, key, &value_index)) {
    return false;
  }

  if (json.startsWith("true", value_index)) {
    *value = true;
    return true;
  }
  if (json.startsWith("false", value_index)) {
    *value = false;
    return true;
  }

  return false;
}

bool extractJsonString(const String &json, const char *key, String *value) {
  if (value == nullptr) {
    return false;
  }

  int first_quote = 0;
  if (!findJsonKeyValueStart(json, key, &first_quote)) {
    return false;
  }
  if (json[first_quote] != '"') {
    return false;
  }

  bool escaped = false;
  for (int i = first_quote + 1; i < json.length(); ++i) {
    const char current = json[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (current == '\\') {
      escaped = true;
      continue;
    }
    if (current == '"') {
      *value = json.substring(first_quote + 1, i);
      value->replace("\\\"", "\"");
      value->replace("\\\\", "\\");
      return true;
    }
  }

  return false;
}

String configCompatibilityTag() {
  String tag;
  tag.reserve(64);
  tag += "axes=";
  tag += String(kAxisCount);
  tag += ";analog=";
  tag += String(kAnalogInputCount);
  tag += ";buttons=";
  tag += String(kButtonCount);
  tag += ";ppm=";
  tag += String(kPpmChannelCount);
  tag += ";selector=";
  tag += String(kChannelSelectorEntryCount);
  return tag;
}

bool extractConfigPayloadJson(const String &json, String *config_json) {
  if (config_json == nullptr) {
    return false;
  }

  String wrapped_payload;
  if (extractJsonContainerValue(json, "config", '{', '}', &wrapped_payload)) {
    *config_json = wrapped_payload;
    return true;
  }

  *config_json = json;
  return true;
}

bool validateConfigDocumentCompatibility(const String &json,
                                         const bool require_metadata,
                                         String *error_message) {
  const String expected_tag = configCompatibilityTag();

  String document_type;
  const bool has_document_type =
      extractJsonString(json, "type", &document_type);
  int schema_version = 0;
  const bool has_schema_version =
      extractJsonInt(json, "schemaVersion", &schema_version);
  String compatibility_tag;
  const bool has_compatibility_tag =
      extractJsonString(json, "compatibilityTag", &compatibility_tag);

  if (!has_document_type && !has_schema_version && !has_compatibility_tag) {
    if (require_metadata) {
      setErrorMessage(error_message,
                      "Backup metadata missing: expected type/schemaVersion/"
                      "compatibilityTag");
      return false;
    }
    return true;
  }

  if (!has_document_type || document_type != kConfigDocumentType) {
    setErrorMessage(error_message, "Backup type not recognized");
    return false;
  }

  if (!has_schema_version) {
    setErrorMessage(error_message, "Backup missing schemaVersion");
    return false;
  }
  const bool schema_version_supported =
      schema_version == static_cast<int>(kConfigSchemaVersion) ||
      (!require_metadata && schema_version > 0 &&
       schema_version < static_cast<int>(kConfigSchemaVersion));
  if (!schema_version_supported) {
    String message = "Backup schemaVersion mismatch: expected ";
    message += String(kConfigSchemaVersion);
    message += ", found ";
    message += String(schema_version);
    setErrorMessage(error_message, message);
    return false;
  }

  if (!has_compatibility_tag) {
    setErrorMessage(error_message, "Backup missing compatibilityTag");
    return false;
  }
  if (compatibility_tag != expected_tag) {
    String message = "Backup compatibilityTag mismatch: expected ";
    message += expected_tag;
    message += ", found ";
    message += compatibility_tag;
    setErrorMessage(error_message, message);
    return false;
  }

  return true;
}

void appendConfigPayloadJson(String *json, const AppConfig &config) {
  if (json == nullptr) {
    return;
  }

  *json += "{\"axes\":[";
  for (size_t i = 0; i < kAxisCount; ++i) {
    if (i > 0) {
      *json += ',';
    }
    *json += "{\"id\":";
    appendJsonString(json, axisDescriptorAt(i).name);
    *json += ",\"trim\":";
    *json += String(config.axes[i].trim_percent);
    *json += ",\"deadZone\":";
    *json += String(config.axes[i].dead_zone_percent);
    *json += ",\"expo\":";
    *json += String(config.axes[i].expo_percent);
    *json += ",\"calibrationMin\":";
    *json += String(config.axes[i].calibration_min);
    *json += ",\"calibrationMax\":";
    *json += String(config.axes[i].calibration_max);
    *json += '}';
  }
  *json += "],\"channels\":[";
  for (size_t i = 0; i < kPpmChannelCount; ++i) {
    if (i > 0) {
      *json += ',';
    }
    *json += "{\"index\":";
    *json += String(i + 1);
    *json += ",\"mode\":";
    appendJsonString(json, channelModeName(config.channels[i].mode));
    *json += ",\"source\":";
    const String source_name = channelSourceName(config.channels[i].source_index);
    appendJsonString(json, source_name.c_str());
    *json += ",\"invert\":";
    *json += config.channels[i].invert ? "true" : "false";
    *json += ",\"failsafePulse\":";
    *json += String(config.channels[i].failsafe_pulse_us);
    *json += ",\"stepper\":{\"upButton\":";
    const String up_button_name =
        channelSourceName(config.channels[i].stepper.up_source_index);
    appendJsonString(json, up_button_name.c_str());
    *json += ",\"downButton\":";
    const String down_button_name =
        channelSourceName(config.channels[i].stepper.down_source_index);
    appendJsonString(json, down_button_name.c_str());
    *json += ",\"stepPulse\":";
    *json += String(config.channels[i].stepper.step_pulse_us);
    *json += ",\"minPulse\":";
    *json += String(config.channels[i].stepper.min_pulse_us);
    *json += ",\"maxPulse\":";
    *json += String(config.channels[i].stepper.max_pulse_us);
    *json += ",\"initialPulse\":";
    *json += String(config.channels[i].stepper.initial_pulse_us);
    *json += "},\"latch\":{\"button\":";
    const String latch_button_name =
        channelSourceName(config.channels[i].latch.button_source_index);
    appendJsonString(json, latch_button_name.c_str());
    *json += ",\"activePulse\":";
    *json += String(config.channels[i].latch.active_pulse_us);
    *json += ",\"resetPulse\":";
    *json += String(config.channels[i].latch.reset_pulse_us);
    *json += "},\"selector\":{\"defaultPulse\":";
    *json += String(config.channels[i].selector.default_pulse_us);
    *json += ",\"entries\":[";
    const size_t entry_count =
        (config.channels[i].selector.entry_count <=
         static_cast<uint8_t>(kChannelSelectorEntryCount))
            ? config.channels[i].selector.entry_count
            : kChannelSelectorEntryCount;
    for (size_t entry_index = 0; entry_index < entry_count; ++entry_index) {
      if (entry_index > 0) {
        *json += ',';
      }
      *json += "{\"button\":";
      const String entry_button_name = channelSourceName(
          config.channels[i].selector.entries[entry_index].button_source_index);
      appendJsonString(json, entry_button_name.c_str());
      *json += ",\"pulse\":";
      *json += String(config.channels[i].selector.entries[entry_index].pulse_us);
      *json += '}';
    }
    *json += "]}";
    *json += '}';
  }
  *json += "]}";
}

bool isValidChannelSourceIndex(const int source_index) {
  return source_index == kChannelSourceNone || isAxisSourceIndex(source_index) ||
         isButtonSourceIndex(source_index);
}

bool isValidButtonSourceOrNone(const int source_index) {
  return source_index == kChannelSourceNone || isButtonSourceIndex(source_index);
}

void sanitizeChannelConfig(ChannelConfig *channel) {
  if (channel == nullptr) {
    return;
  }

  if (!isValidChannelSourceIndex(channel->source_index)) {
    channel->source_index = static_cast<int8_t>(kChannelSourceNone);
  }
  channel->failsafe_pulse_us =
      static_cast<uint16_t>(clampPpmPulseUs(channel->failsafe_pulse_us));

  if (!isValidButtonSourceOrNone(channel->stepper.up_source_index)) {
    channel->stepper.up_source_index = static_cast<int8_t>(kChannelSourceNone);
  }
  if (!isValidButtonSourceOrNone(channel->stepper.down_source_index)) {
    channel->stepper.down_source_index = static_cast<int8_t>(kChannelSourceNone);
  }
  channel->stepper.step_pulse_us = static_cast<uint16_t>(clampInt(
      static_cast<int>(channel->stepper.step_pulse_us), 1,
      kPpmPulseMaxUs - kPpmPulseMinUs));
  int stepper_min = clampPpmPulseUs(channel->stepper.min_pulse_us);
  int stepper_max = clampPpmPulseUs(channel->stepper.max_pulse_us);
  if (stepper_min > stepper_max) {
    const int swap = stepper_min;
    stepper_min = stepper_max;
    stepper_max = swap;
  }
  channel->stepper.min_pulse_us = static_cast<uint16_t>(stepper_min);
  channel->stepper.max_pulse_us = static_cast<uint16_t>(stepper_max);
  channel->stepper.initial_pulse_us = static_cast<uint16_t>(clampInt(
      clampPpmPulseUs(channel->stepper.initial_pulse_us), stepper_min,
      stepper_max));

  if (!isValidButtonSourceOrNone(channel->latch.button_source_index)) {
    channel->latch.button_source_index = static_cast<int8_t>(kChannelSourceNone);
  }
  channel->latch.active_pulse_us =
      static_cast<uint16_t>(clampPpmPulseUs(channel->latch.active_pulse_us));
  channel->latch.reset_pulse_us =
      static_cast<uint16_t>(clampPpmPulseUs(channel->latch.reset_pulse_us));

  channel->selector.default_pulse_us = static_cast<uint16_t>(
      clampPpmPulseUs(channel->selector.default_pulse_us));
  if (channel->selector.entry_count > kChannelSelectorEntryCount) {
    channel->selector.entry_count =
        static_cast<uint8_t>(kChannelSelectorEntryCount);
  }
  for (size_t i = 0; i < kChannelSelectorEntryCount; ++i) {
    ChannelSelectorEntryConfig *entry = &channel->selector.entries[i];
    if (!isValidButtonSourceOrNone(entry->button_source_index)) {
      entry->button_source_index = static_cast<int8_t>(kChannelSourceNone);
    }
    entry->pulse_us = static_cast<uint16_t>(clampPpmPulseUs(entry->pulse_us));
    if (i >= channel->selector.entry_count) {
      *entry = ChannelSelectorEntryConfig{};
    }
  }
}

}  // namespace

bool startFileSystem() {
  g_fs_ready = LittleFS.begin(true);
  if (!g_fs_ready) {
    Serial.println("LittleFS mount failed");
    return false;
  }

  Serial.println("LittleFS ready");
  return true;
}

bool isFileSystemReady() {
  return g_fs_ready;
}

bool configFileExists() {
  return g_fs_ready && LittleFS.exists(kConfigFilePath);
}

bool parseConfigJson(const String &json, AppConfig *config) {
  if (config == nullptr) {
    return false;
  }

  String payload_json;
  if (!extractConfigPayloadJson(json, &payload_json)) {
    return false;
  }

  AppConfig next = *config;
  bool found_payload = false;

  int axes_start = 0;
  int axes_end = 0;
  if (findJsonContainer(payload_json, "axes", '[', ']', &axes_start,
                        &axes_end)) {
    found_payload = true;
    String axis_objects[kAxisCount] = {};
    const size_t axis_count = collectTopLevelObjects(
        payload_json.substring(axes_start, axes_end + 1), axis_objects,
        kAxisCount);

    for (size_t i = 0; i < axis_count; ++i) {
      String axis_id;
      if (!extractJsonString(axis_objects[i], "id", &axis_id)) {
        continue;
      }
      const int axis_index = axisIndexFromName(axis_id);
      if (axis_index < 0) {
        continue;
      }

      int trim = next.axes[axis_index].trim_percent;
      int dead_zone = next.axes[axis_index].dead_zone_percent;
      int expo = next.axes[axis_index].expo_percent;
      int calibration_min = next.axes[axis_index].calibration_min;
      int calibration_max = next.axes[axis_index].calibration_max;
      extractJsonInt(axis_objects[i], "trim", &trim);
      extractJsonInt(axis_objects[i], "deadZone", &dead_zone);
      extractJsonInt(axis_objects[i], "expo", &expo);
      extractJsonInt(axis_objects[i], "calibrationMin", &calibration_min);
      extractJsonInt(axis_objects[i], "calibrationMax", &calibration_max);

      next.axes[axis_index].trim_percent =
          static_cast<int8_t>(clampInt(trim, -100, 100));
      next.axes[axis_index].dead_zone_percent =
          static_cast<uint8_t>(clampInt(dead_zone, 0, 95));
      next.axes[axis_index].expo_percent =
          static_cast<int8_t>(clampInt(expo, -100, 100));
      if (calibration_min < calibration_max) {
        next.axes[axis_index].calibration_min = calibration_min;
        next.axes[axis_index].calibration_max = calibration_max;
      } else if (calibration_max < calibration_min) {
        next.axes[axis_index].calibration_min = calibration_max;
        next.axes[axis_index].calibration_max = calibration_min;
      } else {
        next.axes[axis_index].calibration_min = 0;
        next.axes[axis_index].calibration_max = 0;
      }
    }
  }

  int channels_start = 0;
  int channels_end = 0;
  if (findJsonContainer(payload_json, "channels", '[', ']', &channels_start,
                        &channels_end)) {
    found_payload = true;
    String channel_objects[kPpmChannelCount] = {};
    const size_t channel_count = collectTopLevelObjects(
        payload_json.substring(channels_start, channels_end + 1), channel_objects,
        kPpmChannelCount);

    for (size_t i = 0; i < channel_count; ++i) {
      ChannelConfig channel = next.channels[i];
      String mode_name;
      if (extractJsonString(channel_objects[i], "mode", &mode_name)) {
        channel.mode = channelModeFromName(mode_name);
      }

      String source;
      if (extractJsonString(channel_objects[i], "source", &source)) {
        channel.source_index =
            static_cast<int8_t>(channelSourceIndexFromName(source));
      }

      bool invert = channel.invert;
      int failsafe_pulse = channel.failsafe_pulse_us;
      extractJsonBool(channel_objects[i], "invert", &invert);
      extractJsonInt(channel_objects[i], "failsafePulse", &failsafe_pulse);
      channel.invert = invert;
      channel.failsafe_pulse_us =
          static_cast<uint16_t>(clampPpmPulseUs(failsafe_pulse));

      String stepper_json;
      if (extractJsonContainerValue(channel_objects[i], "stepper", '{', '}',
                                    &stepper_json)) {
        String up_button;
        if (extractJsonString(stepper_json, "upButton", &up_button)) {
          channel.stepper.up_source_index =
              static_cast<int8_t>(channelSourceIndexFromName(up_button));
        }

        String down_button;
        if (extractJsonString(stepper_json, "downButton", &down_button)) {
          channel.stepper.down_source_index =
              static_cast<int8_t>(channelSourceIndexFromName(down_button));
        }

        int step_pulse = channel.stepper.step_pulse_us;
        int min_pulse = channel.stepper.min_pulse_us;
        int max_pulse = channel.stepper.max_pulse_us;
        int initial_pulse = channel.stepper.initial_pulse_us;
        extractJsonInt(stepper_json, "stepPulse", &step_pulse);
        extractJsonInt(stepper_json, "minPulse", &min_pulse);
        extractJsonInt(stepper_json, "maxPulse", &max_pulse);
        extractJsonInt(stepper_json, "initialPulse", &initial_pulse);
        channel.stepper.step_pulse_us = static_cast<uint16_t>(step_pulse);
        channel.stepper.min_pulse_us = static_cast<uint16_t>(min_pulse);
        channel.stepper.max_pulse_us = static_cast<uint16_t>(max_pulse);
        channel.stepper.initial_pulse_us =
            static_cast<uint16_t>(initial_pulse);
      }

      String latch_json;
      if (extractJsonContainerValue(channel_objects[i], "latch", '{', '}',
                                    &latch_json)) {
        String button_name;
        if (extractJsonString(latch_json, "button", &button_name)) {
          channel.latch.button_source_index =
              static_cast<int8_t>(channelSourceIndexFromName(button_name));
        }

        int active_pulse = channel.latch.active_pulse_us;
        int reset_pulse = channel.latch.reset_pulse_us;
        extractJsonInt(latch_json, "activePulse", &active_pulse);
        extractJsonInt(latch_json, "resetPulse", &reset_pulse);
        channel.latch.active_pulse_us = static_cast<uint16_t>(active_pulse);
        channel.latch.reset_pulse_us = static_cast<uint16_t>(reset_pulse);
      }

      String selector_json;
      if (extractJsonContainerValue(channel_objects[i], "selector", '{', '}',
                                    &selector_json)) {
        int default_pulse = channel.selector.default_pulse_us;
        extractJsonInt(selector_json, "defaultPulse", &default_pulse);
        channel.selector.default_pulse_us =
            static_cast<uint16_t>(default_pulse);

        String entries_json;
        if (extractJsonContainerValue(selector_json, "entries", '[', ']',
                                      &entries_json)) {
          String entry_objects[kChannelSelectorEntryCount] = {};
          const size_t entry_count = collectTopLevelObjects(
              entries_json, entry_objects, kChannelSelectorEntryCount);
          channel.selector.entry_count = static_cast<uint8_t>(entry_count);
          for (size_t entry_index = 0; entry_index < kChannelSelectorEntryCount;
               ++entry_index) {
            channel.selector.entries[entry_index] = ChannelSelectorEntryConfig{};
          }
          for (size_t entry_index = 0; entry_index < entry_count;
               ++entry_index) {
            String button_name;
            if (extractJsonString(entry_objects[entry_index], "button",
                                  &button_name)) {
              channel.selector.entries[entry_index].button_source_index =
                  static_cast<int8_t>(channelSourceIndexFromName(button_name));
            }

            int pulse_us = channel.selector.entries[entry_index].pulse_us;
            extractJsonInt(entry_objects[entry_index], "pulse", &pulse_us);
            channel.selector.entries[entry_index].pulse_us =
                static_cast<uint16_t>(pulse_us);
          }
        }
      }

      sanitizeChannelConfig(&channel);
      next.channels[i] = channel;
    }
  }

  for (size_t i = 0; i < kPpmChannelCount; ++i) {
    sanitizeChannelConfig(&next.channels[i]);
  }

  if (!found_payload) {
    return false;
  }

  *config = next;
  return true;
}

String buildBackupConfigJson(const AppConfig &config) {
  String json;
  json.reserve(4608);
  json += "{\"type\":";
  appendJsonString(&json, kConfigDocumentType);
  json += ",\"schemaVersion\":";
  json += String(kConfigSchemaVersion);
  json += ",\"compatibilityTag\":";
  const String compatibility_tag = configCompatibilityTag();
  appendJsonString(&json, compatibility_tag.c_str());
  json += ",\"buildInfo\":";
  appendJsonString(&json, kConfigBuildInfo);
  json += ",\"config\":";
  appendConfigPayloadJson(&json, config);
  json += '}';
  return json;
}

bool validateBackupConfigJson(const String &json, String *error_message) {
  return validateConfigDocumentCompatibility(json, true, error_message);
}

bool saveConfigToFilesystem(const AppConfig &config) {
  if (!g_fs_ready) {
    return false;
  }

  const String json = buildBackupConfigJson(config);

  File file = LittleFS.open(kConfigFilePath, "w");
  if (!file) {
    return false;
  }

  const size_t written = file.print(json);
  file.close();
  return written == json.length();
}

bool loadConfigFromFilesystem(AppConfig *config) {
  if (config == nullptr || !g_fs_ready || !LittleFS.exists(kConfigFilePath)) {
    return false;
  }

  File file = LittleFS.open(kConfigFilePath, "r");
  if (!file) {
    return false;
  }

  const String json = file.readString();
  file.close();
  String error_message;
  if (!validateConfigDocumentCompatibility(json, false, &error_message)) {
    Serial.printf("Config load refused: %s\n", error_message.c_str());
    return false;
  }
  return parseConfigJson(json, config);
}

}  // namespace app
