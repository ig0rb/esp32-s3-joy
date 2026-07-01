#pragma once

#ifndef APP_AP_SSID
#define APP_AP_SSID "ESP32-S3-JOY"
#endif

#ifndef APP_DEV_MODE
#define APP_DEV_MODE 0
#endif

#ifndef APP_AP_PASSWORD
#define APP_AP_PASSWORD ""
#endif

#ifndef APP_AP_CHANNEL
#define APP_AP_CHANNEL 1
#endif

#ifndef APP_AP_MAX_CLIENTS
#define APP_AP_MAX_CLIENTS 4
#endif

#ifndef APP_AP_IP
#define APP_AP_IP "192.168.4.1"
#endif

#ifndef APP_AP_GATEWAY
#define APP_AP_GATEWAY "192.168.4.1"
#endif

#ifndef APP_AP_SUBNET
#define APP_AP_SUBNET "255.255.255.0"
#endif

#ifndef APP_WIFI_SSID
#define APP_WIFI_SSID ""
#endif

#ifndef APP_WIFI_PASSWORD
#define APP_WIFI_PASSWORD ""
#endif

#ifndef APP_WIFI_CONNECT_TIMEOUT_MS
#define APP_WIFI_CONNECT_TIMEOUT_MS 15000
#endif

#ifndef APP_UI_POLL_MS
#define APP_UI_POLL_MS 80
#endif

#ifndef APP_ANALOG_INPUT_COUNT
#define APP_ANALOG_INPUT_COUNT 2
#endif

#ifndef APP_ANALOG_INPUT_1_PIN
#define APP_ANALOG_INPUT_1_PIN 5
#endif

#ifndef APP_ANALOG_INPUT_2_PIN
#define APP_ANALOG_INPUT_2_PIN 6
#endif

#ifndef APP_ANALOG_INPUT_3_PIN
#define APP_ANALOG_INPUT_3_PIN -1
#endif

#ifndef APP_ANALOG_INPUT_4_PIN
#define APP_ANALOG_INPUT_4_PIN -1
#endif

#ifndef APP_ANALOG_READ_INTERVAL_MS
#define APP_ANALOG_READ_INTERVAL_MS 20
#endif

#ifndef APP_ANALOG_READ_RESOLUTION_BITS
#define APP_ANALOG_READ_RESOLUTION_BITS 12
#endif

#ifndef APP_ANALOG_SAMPLES_PER_READ
#define APP_ANALOG_SAMPLES_PER_READ 4
#endif

#ifndef APP_PPM_CHANNEL_COUNT
#define APP_PPM_CHANNEL_COUNT 8
#endif

#ifndef APP_PPM_OUTPUT_PIN
#define APP_PPM_OUTPUT_PIN 4
#endif

#ifndef APP_PPM_FRAME_US
#define APP_PPM_FRAME_US 18000
#endif

#ifndef APP_PPM_PULSE_US
#define APP_PPM_PULSE_US 300
#endif

#ifndef APP_PPM_MIN_SYNC_US
#define APP_PPM_MIN_SYNC_US 4000
#endif

#ifndef APP_PPM_ACTIVE_LOW
#define APP_PPM_ACTIVE_LOW 1
#endif

#ifndef APP_STATUS_LED_ENABLED
#define APP_STATUS_LED_ENABLED 1
#endif

#ifndef APP_STATUS_LED_PIN
#define APP_STATUS_LED_PIN 48
#endif

#ifndef APP_STATUS_LED_BRIGHTNESS
#define APP_STATUS_LED_BRIGHTNESS 48
#endif
