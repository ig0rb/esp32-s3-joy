#include <Arduino.h>

#include "app_analog_input.h"
#include "app_network.h"
#include "app_ppm_output.h"
#include "app_state.h"
#include "app_storage.h"
#include "app_status_led.h"
#include "app_usb_hid.h"
#include "app_web.h"

namespace {

constexpr uint32_t kSerialBaudRate = 115200;

}  // namespace

void setup() {
  Serial.begin(kSerialBaudRate);
  delay(1200);

  Serial.println();
  Serial.println("ESP32-S3 USB HID joypad host");
  Serial.println(
      "Output standardizzato: X/Y/Z/Rx/Ry/Rz/Hat/Button N dalla report map HID.");
  Serial.println(
      "Monitor su UART0. Collega il joypad alla USB nativa dell'S3 con VBUS "
      "5V presente.");
  Serial.println(
      "Se il LED RGB di stato su GPIO48 resta spento, verifica che il jumper "
      "RGB della scheda sia chiuso/presente.");

  app::initializeAppState();
  app::registerWiFiEventLogger();
  app::startFileSystem();
  app::initializeConfig();
  app::startAnalogInputs();
  app::startStatusLed();
  app::startPpmOutput();
  app::startNetwork();
  app::startWebServer();

  if (!app::startUsbHostJoypad()) {
    Serial.println("USB HID host startup failed");
  } else {
    Serial.println("USB HID host ready");
  }
}

void loop() {
  app::pollAnalogInputs();
  app::maintainStationMode();
  app::pollPpmOutput();
  app::pollWebServer();
  delay(5);
}
