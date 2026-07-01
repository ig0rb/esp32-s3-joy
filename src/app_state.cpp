#include "app_state.h"

#include <cstring>

#include "app_storage.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace app {
namespace {

constexpr size_t kMaxDevices = 8;

struct DeviceContext {
  bool in_use = false;
  hid_host_device_handle_t handle = nullptr;
  hid_host_dev_params_t params = {};
  ParsedLayout layout = {};
  GamepadState last_state = {};
  bool has_last_state = false;
};

struct ChannelLogicContext {
  ChannelRuntimeState channels[kPpmChannelCount] = {};
  uint32_t last_sequence = 0;
  uint32_t last_config_revision = 0;
  uint32_t last_buttons_mask = 0;
  bool initialized = false;
  bool device_active = false;
};

const DeviceContext kEmptyDeviceContext = DeviceContext();
const AppConfig kEmptyAppConfig = AppConfig();
const DeviceSnapshot kEmptyDeviceSnapshot = DeviceSnapshot();
const AnalogInputSnapshot kEmptyAnalogInputSnapshot = AnalogInputSnapshot();

SemaphoreHandle_t g_state_mutex = nullptr;
DeviceContext g_devices[kMaxDevices] = {};
AppConfig g_app_config = {};
AnalogInputSnapshot g_analog_inputs = {};
ChannelLogicContext g_channel_logic = {};
uint32_t g_state_sequence = 0;
uint32_t g_analog_input_sequence = 0;
uint32_t g_config_revision = 0;

template <typename Callback>
void withStateLock(Callback callback) {
  if (g_state_mutex == nullptr) {
    callback();
    return;
  }

  while (xSemaphoreTake(g_state_mutex, portMAX_DELAY) != pdTRUE) {
  }
  callback();
  xSemaphoreGive(g_state_mutex);
}

DeviceContext *findDeviceContext(const hid_host_device_handle_t handle) {
  for (auto &device : g_devices) {
    if (device.in_use && device.handle == handle) {
      return &device;
    }
  }
  return nullptr;
}

bool snapshotHasLiveState(const DeviceSnapshot &snapshot) {
  return snapshot.connected && snapshot.has_state;
}

bool analogSnapshotsEqual(const AnalogInputSnapshot &left,
                          const AnalogInputSnapshot &right) {
  return std::memcmp(left.inputs, right.inputs, sizeof(left.inputs)) == 0;
}

bool buttonRisingEdge(const uint32_t previous_buttons,
                      const uint32_t current_buttons,
                      const int source_index) {
  const int button_index = buttonIndexFromSourceIndex(source_index);
  if (button_index < 0) {
    return false;
  }

  const uint32_t mask = 1UL << static_cast<uint32_t>(button_index);
  return ((current_buttons & mask) != 0) && ((previous_buttons & mask) == 0);
}

void resetChannelLogicState(const AppConfig &config, const uint32_t buttons_mask) {
  for (size_t i = 0; i < kPpmChannelCount; ++i) {
    resetChannelRuntimeState(config.channels[i], &g_channel_logic.channels[i]);
  }
  g_channel_logic.last_buttons_mask = buttons_mask;
  g_channel_logic.initialized = true;
}

void updateStepperChannelRuntime(const ChannelConfig &config,
                                 const uint32_t previous_buttons,
                                 const uint32_t current_buttons,
                                 ChannelRuntimeState *state) {
  if (state == nullptr) {
    return;
  }

  const int min_pulse = clampPpmPulseUs(config.stepper.min_pulse_us);
  const int max_pulse = clampPpmPulseUs(config.stepper.max_pulse_us);
  const int lower = (min_pulse <= max_pulse) ? min_pulse : max_pulse;
  const int upper = (min_pulse <= max_pulse) ? max_pulse : min_pulse;
  const int step_pulse =
      clampInt(static_cast<int>(config.stepper.step_pulse_us), 1,
               kPpmPulseMaxUs - kPpmPulseMinUs);

  int pulse = clampInt(static_cast<int>(state->pulse_us), lower, upper);
  if (buttonRisingEdge(previous_buttons, current_buttons,
                       config.stepper.up_source_index)) {
    pulse += step_pulse;
  }
  if (buttonRisingEdge(previous_buttons, current_buttons,
                       config.stepper.down_source_index)) {
    pulse -= step_pulse;
  }

  state->pulse_us = static_cast<uint16_t>(clampInt(pulse, lower, upper));
}

void updateLatchChannelRuntime(const ChannelConfig &config,
                               const uint32_t previous_buttons,
                               const uint32_t current_buttons,
                               ChannelRuntimeState *state) {
  if (state == nullptr) {
    return;
  }

  if (!buttonRisingEdge(previous_buttons, current_buttons,
                        config.latch.button_source_index)) {
    return;
  }

  state->latched = !state->latched;
  state->pulse_us = static_cast<uint16_t>(
      clampPpmPulseUs(state->latched ? config.latch.active_pulse_us
                                     : config.latch.reset_pulse_us));
}

void updateSelectorChannelRuntime(const ChannelConfig &config,
                                  const uint32_t previous_buttons,
                                  const uint32_t current_buttons,
                                  ChannelRuntimeState *state) {
  if (state == nullptr) {
    return;
  }

  const size_t entry_count = (config.selector.entry_count <=
                              static_cast<uint8_t>(kChannelSelectorEntryCount))
                                 ? config.selector.entry_count
                                 : kChannelSelectorEntryCount;
  for (size_t i = 0; i < entry_count; ++i) {
    const ChannelSelectorEntryConfig &entry = config.selector.entries[i];
    if (!buttonRisingEdge(previous_buttons, current_buttons,
                          entry.button_source_index)) {
      continue;
    }

    state->active_selector_index = static_cast<int8_t>(i);
    state->pulse_us =
        static_cast<uint16_t>(clampPpmPulseUs(entry.pulse_us));
    return;
  }
}

void updateChannelLogicRuntime(const AppConfig &config,
                               const uint32_t previous_buttons,
                               const uint32_t current_buttons) {
  for (size_t i = 0; i < kPpmChannelCount; ++i) {
    ChannelRuntimeState *runtime = &g_channel_logic.channels[i];
    switch (config.channels[i].mode) {
      case ChannelMode::Stepper:
        updateStepperChannelRuntime(config.channels[i], previous_buttons,
                                    current_buttons, runtime);
        break;
      case ChannelMode::Latch:
        updateLatchChannelRuntime(config.channels[i], previous_buttons,
                                  current_buttons, runtime);
        break;
      case ChannelMode::Selector:
        updateSelectorChannelRuntime(config.channels[i], previous_buttons,
                                     current_buttons, runtime);
        break;
      case ChannelMode::Source:
      default:
        break;
    }
  }
}

}  // namespace

