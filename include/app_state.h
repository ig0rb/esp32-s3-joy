#pragma once

#include "app_types.h"
#include "hid_host.h"

namespace app {

bool initializeAppState();
void resetConfigToDefaults(AppConfig *config);
void initializeConfig();
void copyConfigSnapshot(AppConfig *config, uint32_t *revision = nullptr);
void setConfigSnapshot(const AppConfig &config, bool bump_revision);
bool copyActiveDeviceSnapshot(DeviceSnapshot *snapshot);
bool copyAnalogInputSnapshot(AnalogInputSnapshot *snapshot);
bool buildChannelOutputState(const DeviceSnapshot &snapshot,
                             const AppConfig &config, uint32_t config_revision,
                             ChannelOutputState *outputs);
bool updateAnalogInputSnapshot(const AnalogInputSnapshot &snapshot,
                               bool *changed = nullptr);
bool reserveDeviceSlot(hid_host_device_handle_t handle,
                       const hid_host_dev_params_t &params);
bool copyTrackedDeviceLayout(hid_host_device_handle_t handle,
                             ParsedLayout *layout);
bool updateTrackedDeviceLayout(hid_host_device_handle_t handle,
                               const hid_host_dev_params_t &params,
                               const ParsedLayout &layout);
bool updateTrackedDeviceState(hid_host_device_handle_t handle,
                              const GamepadState &state, bool *changed);
void clearTrackedDevice(hid_host_device_handle_t handle);

}  // namespace app
