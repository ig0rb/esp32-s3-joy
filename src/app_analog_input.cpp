#include "app_analog_input.h"

#include <Arduino.h>

#include "app_build_config.h"
#include "app_state.h"
#include "app_types.h"
#include "esp32-hal-adc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace app {
namespace {

constexpr uint32_t kAnalogReadIntervalMs =
    (APP_ANALOG_READ_INTERVAL_MS > 0) ? APP_ANALOG_READ_INTERVAL_MS : 1;
constexpr uint8_t kAnalogResolutionBits =
    (APP_ANALOG_READ_RESOLUTION_BITS < 9)
        ? 9
        : ((APP_ANALOG_READ_RESOLUTION_BITS > 12)
               ? 12
               : APP_ANALOG_READ_RESOLUTION_BITS);
constexpr uint8_t kAnalogSamplesPerRead =
    (APP_ANALOG_SAMPLES_PER_READ < 1)
        ? 1
        : ((APP_ANALOG_SAMPLES_PER_READ > 16) ? 16
                                              : APP_ANALOG_SAMPLES_PER_READ);
constexpr int32_t kAnalogLogicalMin = 0;
constexpr int32_t kAnalogLogicalMax = (1 << kAnalogResolutionBits) - 1;
constexpr uint32_t kAnalogTaskStackSize = 3072;
constexpr UBaseType_t kAnalogTaskPriority = 2;
constexpr int kAnalogInputPins[kAnalogInputMaxCount] = {
    APP_ANALOG_INPUT_1_PIN,
    APP_ANALOG_INPUT_2_PIN,
    APP_ANALOG_INPUT_3_PIN,
    APP_ANALOG_INPUT_4_PIN,
};

TaskHandle_t g_analog_task_handle = nullptr;

int32_t readAveragedPin(const int pin) {
  uint32_t total = 0;
  for (uint8_t sample_index = 0; sample_index < kAnalogSamplesPerRead;
       ++sample_index) {
    total += static_cast<uint32_t>(analogRead(pin));
  }

  const int32_t averaged =
      static_cast<int32_t>(total / static_cast<uint32_t>(kAnalogSamplesPerRead));
  return static_cast<int32_t>(
      clampInt(averaged, kAnalogLogicalMin, kAnalogLogicalMax));
}

void buildAnalogSnapshot(AnalogInputSnapshot *snapshot) {
  if (snapshot == nullptr) {
    return;
  }

  *snapshot = AnalogInputSnapshot{};
  for (size_t input_index = 0; input_index < kAnalogInputCount; ++input_index) {
    AnalogInputState &input = snapshot->inputs[input_index];
    input.logical_min = kAnalogLogicalMin;
    input.logical_max = kAnalogLogicalMax;

    const int pin = kAnalogInputPins[input_index];
    if (pin < 0) {
      continue;
    }

    input.present = true;
    input.raw = readAveragedPin(pin);
  }
}

void analogInputTask(void *parameter) {
  (void)parameter;

  while (true) {
    AnalogInputSnapshot snapshot = {};
    buildAnalogSnapshot(&snapshot);
    updateAnalogInputSnapshot(snapshot);
    vTaskDelay(pdMS_TO_TICKS(kAnalogReadIntervalMs));
  }
}

}  // namespace

bool startAnalogInputs() {
  if (g_analog_task_handle != nullptr) {
    return true;
  }

  if (kAnalogInputCount == 0) {
    Serial.println("Analog inputs disabled");
    return true;
  }

  analogReadResolution(kAnalogResolutionBits);

  size_t configured_inputs = 0;
  for (size_t input_index = 0; input_index < kAnalogInputCount; ++input_index) {
    const int pin = kAnalogInputPins[input_index];
    if (pin < 0) {
      Serial.printf("ADC %u skipped: invalid GPIO%d\n",
                    static_cast<unsigned>(input_index + 1), pin);
      continue;
    }

    pinMode(pin, INPUT);
    analogSetPinAttenuation(static_cast<uint8_t>(pin), ADC_11db);
    ++configured_inputs;
  }

  AnalogInputSnapshot initial_snapshot = {};
  buildAnalogSnapshot(&initial_snapshot);
  updateAnalogInputSnapshot(initial_snapshot);

  if (xTaskCreate(analogInputTask, "analog_input", kAnalogTaskStackSize, nullptr,
                  kAnalogTaskPriority, &g_analog_task_handle) != pdPASS) {
    Serial.println("Analog input task creation failed");
    g_analog_task_handle = nullptr;
    return false;
  }

  Serial.printf(
      "Analog inputs ready: %u/%u configured, resolution=%u-bit, sample=%ums\n",
      static_cast<unsigned>(configured_inputs),
      static_cast<unsigned>(kAnalogInputCount),
      static_cast<unsigned>(kAnalogResolutionBits), kAnalogReadIntervalMs);
  return true;
}

void pollAnalogInputs() {
  // ADC sampling is task-driven to keep reads periodic and detached from loop().
}

}  // namespace app
