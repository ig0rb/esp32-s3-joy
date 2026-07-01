#include "app_ppm_output.h"

#include <Arduino.h>

#include <cstdint>

#include "app_build_config.h"
#include "app_state.h"
#include "app_types.h"
#include "esp32-hal-rmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace app {
namespace {

constexpr int kPpmPin = APP_PPM_OUTPUT_PIN;
constexpr int kPpmFrequencyHz = 1000000;
constexpr int kPpmFrameUs = APP_PPM_FRAME_US;
constexpr int kPpmPulseUs = APP_PPM_PULSE_US;
constexpr int kPpmMinSyncUs = APP_PPM_MIN_SYNC_US;
constexpr bool kPpmActiveLow = (APP_PPM_ACTIVE_LOW != 0);
constexpr size_t kPpmSymbolCount = kPpmChannelCount + 1;
constexpr rmt_reserve_memsize_t kPpmRmtMemBlocks = RMT_MEM_NUM_BLOCKS_2;
constexpr uint32_t kPpmTaskStackSize = 4096;
constexpr UBaseType_t kPpmTaskPriority = 6;

static_assert(
    kPpmSymbolCount <=
        (static_cast<size_t>(RMT_SYMBOLS_PER_CHANNEL_BLOCK) *
         static_cast<size_t>(kPpmRmtMemBlocks)),
    "PPM frame does not fit in the reserved RMT memory blocks");

rmt_data_t g_ppm_symbols[kPpmSymbolCount] = {};
DeviceSnapshot g_ppm_snapshot = {};
AppConfig g_ppm_config = {};
ChannelOutputState g_ppm_outputs[kPpmChannelCount] = {};
uint32_t g_ppm_config_revision = 0;
TaskHandle_t g_ppm_task_handle = nullptr;

void setPpmSymbol(rmt_data_t *symbol, const uint16_t pulse_us,
                  const uint16_t space_us) {
  if (symbol == nullptr) {
    return;
  }

  symbol->level0 = kPpmActiveLow ? 0 : 1;
  symbol->duration0 = pulse_us;
  symbol->level1 = kPpmActiveLow ? 1 : 0;
  symbol->duration1 = space_us;
}

void buildPpmFrame(const DeviceSnapshot &snapshot, const AppConfig &config,
                   const uint32_t config_revision, rmt_data_t *symbols,
                   size_t *symbol_count) {
  if (symbols == nullptr || symbol_count == nullptr) {
    return;
  }

  buildChannelOutputState(snapshot, config, config_revision, g_ppm_outputs);

  uint32_t used_frame_us = 0;
  for (size_t channel_index = 0; channel_index < kPpmChannelCount;
       ++channel_index) {
    const int pulse_total_us = g_ppm_outputs[channel_index].pulse_us;

    const uint16_t channel_total_us = static_cast<uint16_t>(
        pulse_total_us > kPpmPulseUs ? pulse_total_us : kPpmPulseUs + 1);
    used_frame_us += channel_total_us;
    setPpmSymbol(&symbols[channel_index], kPpmPulseUs,
                 channel_total_us - kPpmPulseUs);
  }

  uint32_t frame_total_us = kPpmFrameUs;
  const uint32_t required_minimum_us =
      used_frame_us + kPpmPulseUs + kPpmMinSyncUs;
  if (frame_total_us < required_minimum_us) {
    frame_total_us = required_minimum_us;
  }

  const uint16_t sync_gap_us =
      static_cast<uint16_t>(frame_total_us - used_frame_us - kPpmPulseUs);
  setPpmSymbol(&symbols[kPpmChannelCount], kPpmPulseUs, sync_gap_us);
  *symbol_count = kPpmSymbolCount;
}

bool writePpmFrame(const DeviceSnapshot &snapshot, const AppConfig &config,
                   const uint32_t config_revision, size_t *symbol_count) {
  if (symbol_count == nullptr) {
    return false;
  }

  buildPpmFrame(snapshot, config, config_revision, g_ppm_symbols, symbol_count);
  if (*symbol_count == 0) {
    return false;
  }

  return rmtWrite(kPpmPin, g_ppm_symbols, *symbol_count, RMT_WAIT_FOR_EVER);
}

void ppmOutputTask(void *parameter) {
  (void)parameter;

  bool last_failsafe_active = false;
  bool has_last_failsafe_state = false;

  while (true) {
    size_t symbol_count = 0;
    copyActiveDeviceSnapshot(&g_ppm_snapshot);
    copyConfigSnapshot(&g_ppm_config, &g_ppm_config_revision);

    if (!writePpmFrame(g_ppm_snapshot, g_ppm_config, g_ppm_config_revision,
                       &symbol_count)) {
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    const bool failsafe_active =
        !g_ppm_snapshot.connected || !g_ppm_snapshot.has_state;
    if (has_last_failsafe_state &&
        failsafe_active != last_failsafe_active) {
      Serial.printf("PPM failsafe %s\n",
                    failsafe_active ? "ACTIVE" : "CLEARED");
    }

    last_failsafe_active = failsafe_active;
    has_last_failsafe_state = true;
  }
}

}  // namespace

bool startPpmOutput() {
  if (g_ppm_task_handle != nullptr) {
    return true;
  }

  pinMode(kPpmPin, OUTPUT);
  digitalWrite(kPpmPin, kPpmActiveLow ? HIGH : LOW);

  if (!rmtInit(kPpmPin, RMT_TX_MODE, kPpmRmtMemBlocks, kPpmFrequencyHz)) {
    Serial.printf("PPM init failed on GPIO%d\n", kPpmPin);
    return false;
  }
  rmtSetEOT(kPpmPin, kPpmActiveLow ? HIGH : LOW);

  if (xTaskCreate(ppmOutputTask, "ppm_output", kPpmTaskStackSize, nullptr,
                  kPpmTaskPriority, &g_ppm_task_handle) != pdPASS) {
    Serial.printf("PPM task creation failed on GPIO%d\n", kPpmPin);
    return false;
  }

  Serial.printf(
      "PPM output ready on GPIO%d, %u channels, frame=%dus, pulse=%dus, polarity=%s\n",
      kPpmPin, static_cast<unsigned>(kPpmChannelCount), kPpmFrameUs,
      kPpmPulseUs, kPpmActiveLow ? "active-low" : "active-high");
  return true;
}

void pollPpmOutput() {
  // PPM generation is task-driven to guarantee frame-boundary updates.
}

int ppmOutputPin() {
  return kPpmPin;
}

}  // namespace app
