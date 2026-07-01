#include "app_types.h"

#include <cmath>
#include <cstring>

#include "app_state.h"
#include "hid.h"

namespace app {

#define APP_STRINGIFY_IMPL(value) #value
#define APP_STRINGIFY(value) APP_STRINGIFY_IMPL(value)

const AxisDescriptor kAxisDescriptors[kAxisCount] = {
    {AxisId::X, ParsedFieldType::AxisX, "x", "X", -1},
    {AxisId::Y, ParsedFieldType::AxisY, "y", "Y", -1},
    {AxisId::Z, ParsedFieldType::AxisZ, "z", "Z", -1},
    {AxisId::Rx, ParsedFieldType::AxisRx, "rx", "Rx", -1},
    {AxisId::Ry, ParsedFieldType::AxisRy, "ry", "Ry", -1},
    {AxisId::Rz, ParsedFieldType::AxisRz, "rz", "Rz", -1},
    {AxisId::Slider, ParsedFieldType::Slider, "slider", "Slider", -1},
    {AxisId::Wheel, ParsedFieldType::Wheel, "wheel", "Wheel", -1},
    {AxisId::Dial, ParsedFieldType::Dial, "dial", "Dial", -1},
    {AxisId::HatX, ParsedFieldType::Hat, "hatx", "Hat X", -1},
    {AxisId::HatY, ParsedFieldType::Hat, "haty", "Hat Y", -1},
#if APP_ANALOG_INPUT_COUNT > 0
    {AxisId::X, ParsedFieldType::Other, "adc1",
     "ADC 1 (GPIO" APP_STRINGIFY(APP_ANALOG_INPUT_1_PIN) ")", 0},
#endif
#if APP_ANALOG_INPUT_COUNT > 1
    {AxisId::X, ParsedFieldType::Other, "adc2",
     "ADC 2 (GPIO" APP_STRINGIFY(APP_ANALOG_INPUT_2_PIN) ")", 1},
#endif
#if APP_ANALOG_INPUT_COUNT > 2
    {AxisId::X, ParsedFieldType::Other, "adc3",
     "ADC 3 (GPIO" APP_STRINGIFY(APP_ANALOG_INPUT_3_PIN) ")", 2},
#endif
#if APP_ANALOG_INPUT_COUNT > 3
    {AxisId::X, ParsedFieldType::Other, "adc4",
     "ADC 4 (GPIO" APP_STRINGIFY(APP_ANALOG_INPUT_4_PIN) ")", 3},
#endif
};

#undef APP_STRINGIFY
#undef APP_STRINGIFY_IMPL

namespace {

constexpr int kPpmHalfRangeUs = kPpmPulseMaxUs - kPpmPulseCenterUs;
static_assert(kPpmPulseCenterUs - kPpmPulseMinUs == kPpmHalfRangeUs,
              "PPM min/max range must stay symmetric around center");
constexpr char kChannelModeSource[] = "source";
constexpr char kChannelModeStepper[] = "stepper";
constexpr char kChannelModeLatch[] = "latch";
constexpr char kChannelModeSelector[] = "selector";

int hatComponentX(const uint8_t hat) {
  switch (hat) {
    case 1:
    case 2:
    case 3:
      return 1;
    case 5:
    case 6:
    case 7:
      return -1;
    default:
      return 0;
  }
}

int hatComponentY(const uint8_t hat) {
  switch (hat) {
    case 0:
    case 1:
    case 7:
      return -1;
    case 3:
    case 4:
    case 5:
      return 1;
    default:
      return 0;
  }
}

bool buildVirtualHatAxisSample(const GamepadState &state,
                               const AxisConfig &config, const AxisId axis_id,
                               AxisRuntimeSample *sample) {
  if (sample == nullptr) {
    return false;
  }

  bool present = false;
  int raw_value = 0;

  if (state.has_hat) {
    raw_value =
        (axis_id == AxisId::HatX) ? hatComponentX(state.hat) : hatComponentY(state.hat);
    present = true;
  } else if (state.has_dpad_up || state.has_dpad_down || state.has_dpad_right ||
             state.has_dpad_left) {
    const int horizontal =
        (state.dpad_right ? 1 : 0) - (state.dpad_left ? 1 : 0);
    const int vertical =
        (state.dpad_down ? 1 : 0) - (state.dpad_up ? 1 : 0);
    raw_value = (axis_id == AxisId::HatX) ? horizontal : vertical;
    present = true;
  }

  if (!present) {
    return true;
  }

  sample->present = true;
  sample->raw = raw_value;
  sample->logical_min = -1;
  sample->logical_max = 1;
  sample->normalized = static_cast<float>(raw_value);
  sample->processed = applyAxisConfigToValue(sample->normalized, config);
  return true;
}

bool buildAnalogAxisSample(const size_t axis_index, const AxisConfig &config,
                           AxisRuntimeSample *sample) {
  if (sample == nullptr || axis_index >= kAxisCount) {
    return false;
  }

  const int analog_input_index =
      axisDescriptorAt(axis_index).analog_input_index;
  if (analog_input_index < 0 ||
      analog_input_index >= static_cast<int>(kAnalogInputCount)) {
    return true;
  }

  AnalogInputSnapshot analog_snapshot = {};
  if (!copyAnalogInputSnapshot(&analog_snapshot)) {
    return true;
  }

  const AnalogInputState &input =
      analog_snapshot.inputs[static_cast<size_t>(analog_input_index)];
  if (!input.present) {
    return true;
  }

  sample->present = true;
  sample->raw = input.raw;
  const bool has_calibration = config.calibration_min < config.calibration_max;
  sample->logical_min =
      has_calibration ? config.calibration_min : input.logical_min;
  sample->logical_max =
      has_calibration ? config.calibration_max : input.logical_max;
  sample->normalized = normalizeAxisValue(input.raw, sample->logical_min,
                                          sample->logical_max);
  sample->processed = applyAxisConfigToValue(sample->normalized, config);
  return true;
}

}  // namespace

const AxisDescriptor &axisDescriptorAt(const size_t index) {
  return kAxisDescriptors[index];
}

int clampInt(const int value, const int minimum, const int maximum) {
  if (value < minimum) {
    return minimum;
  }
  if (value > maximum) {
    return maximum;
  }
  return value;
}

float clampFloat(const float value, const float minimum, const float maximum) {
  if (value < minimum) {
    return minimum;
  }
  if (value > maximum) {
    return maximum;
  }
  return value;
}

int axisIndexFromName(const String &name) {
  for (size_t i = 0; i < kAxisCount; ++i) {
    if (name.equalsIgnoreCase(axisDescriptorAt(i).name)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

ChannelMode channelModeFromName(const String &name) {
  String normalized = name;
  normalized.trim();
  normalized.toLowerCase();

  if (normalized.isEmpty() || normalized == kChannelModeSource) {
    return ChannelMode::Source;
  }
  if (normalized == kChannelModeStepper || normalized == "updown") {
    return ChannelMode::Stepper;
  }
  if (normalized == kChannelModeLatch || normalized == "toggle") {
    return ChannelMode::Latch;
  }
  if (normalized == kChannelModeSelector) {
    return ChannelMode::Selector;
  }
  return ChannelMode::Source;
}

const char *channelModeName(const ChannelMode mode) {
  switch (mode) {
    case ChannelMode::Stepper:
      return kChannelModeStepper;
    case ChannelMode::Latch:
      return kChannelModeLatch;
    case ChannelMode::Selector:
      return kChannelModeSelector;
    case ChannelMode::Source:
    default:
      return kChannelModeSource;
  }
}

const char *channelModeLabel(const ChannelMode mode) {
  switch (mode) {
    case ChannelMode::Stepper:
      return "Step";
    case ChannelMode::Latch:
      return "Latch";
    case ChannelMode::Selector:
      return "Selector";
    case ChannelMode::Source:
    default:
      return "Source";
  }
}

bool isAxisSourceIndex(const int source_index) {
  return source_index >= 0 && source_index < static_cast<int>(kAxisCount);
}

bool isAnalogAxisSourceIndex(const int source_index) {
  return isAxisSourceIndex(source_index) &&
         axisDescriptorAt(static_cast<size_t>(source_index))
                 .analog_input_index >= 0;
}

bool isButtonSourceIndex(const int source_index) {
  return source_index >= kChannelSourceButtonBase &&
         source_index <
             (kChannelSourceButtonBase + static_cast<int>(kButtonCount));
}

int buttonIndexFromSourceIndex(const int source_index) {
  if (!isButtonSourceIndex(source_index)) {
    return -1;
  }
  return source_index - kChannelSourceButtonBase;
}

int channelSourceIndexFromName(const String &name) {
  if (name.equalsIgnoreCase("none")) {
    return kChannelSourceNone;
  }

  const int axis_index = axisIndexFromName(name);
  if (axis_index >= 0) {
    return axis_index;
  }

  String normalized = name;
  normalized.trim();
  normalized.toLowerCase();

  if (normalized.startsWith("button")) {
    const int button_number = normalized.substring(6).toInt();
    if (button_number >= 1 && button_number <= static_cast<int>(kButtonCount)) {
      return kChannelSourceButtonBase + (button_number - 1);
    }
  }

  if (normalized.startsWith("btn")) {
    const int button_number = normalized.substring(3).toInt();
    if (button_number >= 1 && button_number <= static_cast<int>(kButtonCount)) {
      return kChannelSourceButtonBase + (button_number - 1);
    }
  }

  return kChannelSourceNone;
}

String channelSourceName(const int source_index) {
  if (isAxisSourceIndex(source_index)) {
    return String(axisDescriptorAt(static_cast<size_t>(source_index)).name);
  }

  const int button_index = buttonIndexFromSourceIndex(source_index);
  if (button_index >= 0) {
    return String("button") + String(button_index + 1);
  }

  return String("none");
}

String channelSourceLabel(const int source_index) {
  if (isAxisSourceIndex(source_index)) {
    return String(axisDescriptorAt(static_cast<size_t>(source_index)).label);
  }

  const int button_index = buttonIndexFromSourceIndex(source_index);
  if (button_index >= 0) {
    return String("Button ") + String(button_index + 1);
  }

  return String("Nessuno");
}

const char *axisSourceLabel(const int axis_index) {
  if (axis_index < 0 || axis_index >= static_cast<int>(kAxisCount)) {
    return "Nessuno";
  }
  return axisDescriptorAt(static_cast<size_t>(axis_index)).label;
}

bool isButtonPressed(const GamepadState &state, const int source_index) {
  const int button_index = buttonIndexFromSourceIndex(source_index);
  if (button_index < 0) {
    return false;
  }

  const uint32_t mask = 1UL << static_cast<uint32_t>(button_index);
  return (state.buttons & mask) != 0;
}

bool readAxisValue(const GamepadState &state, const AxisId axis_id,
                   int32_t *value) {
  if (value == nullptr) {
    return false;
  }

  switch (axis_id) {
    case AxisId::X:
      if (!state.has_x) {
        return false;
      }
      *value = state.x;
      return true;
    case AxisId::Y:
      if (!state.has_y) {
        return false;
      }
      *value = state.y;
      return true;
    case AxisId::Z:
      if (!state.has_z) {
        return false;
      }
      *value = state.z;
      return true;
    case AxisId::Rx:
      if (!state.has_rx) {
        return false;
      }
      *value = state.rx;
      return true;
    case AxisId::Ry:
      if (!state.has_ry) {
        return false;
      }
      *value = state.ry;
      return true;
    case AxisId::Rz:
      if (!state.has_rz) {
        return false;
      }
      *value = state.rz;
      return true;
    case AxisId::Slider:
      if (!state.has_slider) {
        return false;
      }
      *value = state.slider;
      return true;
    case AxisId::Wheel:
      if (!state.has_wheel) {
        return false;
      }
      *value = state.wheel;
      return true;
    case AxisId::Dial:
      if (!state.has_dial) {
        return false;
      }
      *value = state.dial;
      return true;
    case AxisId::HatX:
    case AxisId::HatY:
      return false;
  }

  return false;
}

const ParsedField *findAxisField(const ParsedLayout &layout,
                                 const AxisId axis_id) {
  const ParsedFieldType target_type =
      axisDescriptorAt(static_cast<size_t>(axis_id)).field_type;
  for (size_t i = 0; i < layout.field_count; ++i) {
    if (layout.fields[i].type == target_type) {
      return &layout.fields[i];
    }
  }
  return nullptr;
}

float normalizeAxisValue(const int32_t raw_value, const int32_t logical_min,
                         const int32_t logical_max) {
  if (logical_min == logical_max) {
    return 0.0f;
  }

  const float min_value = static_cast<float>(logical_min);
  const float max_value = static_cast<float>(logical_max);
  const float center = (min_value + max_value) * 0.5f;
  float half_range = max_value - center;
  const float lower_half = center - min_value;
  if (lower_half > half_range) {
    half_range = lower_half;
  }
  if (half_range < 1.0f) {
    return 0.0f;
  }

  return clampFloat((static_cast<float>(raw_value) - center) / half_range,
                    -1.0f, 1.0f);
}

float applyAxisConfigToValue(const float normalized,
                             const AxisConfig &config) {
  float value =
      clampFloat(normalized + (static_cast<float>(config.trim_percent) / 100.0f),
                 -1.0f, 1.0f);

  const float dead_zone =
      clampFloat(static_cast<float>(config.dead_zone_percent) / 100.0f, 0.0f,
                 0.95f);
  if (std::fabs(value) <= dead_zone) {
    value = 0.0f;
  } else if (dead_zone > 0.0f) {
    const float sign = (value < 0.0f) ? -1.0f : 1.0f;
    value = sign * ((std::fabs(value) - dead_zone) / (1.0f - dead_zone));
  }

  const float expo =
      clampFloat(static_cast<float>(config.expo_percent) / 100.0f, -1.0f,
                 1.0f);
  value = (value * (1.0f - expo)) + ((value * value * value) * expo);
  return clampFloat(value, -1.0f, 1.0f);
}

int clampPpmPulseUs(const int pulse_us) {
  return clampInt(pulse_us, kPpmPulseMinUs, kPpmPulseMaxUs);
}

int ppmPulseFromValue(const float value) {
  return clampPpmPulseUs(
      kPpmPulseCenterUs + static_cast<int>(std::lround(
                               clampFloat(value, -1.0f, 1.0f) * kPpmHalfRangeUs)));
}

void resetChannelRuntimeState(const ChannelConfig &config,
                              ChannelRuntimeState *state) {
  if (state == nullptr) {
    return;
  }

  *state = ChannelRuntimeState{};

  switch (config.mode) {
    case ChannelMode::Stepper: {
      const uint16_t min_pulse_us = static_cast<uint16_t>(
          clampPpmPulseUs(config.stepper.min_pulse_us));
      const uint16_t max_pulse_us = static_cast<uint16_t>(
          clampPpmPulseUs(config.stepper.max_pulse_us));
      const uint16_t lower = (min_pulse_us <= max_pulse_us) ? min_pulse_us
                                                            : max_pulse_us;
      const uint16_t upper = (min_pulse_us <= max_pulse_us) ? max_pulse_us
                                                            : min_pulse_us;
      state->pulse_us = static_cast<uint16_t>(clampInt(
          clampPpmPulseUs(config.stepper.initial_pulse_us), lower, upper));
      break;
    }
    case ChannelMode::Latch:
      state->pulse_us = static_cast<uint16_t>(
          clampPpmPulseUs(config.latch.reset_pulse_us));
      break;
    case ChannelMode::Selector:
      state->pulse_us = static_cast<uint16_t>(
          clampPpmPulseUs(config.selector.default_pulse_us));
      break;
    case ChannelMode::Source:
    default:
      state->pulse_us = static_cast<uint16_t>(
          clampPpmPulseUs(config.failsafe_pulse_us));
      break;
  }
}

bool buildAxisSample(const DeviceSnapshot &snapshot, const AppConfig &config,
                     const size_t axis_index, AxisRuntimeSample *sample) {
  if (sample == nullptr || axis_index >= kAxisCount) {
    return false;
  }

  *sample = AxisRuntimeSample{};
  if (axisDescriptorAt(axis_index).analog_input_index >= 0) {
    return buildAnalogAxisSample(axis_index, config.axes[axis_index], sample);
  }

  if (!snapshot.connected || !snapshot.has_state) {
    return true;
  }

  const AxisId axis_id = axisDescriptorAt(axis_index).id;
  if (axis_id == AxisId::HatX || axis_id == AxisId::HatY) {
    return buildVirtualHatAxisSample(snapshot.state, config.axes[axis_index],
                                     axis_id, sample);
  }

  int32_t raw_value = 0;
  if (!readAxisValue(snapshot.state, axis_id, &raw_value)) {
    return true;
  }

  const ParsedField *field = findAxisField(snapshot.layout, axis_id);
  if (field == nullptr) {
    return true;
  }

  sample->present = true;
  sample->raw = raw_value;
  sample->logical_min = field->logical_min;
  sample->logical_max = field->logical_max;
  sample->normalized =
      normalizeAxisValue(raw_value, field->logical_min, field->logical_max);
  sample->processed = applyAxisConfigToValue(sample->normalized,
                                             config.axes[axis_index]);
  return true;
}

float channelSourceValue(const DeviceSnapshot &snapshot, const AppConfig &config,
                         const int source_index,
                         const AxisRuntimeSample *axis_samples) {
  if (isAxisSourceIndex(source_index)) {
    if (axis_samples != nullptr) {
      return axis_samples[static_cast<size_t>(source_index)].processed;
    }

    AxisRuntimeSample sample = {};
    if (!buildAxisSample(snapshot, config, static_cast<size_t>(source_index),
                         &sample)) {
      return 0.0f;
    }
    return sample.processed;
  }

  if (!snapshot.connected || !snapshot.has_state) {
    return 0.0f;
  }

  const int button_index = buttonIndexFromSourceIndex(source_index);
  if (button_index >= 0) {
    return isButtonPressed(snapshot.state, source_index) ? 1.0f : -1.0f;
  }

  return 0.0f;
}

void zeroConfigFromCurrentState(const DeviceSnapshot &snapshot,
                                AppConfig *config) {
  if (config == nullptr) {
    return;
  }

  for (size_t i = 0; i < kAxisCount; ++i) {
    AxisRuntimeSample sample = {};
    if (!buildAxisSample(snapshot, *config, i, &sample) || !sample.present) {
      continue;
    }

    config->axes[i].trim_percent = static_cast<int8_t>(
        clampInt(static_cast<int>(std::lround(-sample.normalized * 100.0f)),
                 -100, 100));
  }
}

bool statesEqual(const GamepadState &left, const GamepadState &right) {
  return std::memcmp(&left, &right, sizeof(GamepadState)) == 0;
}

const char *protocolName(const uint8_t proto) {
  switch (proto) {
    case HID_PROTOCOL_NONE:
      return "NONE";
    case HID_PROTOCOL_KEYBOARD:
      return "KEYBOARD";
    case HID_PROTOCOL_MOUSE:
      return "MOUSE";
    default:
      return "UNKNOWN";
  }
}

}  // namespace app
