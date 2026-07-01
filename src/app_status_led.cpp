#include "app_status_led.h"

#include <Arduino.h>

#include <cstdint>

#include "app_build_config.h"
#include "app_state.h"
#include "esp32-hal-rgb-led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace app {
namespace {

#ifdef RGB_BUILTIN
constexpr int kStatusLedWritePin =
    (APP_STATUS_LED_PIN == PIN_RGB_LED) ? RGB_BUILTIN : APP_STATUS_LED_PIN;
constexpr int kStatusLedGpio = APP_STATUS_LED_PIN;
constexpr bool kStatusLedUsingBuiltinAlias =
    (APP_STATUS_LED_PIN == PIN_RGB_LED);
#else
constexpr int kStatusLedWritePin = APP_STATUS_LED_PIN;
constexpr int kStatusLedGpio = APP_STATUS_LED_PIN;
constexpr bool kStatusLedUsingBuiltinAlias = false;
#endif
constexpr bool kStatusLedEnabled = (APP_STATUS_LED_ENABLED != 0);
constexpr uint8_t kStatusLedBrightness =
    static_cast<uint8_t>(APP_STATUS_LED_BRIGHTNESS);
constexpr uint32_t kStatusLedTaskStackSize = 3072;
constexpr UBaseType_t kStatusLedTaskPriority = 1;
constexpr TickType_t kStatusLedTick = pdMS_TO_TICKS(25);
constexpr uint32_t kFailsafeBlinkMs = 250;
constexpr uint32_t kOkCycleMs = 1800;
constexpr uint8_t kOkBaseGreen = 18;
constexpr uint8_t kOkPeakGreen = 180;
constexpr uint8_t kOkPeakBlue = 72;

TaskHandle_t g_status_led_task_handle = nullptr;
DeviceSnapshot g_status_led_snapshot = {};

uint8_t scaleBrightness(const uint8_t value) {
  return static_cast<uint8_t>((static_cast<uint16_t>(value) *
                               static_cast<uint16_t>(kStatusLedBrightness)) /
                              255U);
}

void writeStatusLed(const uint8_t red, const uint8_t green,
                    const uint8_t blue) {
  rgbLedWrite(kStatusLedWritePin, scaleBrightness(red), scaleBrightness(green),
              scaleBrightness(blue));
}

uint8_t triangleWave(const uint32_t now_ms, const uint32_t period_ms) {
  if (period_ms < 2) {
    return 0;
  }

  const uint32_t phase = now_ms % period_ms;
  const uint32_t half_period = period_ms / 2;
  if (phase < half_period) {
    return static_cast<uint8_t>((phase * 255U) / half_period);
  }

  return static_cast<uint8_t>(((period_ms - phase) * 255U) / half_period);
}

void runStartupAnimation() {
  static const uint8_t kStartupColors[][3] = {
      {255, 0, 0}, {255, 96, 0}, {0, 255, 0}, {0, 180, 72},
      {0, 96, 255}, {180, 0, 255}, {0, 0, 0},
  };

  for (const auto &color : kStartupColors) {
    writeStatusLed(color[0], color[1], color[2]);
    vTaskDelay(pdMS_TO_TICKS(140));
  }
}

void statusLedTask(void *parameter) {
  (void)parameter;

  runStartupAnimation();

  while (true) {
    copyActiveDeviceSnapshot(&g_status_led_snapshot);

    const bool failsafe_active =
        !g_status_led_snapshot.connected || !g_status_led_snapshot.has_state;
    if (failsafe_active) {
      const bool led_on = ((millis() / kFailsafeBlinkMs) % 2U) == 0U;
      writeStatusLed(led_on ? 255 : 0, 0, 0);
    } else {
      const uint8_t wave = triangleWave(millis(), kOkCycleMs);
      const uint8_t green = static_cast<uint8_t>(
          kOkBaseGreen +
          ((static_cast<uint16_t>(wave) *
            static_cast<uint16_t>(kOkPeakGreen - kOkBaseGreen)) /
           255U));
      const uint8_t blue = static_cast<uint8_t>(
          (static_cast<uint16_t>(wave) * static_cast<uint16_t>(kOkPeakBlue)) /
          255U);
      writeStatusLed(0, green, blue);
    }

    vTaskDelay(kStatusLedTick);
  }
}

}  // namespace

bool startStatusLed() {
  if (!kStatusLedEnabled) {
    return false;
  }

  if (g_status_led_task_handle != nullptr) {
    return true;
  }

  pinMode(kStatusLedWritePin, OUTPUT);
  writeStatusLed(0, 0, 0);

  if (xTaskCreate(statusLedTask, "status_led", kStatusLedTaskStackSize,
                  nullptr, kStatusLedTaskPriority,
                  &g_status_led_task_handle) != pdPASS) {
    Serial.printf("Status LED task creation failed on GPIO%d\n",
                  kStatusLedGpio);
    return false;
  }

  Serial.printf("Status LED ready on GPIO%d%s, brightness=%u\n", kStatusLedGpio,
                kStatusLedUsingBuiltinAlias ? " via RGB_BUILTIN" : "",
                static_cast<unsigned>(kStatusLedBrightness));

#if defined(ARDUINO_ESP32S3_DEV) && defined(PIN_RGB_LED)
  if (kStatusLedGpio == PIN_RGB_LED) {
    Serial.println(
        "Status LED note: the esp32s3 board variant warns some DevKitC-S3 "
        "boards have weak RGB data on GPIO48; if the LED stays off, try "
        "driving the LED from another 3V GPIO wired to the RGB data line.");
  }
#endif
  return true;
}

}  // namespace app