bool initializeAppState() {
  if (g_state_mutex != nullptr) {
    return true;
  }

  g_state_mutex = xSemaphoreCreateMutex();
  if (g_state_mutex == nullptr) {
    Serial.println("State mutex alloc failed");
    return false;
  }

  return true;
}

void resetConfigToDefaults(AppConfig *config) {
  if (config == nullptr) {
    return;
  }

  *config = kEmptyAppConfig;
  for (size_t i = 0; i < kAxisCount; ++i) {
    config->axes[i].calibration_min = 0;
    config->axes[i].calibration_max = 0;
  }
  for (size_t i = 0; i < kPpmChannelCount; ++i) {
    config->channels[i].source_index =
        (i < kAxisCount) ? static_cast<int8_t>(i)
                         : static_cast<int8_t>(kChannelSourceNone);
  }
}

void initializeConfig() {
  AppConfig config = {};
  resetConfigToDefaults(&config);
  if (isFileSystemReady() && loadConfigFromFilesystem(&config)) {
    Serial.println("Config loaded from /config.json");
  } else if (isFileSystemReady() && !configFileExists()) {
    saveConfigToFilesystem(config);
    Serial.println("Default config created");
  } else if (isFileSystemReady()) {
    Serial.println("Persisted config rejected, running with defaults");
  } else {
    Serial.println("Running with in-memory default config");
  }

  setConfigSnapshot(config, false);
  withStateLock([]() { g_config_revision = 1; });
}

void copyConfigSnapshot(AppConfig *config, uint32_t *revision) {
  if (config == nullptr) {
    return;
  }

  *config = kEmptyAppConfig;
  withStateLock([&]() {
    *config = g_app_config;
    if (revision != nullptr) {
      *revision = g_config_revision;
    }
  });
}

void setConfigSnapshot(const AppConfig &config, const bool bump_revision) {
  withStateLock([&]() {
    g_app_config = config;
    if (bump_revision) {
      ++g_config_revision;
    }
  });
}

