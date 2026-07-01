#pragma once

#include <Arduino.h>

#include <cstddef>
#include <cstdint>

#include "app_build_config.h"

namespace app {

constexpr size_t kMaxParsedFields = 64;
constexpr size_t kButtonCount = 32;
constexpr size_t kGamepadAxisCount = 11;
constexpr size_t kAnalogInputMaxCount = 4;
constexpr size_t kAnalogInputCount = APP_ANALOG_INPUT_COUNT;
constexpr size_t kAnalogInputStorageCount =
    (kAnalogInputCount > 0) ? kAnalogInputCount : 1;
constexpr size_t kAxisCount = kGamepadAxisCount + kAnalogInputCount;
constexpr size_t kPpmChannelCount = APP_PPM_CHANNEL_COUNT;
constexpr size_t kChannelSelectorEntryCount = 8;
constexpr int kChannelSourceNone = -1;
constexpr int kChannelSourceButtonBase = static_cast<int>(kAxisCount);
constexpr int kPpmPulseMinUs = 900;
constexpr int kPpmPulseCenterUs = 1500;
constexpr int kPpmPulseMaxUs = 2100;

static_assert(kAnalogInputCount <= kAnalogInputMaxCount,
              "APP_ANALOG_INPUT_COUNT exceeds supported analog input slots");

enum class ParsedFieldType : uint8_t {
  Other = 0,
  AxisX,
  AxisY,
  AxisZ,
  AxisRx,
  AxisRy,
  AxisRz,
  Slider,
  Dial,
  Wheel,
  Hat,
  DpadUp,
  DpadDown,
  DpadRight,
  DpadLeft,
  Button,
};

struct ParsedField {
  ParsedFieldType type = ParsedFieldType::Other;
  uint16_t usage_page = 0;
  uint16_t usage = 0;
  uint16_t bit_offset = 0;
  uint8_t bit_size = 0;
  int32_t logical_min = 0;
  int32_t logical_max = 0;
  uint8_t report_id = 0;
};

struct ParsedLayout {
  ParsedField fields[kMaxParsedFields] = {};
  size_t field_count = 0;
  bool has_report_id = false;
  bool valid = false;
};

struct GamepadState {
  bool has_x = false;
  bool has_y = false;
  bool has_z = false;
  bool has_rx = false;
  bool has_ry = false;
  bool has_rz = false;
  bool has_slider = false;
  bool has_dial = false;
  bool has_wheel = false;
  bool has_hat = false;
  bool has_dpad_up = false;
  bool has_dpad_down = false;
  bool has_dpad_right = false;
  bool has_dpad_left = false;
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
  int32_t rx = 0;
  int32_t ry = 0;
  int32_t rz = 0;
  int32_t slider = 0;
  int32_t dial = 0;
  int32_t wheel = 0;
  uint8_t hat = 0xFF;
  bool dpad_up = false;
  bool dpad_down = false;
  bool dpad_right = false;
  bool dpad_left = false;
  uint32_t buttons = 0;
};

enum class AxisId : uint8_t {
  X = 0,
  Y,
  Z,
  Rx,
  Ry,
  Rz,
  Slider,
  Wheel,
  Dial,
  HatX,
  HatY,
};

struct AxisDescriptor {
  AxisId id;
  ParsedFieldType field_type;
  const char *name;
  const char *label;
  int8_t analog_input_index = -1;
};

extern const AxisDescriptor kAxisDescriptors[kAxisCount];

struct AxisConfig {
  int8_t trim_percent = 0;
  uint8_t dead_zone_percent = 0;
  int8_t expo_percent = 0;
  int32_t calibration_min = 0;
  int32_t calibration_max = 0;
};

enum class ChannelMode : uint8_t {
  Source = 0,
  Stepper,
  Latch,
  Selector,
};

struct ChannelStepperConfig {
  int8_t up_source_index = kChannelSourceNone;
  int8_t down_source_index = kChannelSourceNone;
  uint16_t step_pulse_us = 25;
  uint16_t min_pulse_us = kPpmPulseMinUs;
  uint16_t max_pulse_us = kPpmPulseMaxUs;
  uint16_t initial_pulse_us = kPpmPulseCenterUs;
};

struct ChannelLatchConfig {
  int8_t button_source_index = kChannelSourceNone;
  uint16_t active_pulse_us = kPpmPulseMaxUs;
  uint16_t reset_pulse_us = kPpmPulseCenterUs;
};

struct ChannelSelectorEntryConfig {
  int8_t button_source_index = kChannelSourceNone;
  uint16_t pulse_us = kPpmPulseCenterUs;
};

struct ChannelSelectorConfig {
  uint16_t default_pulse_us = kPpmPulseCenterUs;
  uint8_t entry_count = 0;
  ChannelSelectorEntryConfig entries[kChannelSelectorEntryCount] = {};
};

struct ChannelConfig {
  ChannelMode mode = ChannelMode::Source;
  int8_t source_index = kChannelSourceNone;
  bool invert = false;
  uint16_t failsafe_pulse_us = kPpmPulseCenterUs;
  ChannelStepperConfig stepper = {};
  ChannelLatchConfig latch = {};
  ChannelSelectorConfig selector = {};
};

struct AppConfig {
  AxisConfig axes[kAxisCount] = {};
  ChannelConfig channels[kPpmChannelCount] = {};
};

struct DeviceSnapshot {
  bool connected = false;
  uint8_t address = 0;
  uint8_t interface_number = 0;
  const char *protocol = "NONE";
  uint32_t sequence = 0;
  ParsedLayout layout = {};
  GamepadState state = {};
  bool has_state = false;
};

struct AnalogInputState {
  bool present = false;
  int32_t raw = 0;
  int32_t logical_min = 0;
  int32_t logical_max = 0;
};

struct AnalogInputSnapshot {
  uint32_t sequence = 0;
  AnalogInputState inputs[kAnalogInputStorageCount] = {};
};

struct AxisRuntimeSample {
  bool present = false;
  int32_t raw = 0;
  int32_t logical_min = 0;
  int32_t logical_max = 0;
  float normalized = 0.0f;
  float processed = 0.0f;
};

struct ChannelRuntimeState {
  uint16_t pulse_us = kPpmPulseCenterUs;
  bool latched = false;
  int8_t active_selector_index = -1;
};

struct ChannelOutputState {
  ChannelMode mode = ChannelMode::Source;
  int8_t source_index = kChannelSourceNone;
  bool invert = false;
  bool using_failsafe = false;
  uint16_t failsafe_pulse_us = kPpmPulseCenterUs;
  uint16_t pulse_us = kPpmPulseCenterUs;
  bool latched = false;
  int8_t active_selector_index = -1;
};

const AxisDescriptor &axisDescriptorAt(size_t index);
int clampInt(int value, int minimum, int maximum);
float clampFloat(float value, float minimum, float maximum);
int axisIndexFromName(const String &name);
ChannelMode channelModeFromName(const String &name);
const char *channelModeName(ChannelMode mode);
const char *channelModeLabel(ChannelMode mode);
bool isAxisSourceIndex(int source_index);
bool isAnalogAxisSourceIndex(int source_index);
bool isButtonSourceIndex(int source_index);
int buttonIndexFromSourceIndex(int source_index);
int channelSourceIndexFromName(const String &name);
String channelSourceName(int source_index);
String channelSourceLabel(int source_index);
const char *axisSourceLabel(int axis_index);
bool isButtonPressed(const GamepadState &state, int source_index);
bool readAxisValue(const GamepadState &state, AxisId axis_id, int32_t *value);
const ParsedField *findAxisField(const ParsedLayout &layout, AxisId axis_id);
float normalizeAxisValue(int32_t raw_value, int32_t logical_min,
                         int32_t logical_max);
float applyAxisConfigToValue(float normalized, const AxisConfig &config);
int clampPpmPulseUs(int pulse_us);
int ppmPulseFromValue(float value);
void resetChannelRuntimeState(const ChannelConfig &config,
                              ChannelRuntimeState *state);
bool buildAxisSample(const DeviceSnapshot &snapshot, const AppConfig &config,
                     size_t axis_index, AxisRuntimeSample *sample);
float channelSourceValue(const DeviceSnapshot &snapshot, const AppConfig &config,
                         int source_index,
                         const AxisRuntimeSample *axis_samples = nullptr);
void zeroConfigFromCurrentState(const DeviceSnapshot &snapshot,
                                AppConfig *config);
bool statesEqual(const GamepadState &left, const GamepadState &right);
const char *protocolName(uint8_t proto);

}  // namespace app