bool buildChannelOutputState(const DeviceSnapshot &snapshot,
                             const AppConfig &config,
                             const uint32_t config_revision,
                             ChannelOutputState *outputs) {
  if (outputs == nullptr) {
    return false;
  }

  AxisRuntimeSample axis_samples[kAxisCount] = {};
  const bool snapshot_has_live_state = snapshotHasLiveState(snapshot);
  for (size_t i = 0; i < kAxisCount; ++i) {
    buildAxisSample(snapshot, config, i, &axis_samples[i]);
  }

  withStateLock([&]() {
    const uint32_t buttons_mask =
        snapshot_has_live_state ? snapshot.state.buttons : 0;
    const bool config_is_stale =
        g_channel_logic.initialized &&
        (config_revision < g_channel_logic.last_config_revision);
    const bool sequence_is_stale =
        g_channel_logic.initialized &&
        (snapshot.sequence < g_channel_logic.last_sequence);
    const bool config_changed =
        !config_is_stale &&
        (!g_channel_logic.initialized ||
         config_revision != g_channel_logic.last_config_revision);
    const bool device_reactivated =
        snapshot_has_live_state && !g_channel_logic.device_active;

    if (config_changed || device_reactivated) {
      resetChannelLogicState(config, buttons_mask);
    }

    if (!config_is_stale && !sequence_is_stale) {
      if (!snapshot_has_live_state) {
        g_channel_logic.device_active = false;
        g_channel_logic.last_buttons_mask = 0;
      } else if (!config_changed && !device_reactivated &&
                 snapshot.sequence != g_channel_logic.last_sequence) {
        updateChannelLogicRuntime(config, g_channel_logic.last_buttons_mask,
                                  buttons_mask);
        g_channel_logic.last_buttons_mask = buttons_mask;
      }

      g_channel_logic.device_active = snapshot_has_live_state;
      g_channel_logic.last_sequence = snapshot.sequence;
      g_channel_logic.last_config_revision = config_revision;
    }

    for (size_t i = 0; i < kPpmChannelCount; ++i) {
      outputs[i] = ChannelOutputState{};
      outputs[i].mode = config.channels[i].mode;
      outputs[i].source_index = config.channels[i].source_index;
      outputs[i].invert = config.channels[i].invert;
      outputs[i].failsafe_pulse_us = config.channels[i].failsafe_pulse_us;

      switch (config.channels[i].mode) {
        case ChannelMode::Source: {
          const bool source_available =
              isAnalogAxisSourceIndex(config.channels[i].source_index)
                  ? axis_samples[static_cast<size_t>(
                                   config.channels[i].source_index)]
                        .present
                  : snapshot_has_live_state;
          outputs[i].using_failsafe = !source_available;
          if (!source_available) {
            outputs[i].pulse_us = config.channels[i].failsafe_pulse_us;
            break;
          }

          float output_value = channelSourceValue(
              snapshot, config, config.channels[i].source_index, axis_samples);
          if (config.channels[i].invert) {
            output_value = -output_value;
          }
          outputs[i].pulse_us = static_cast<uint16_t>(
              ppmPulseFromValue(output_value));
          break;
        }
        case ChannelMode::Stepper:
          outputs[i].using_failsafe = !snapshot_has_live_state;
          if (outputs[i].using_failsafe) {
            outputs[i].pulse_us = config.channels[i].failsafe_pulse_us;
            break;
          }
          outputs[i].pulse_us =
              static_cast<uint16_t>(clampPpmPulseUs(
                  g_channel_logic.channels[i].pulse_us));
          break;
        case ChannelMode::Latch:
          outputs[i].using_failsafe = !snapshot_has_live_state;
          if (outputs[i].using_failsafe) {
            outputs[i].pulse_us = config.channels[i].failsafe_pulse_us;
            break;
          }
          outputs[i].pulse_us =
              static_cast<uint16_t>(clampPpmPulseUs(
                  g_channel_logic.channels[i].pulse_us));
          outputs[i].latched = g_channel_logic.channels[i].latched;
          break;
        case ChannelMode::Selector:
          outputs[i].using_failsafe = !snapshot_has_live_state;
          if (outputs[i].using_failsafe) {
            outputs[i].pulse_us = config.channels[i].failsafe_pulse_us;
            break;
          }
          outputs[i].pulse_us =
              static_cast<uint16_t>(clampPpmPulseUs(
                  g_channel_logic.channels[i].pulse_us));
          outputs[i].active_selector_index =
              g_channel_logic.channels[i].active_selector_index;
          break;
      }
    }
  });

  return true;
}

bool copyActiveDeviceSnapshot(DeviceSnapshot *snapshot) {
  if (snapshot == nullptr) {
    return false;
  }

  *snapshot = kEmptyDeviceSnapshot;
  withStateLock([&]() {
    snapshot->sequence = g_state_sequence;
    for (const auto &device : g_devices) {
      if (!device.in_use) {
        continue;
      }

      snapshot->connected = true;
      snapshot->address = device.params.addr;
      snapshot->interface_number = device.params.iface_num;
      snapshot->protocol = protocolName(device.params.proto);
      snapshot->layout = device.layout;
      snapshot->state = device.last_state;
      snapshot->has_state = device.has_last_state;
      break;
    }
  });

  return true;
}

bool copyAnalogInputSnapshot(AnalogInputSnapshot *snapshot) {
  if (snapshot == nullptr) {
    return false;
  }

  *snapshot = kEmptyAnalogInputSnapshot;
  withStateLock([&]() {
    *snapshot = g_analog_inputs;
    snapshot->sequence = g_analog_input_sequence;
  });

  return true;
}

bool updateAnalogInputSnapshot(const AnalogInputSnapshot &snapshot,
                               bool *changed) {
  bool updated = false;
  if (changed != nullptr) {
    *changed = false;
  }

  withStateLock([&]() {
    const bool next_changed = !analogSnapshotsEqual(snapshot, g_analog_inputs);
    if (next_changed) {
      g_analog_inputs = snapshot;
      ++g_analog_input_sequence;
      updated = true;
    }
    if (changed != nullptr) {
      *changed = next_changed;
    }
  });

  return updated;
}

bool reserveDeviceSlot(hid_host_device_handle_t handle,
                       const hid_host_dev_params_t &params) {
  bool reserved = false;
  withStateLock([&]() {
    DeviceContext *device = findDeviceContext(handle);
    if (device != nullptr) {
      device->params = params;
      reserved = true;
      return;
    }

    for (auto &slot : g_devices) {
      if (!slot.in_use) {
        slot = kEmptyDeviceContext;
        slot.in_use = true;
        slot.handle = handle;
        slot.params = params;
        ++g_state_sequence;
        reserved = true;
        return;
      }
    }
  });

  return reserved;
}

bool copyTrackedDeviceLayout(hid_host_device_handle_t handle,
                             ParsedLayout *layout) {
  if (layout == nullptr) {
    return false;
  }

  bool found = false;
  const ParsedLayout empty_layout = ParsedLayout();
  *layout = empty_layout;
  withStateLock([&]() {
    DeviceContext *device = findDeviceContext(handle);
    if (device == nullptr) {
      return;
    }

    *layout = device->layout;
    found = true;
  });

  return found;
}

bool updateTrackedDeviceLayout(hid_host_device_handle_t handle,
                               const hid_host_dev_params_t &params,
                               const ParsedLayout &layout) {
  bool updated = false;
  withStateLock([&]() {
    DeviceContext *device = findDeviceContext(handle);
    if (device == nullptr) {
      return;
    }

    device->layout = layout;
    device->params = params;
    ++g_state_sequence;
    updated = true;
  });

  return updated;
}

bool updateTrackedDeviceState(hid_host_device_handle_t handle,
                              const GamepadState &state, bool *changed) {
  bool tracked = false;
  if (changed != nullptr) {
    *changed = false;
  }

  withStateLock([&]() {
    DeviceContext *device = findDeviceContext(handle);
    if (device == nullptr) {
      return;
    }

    tracked = true;
    const bool next_changed =
        !device->has_last_state || !statesEqual(state, device->last_state);
    device->last_state = state;
    device->has_last_state = true;
    if (next_changed) {
      ++g_state_sequence;
    }
    if (changed != nullptr) {
      *changed = next_changed;
    }
  });

  return tracked;
}

void clearTrackedDevice(const hid_host_device_handle_t handle) {
  withStateLock([&]() {
    DeviceContext *device = findDeviceContext(handle);
    if (device != nullptr) {
      *device = kEmptyDeviceContext;
      ++g_state_sequence;
    }
  });
}

}  // namespace app
