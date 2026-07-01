#include "app_usb_hid.h"

#include <Arduino.h>

#include <cstring>

#include "app_state.h"
#include "app_types.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hid.h"
#include "hid_host.h"
#include "usb/usb_host.h"

extern "C" void hidHostInterfaceCallback(
    hid_host_device_handle_t device_handle,
    hid_host_interface_event_t event, void *arg);

extern "C" void hidHostDeviceCallback(
    hid_host_device_handle_t device_handle, hid_host_driver_event_t event,
    void *arg);

namespace {

using app::GamepadState;
using app::ParsedField;
using app::ParsedFieldType;
using app::ParsedLayout;

constexpr size_t kInputReportBufferSize = 64;
constexpr size_t kMaxLocalUsages = 32;
constexpr size_t kMaxGlobalStackDepth = 4;
constexpr uint32_t kUsbLibTaskStackSize = 4096;
constexpr uint32_t kHidBackgroundTaskStackSize = 6144;
constexpr uint32_t kHidDispatchTaskStackSize = 4096;
constexpr UBaseType_t kUsbLibTaskPriority = 7;
constexpr UBaseType_t kHidBackgroundTaskPriority = 7;
constexpr UBaseType_t kHidDispatchTaskPriority = 3;

constexpr uint16_t kUsagePageGenericDesktop = 0x01;
constexpr uint16_t kUsagePageButton = 0x09;

constexpr uint16_t kUsageX = 0x30;
constexpr uint16_t kUsageY = 0x31;
constexpr uint16_t kUsageZ = 0x32;
constexpr uint16_t kUsageRx = 0x33;
constexpr uint16_t kUsageRy = 0x34;
constexpr uint16_t kUsageRz = 0x35;
constexpr uint16_t kUsageSlider = 0x36;
constexpr uint16_t kUsageDial = 0x37;
constexpr uint16_t kUsageWheel = 0x38;
constexpr uint16_t kUsageHatSwitch = 0x39;
constexpr uint16_t kUsageDpadUp = 0x90;
constexpr uint16_t kUsageDpadDown = 0x91;
constexpr uint16_t kUsageDpadRight = 0x92;
constexpr uint16_t kUsageDpadLeft = 0x93;

struct HidDriverEvent {
  hid_host_device_handle_t device_handle;
  hid_host_driver_event_t event;
  void *arg;
};

struct GlobalState {
  uint16_t usage_page = 0;
  int32_t logical_min = 0;
  int32_t logical_max = 0;
  uint8_t report_size = 0;
  uint8_t report_count = 0;
  uint8_t report_id = 0;
};

struct LocalState {
  uint16_t usages[kMaxLocalUsages] = {};
  size_t usage_count = 0;
  bool has_usage_range = false;
  uint16_t usage_min = 0;
  uint16_t usage_max = 0;

  void clear() {
    usage_count = 0;
    has_usage_range = false;
    usage_min = 0;
    usage_max = 0;
  }

  void addUsage(const uint16_t usage) {
    if (usage_count < kMaxLocalUsages) {
      usages[usage_count++] = usage;
    }
  }

  uint16_t usageForIndex(const size_t index) const {
    if (index < usage_count) {
      return usages[index];
    }

    if (has_usage_range) {
      const uint32_t usage = usage_min + index;
      if (usage <= usage_max) {
        return static_cast<uint16_t>(usage);
      }
      return 0;
    }

    if (usage_count > 0) {
      return usages[usage_count - 1];
    }

    return 0;
  }
};

QueueHandle_t g_hid_event_queue = nullptr;

struct InputCallbackScratch {
  uint8_t report_data[kInputReportBufferSize] = {};
  ParsedLayout layout = {};
  GamepadState state = {};
};

// The HID library invokes interface callbacks from a single background task
// named "USB HID Host". Keeping these large decode buffers out of that task's
// stack avoids stack canary trips on the first input reports.
InputCallbackScratch g_input_callback_scratch = {};

const char *hatName(const uint8_t hat) {
  switch (hat) {
    case 0:
      return "UP";
    case 1:
      return "UP-RIGHT";
    case 2:
      return "RIGHT";
    case 3:
      return "DOWN-RIGHT";
    case 4:
      return "DOWN";
    case 5:
      return "DOWN-LEFT";
    case 6:
      return "LEFT";
    case 7:
      return "UP-LEFT";
    case 8:
    case 15:
      return "CENTER";
    default:
      return "?";
  }
}

const char *fieldTypeName(const ParsedFieldType type) {
  switch (type) {
    case ParsedFieldType::AxisX:
      return "X";
    case ParsedFieldType::AxisY:
      return "Y";
    case ParsedFieldType::AxisZ:
      return "Z";
    case ParsedFieldType::AxisRx:
      return "Rx";
    case ParsedFieldType::AxisRy:
      return "Ry";
    case ParsedFieldType::AxisRz:
      return "Rz";
    case ParsedFieldType::Slider:
      return "Slider";
    case ParsedFieldType::Dial:
      return "Dial";
    case ParsedFieldType::Wheel:
      return "Wheel";
    case ParsedFieldType::Hat:
      return "Hat";
    case ParsedFieldType::DpadUp:
      return "DPadUp";
    case ParsedFieldType::DpadDown:
      return "DPadDown";
    case ParsedFieldType::DpadRight:
      return "DPadRight";
    case ParsedFieldType::DpadLeft:
      return "DPadLeft";
    case ParsedFieldType::Button:
      return "Button";
    default:
      return "Other";
  }
}

uint32_t readUnsignedLittleEndian(const uint8_t *data, const uint8_t size) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < size; ++i) {
    value |= static_cast<uint32_t>(data[i]) << (8 * i);
  }
  return value;
}

int32_t signExtend(const uint32_t value, const uint8_t bits) {
  if (bits == 0 || bits >= 32) {
    return static_cast<int32_t>(value);
  }

  const uint32_t sign_bit = 1UL << (bits - 1);
  if (value & sign_bit) {
    const uint32_t mask = ~((1UL << bits) - 1);
    return static_cast<int32_t>(value | mask);
  }

  return static_cast<int32_t>(value);
}

ParsedFieldType mapUsageToFieldType(const uint16_t usage_page,
                                    const uint16_t usage) {
  if (usage_page == kUsagePageButton) {
    return ParsedFieldType::Button;
  }

  if (usage_page != kUsagePageGenericDesktop) {
    return ParsedFieldType::Other;
  }

  switch (usage) {
    case kUsageX:
      return ParsedFieldType::AxisX;
    case kUsageY:
      return ParsedFieldType::AxisY;
    case kUsageZ:
      return ParsedFieldType::AxisZ;
    case kUsageRx:
      return ParsedFieldType::AxisRx;
    case kUsageRy:
      return ParsedFieldType::AxisRy;
    case kUsageRz:
      return ParsedFieldType::AxisRz;
    case kUsageSlider:
      return ParsedFieldType::Slider;
    case kUsageDial:
      return ParsedFieldType::Dial;
    case kUsageWheel:
      return ParsedFieldType::Wheel;
    case kUsageHatSwitch:
      return ParsedFieldType::Hat;
    case kUsageDpadUp:
      return ParsedFieldType::DpadUp;
    case kUsageDpadDown:
      return ParsedFieldType::DpadDown;
    case kUsageDpadRight:
      return ParsedFieldType::DpadRight;
    case kUsageDpadLeft:
      return ParsedFieldType::DpadLeft;
    default:
      return ParsedFieldType::Other;
  }
}

bool addParsedField(ParsedLayout *layout, const ParsedFieldType type,
                    const GlobalState &global_state, const uint16_t usage,
                    const uint16_t bit_offset) {
  if (layout == nullptr || layout->field_count >= app::kMaxParsedFields) {
    return false;
  }

  ParsedField &field = layout->fields[layout->field_count++];
  field.type = type;
  field.usage_page = global_state.usage_page;
  field.usage = usage;
  field.bit_offset = bit_offset;
  field.bit_size = global_state.report_size;
  field.logical_min = global_state.logical_min;
  field.logical_max = global_state.logical_max;
  field.report_id = global_state.report_id;
  return true;
}

bool parseReportDescriptor(const uint8_t *descriptor, const size_t length,
                           ParsedLayout *layout) {
  if (descriptor == nullptr || layout == nullptr) {
    return false;
  }

  *layout = ParsedLayout{};

  GlobalState global_state = {};
  GlobalState global_stack[kMaxGlobalStackDepth] = {};
  size_t global_stack_size = 0;
  LocalState local_state = {};
  uint16_t input_bit_offsets[256] = {};

  size_t index = 0;
  while (index < length) {
    const uint8_t prefix = descriptor[index++];

    if (prefix == 0xFE) {
      if (index + 1 >= length) {
        break;
      }
      const uint8_t data_size = descriptor[index++];
      index++;
      index += data_size;
      continue;
    }

    uint8_t item_size = prefix & 0x03;
    if (item_size == 3) {
      item_size = 4;
    }

    const uint8_t item_type = (prefix >> 2) & 0x03;
    const uint8_t item_tag = (prefix >> 4) & 0x0F;

    if (index + item_size > length) {
      break;
    }

    const uint8_t *item_data = descriptor + index;
    index += item_size;

    const uint32_t item_value_u =
        item_size ? readUnsignedLittleEndian(item_data, item_size) : 0;
    const int32_t item_value_s =
        item_size ? signExtend(item_value_u, item_size * 8) : 0;

    switch (item_type) {
      case 0x00: {
        if (item_tag == 0x08) {
          const bool is_constant = (item_value_u & 0x01U) != 0;
          const bool is_variable = (item_value_u & 0x02U) != 0;
          uint16_t &bit_offset = input_bit_offsets[global_state.report_id];
          const uint16_t base_bit_offset = bit_offset;

          if (!is_constant && is_variable) {
            for (uint8_t i = 0; i < global_state.report_count; ++i) {
              const uint16_t usage = local_state.usageForIndex(i);
              const ParsedFieldType type =
                  mapUsageToFieldType(global_state.usage_page, usage);
              const uint16_t field_bit_offset =
                  base_bit_offset + (i * global_state.report_size);

              if (type != ParsedFieldType::Other && usage != 0) {
                addParsedField(layout, type, global_state, usage,
                               field_bit_offset);
              }
            }
          }

          bit_offset =
              base_bit_offset +
              (global_state.report_size * global_state.report_count);
          local_state.clear();
        } else if (item_tag == 0x09 || item_tag == 0x0B || item_tag == 0x0A ||
                   item_tag == 0x0C) {
          local_state.clear();
        }
        break;
      }

      case 0x01: {
        switch (item_tag) {
          case 0x00:
            global_state.usage_page = static_cast<uint16_t>(item_value_u);
            break;
          case 0x01:
            global_state.logical_min = item_value_s;
            break;
          case 0x02:
            global_state.logical_max = item_value_s;
            break;
          case 0x07:
            global_state.report_size = static_cast<uint8_t>(item_value_u);
            break;
          case 0x08:
            global_state.report_id = static_cast<uint8_t>(item_value_u);
            if (global_state.report_id != 0) {
              layout->has_report_id = true;
              if (input_bit_offsets[global_state.report_id] == 0) {
                input_bit_offsets[global_state.report_id] = 8;
              }
            }
            break;
          case 0x09:
            global_state.report_count = static_cast<uint8_t>(item_value_u);
            break;
          case 0x0A:
            if (global_stack_size < kMaxGlobalStackDepth) {
              global_stack[global_stack_size++] = global_state;
            }
            break;
          case 0x0B:
            if (global_stack_size > 0) {
              global_state = global_stack[--global_stack_size];
            }
            break;
          default:
            break;
        }
        break;
      }

      case 0x02: {
        switch (item_tag) {
          case 0x00:
            local_state.addUsage(static_cast<uint16_t>(item_value_u));
            break;
          case 0x01:
            local_state.has_usage_range = true;
            local_state.usage_min = static_cast<uint16_t>(item_value_u);
            break;
          case 0x02:
            local_state.has_usage_range = true;
            local_state.usage_max = static_cast<uint16_t>(item_value_u);
            break;
          default:
            break;
        }
        break;
      }

      default:
        break;
    }
  }

  layout->valid = layout->field_count > 0;
  return layout->valid;
}

void printParsedLayout(const ParsedLayout &layout) {
  if (!layout.valid) {
    Serial.println("No standard HID fields parsed from descriptor");
    return;
  }

  Serial.printf("Parsed HID layout: %u field(s)\n",
                static_cast<unsigned>(layout.field_count));
  for (size_t i = 0; i < layout.field_count; ++i) {
    const ParsedField &field = layout.fields[i];
    Serial.printf(
        "  [%u] rid=%u %s usage_page=0x%02X usage=0x%02X bits=%u@%u "
        "logical=%ld..%ld\n",
        static_cast<unsigned>(i), field.report_id, fieldTypeName(field.type),
        field.usage_page, field.usage, field.bit_size, field.bit_offset,
        static_cast<long>(field.logical_min),
        static_cast<long>(field.logical_max));
  }
}

uint32_t extractBits(const uint8_t *data, const size_t length,
                     const uint16_t bit_offset, const uint8_t bit_size) {
  uint32_t value = 0;

  for (uint8_t bit = 0; bit < bit_size && bit < 32; ++bit) {
    const uint16_t absolute_bit = bit_offset + bit;
    const size_t byte_index = absolute_bit / 8;
    const uint8_t bit_index = absolute_bit % 8;

    if (byte_index >= length) {
      break;
    }

    if (data[byte_index] & (1U << bit_index)) {
      value |= (1UL << bit);
    }
  }

  return value;
}

bool decodeInputReport(const ParsedLayout &layout, const uint8_t *data,
                       const size_t length, GamepadState *state) {
  if (!layout.valid || data == nullptr || state == nullptr || length == 0) {
    return false;
  }

  *state = GamepadState{};
  const uint8_t report_id = layout.has_report_id ? data[0] : 0;
  bool found = false;

  for (size_t i = 0; i < layout.field_count; ++i) {
    const ParsedField &field = layout.fields[i];
    if (field.report_id != report_id) {
      continue;
    }

    const uint32_t raw_value =
        extractBits(data, length, field.bit_offset, field.bit_size);
    const int32_t signed_value =
        (field.logical_min < 0) ? signExtend(raw_value, field.bit_size)
                                : static_cast<int32_t>(raw_value);

    switch (field.type) {
      case ParsedFieldType::AxisX:
        state->has_x = true;
        state->x = signed_value;
        found = true;
        break;
      case ParsedFieldType::AxisY:
        state->has_y = true;
        state->y = signed_value;
        found = true;
        break;
      case ParsedFieldType::AxisZ:
        state->has_z = true;
        state->z = signed_value;
        found = true;
        break;
      case ParsedFieldType::AxisRx:
        state->has_rx = true;
        state->rx = signed_value;
        found = true;
        break;
      case ParsedFieldType::AxisRy:
        state->has_ry = true;
        state->ry = signed_value;
        found = true;
        break;
      case ParsedFieldType::AxisRz:
        state->has_rz = true;
        state->rz = signed_value;
        found = true;
        break;
      case ParsedFieldType::Slider:
        state->has_slider = true;
        state->slider = signed_value;
        found = true;
        break;
      case ParsedFieldType::Dial:
        state->has_dial = true;
        state->dial = signed_value;
        found = true;
        break;
      case ParsedFieldType::Wheel:
        state->has_wheel = true;
        state->wheel = signed_value;
        found = true;
        break;
      case ParsedFieldType::Hat:
        state->has_hat = true;
        state->hat = static_cast<uint8_t>(raw_value);
        found = true;
        break;
      case ParsedFieldType::DpadUp:
        state->has_dpad_up = true;
        state->dpad_up = raw_value != 0;
        found = true;
        break;
      case ParsedFieldType::DpadDown:
        state->has_dpad_down = true;
        state->dpad_down = raw_value != 0;
        found = true;
        break;
      case ParsedFieldType::DpadRight:
        state->has_dpad_right = true;
        state->dpad_right = raw_value != 0;
        found = true;
        break;
      case ParsedFieldType::DpadLeft:
        state->has_dpad_left = true;
        state->dpad_left = raw_value != 0;
        found = true;
        break;
      case ParsedFieldType::Button:
        if (field.usage >= 1 && field.usage <= 32 && raw_value != 0) {
          state->buttons |= (1UL << (field.usage - 1));
        }
        found = true;
        break;
      default:
        break;
    }
  }

  return found;
}

void printPressedButtons(const uint32_t buttons) {
  bool any_pressed = false;
  for (uint8_t i = 0; i < 32; ++i) {
    if (buttons & (1UL << i)) {
      if (!any_pressed) {
        Serial.print(" pressed:");
        any_pressed = true;
      }
      Serial.printf(" B%u", i + 1);
    }
  }
}

void printGamepadState(const GamepadState &state) {
  #ifdef DEBUG_PRINT_GAMEPAD_STATE
  Serial.print("HID");

  if (state.has_x) {
    Serial.printf(" X=%ld", static_cast<long>(state.x));
  }
  if (state.has_y) {
    Serial.printf(" Y=%ld", static_cast<long>(state.y));
  }
  if (state.has_z) {
    Serial.printf(" Z=%ld", static_cast<long>(state.z));
  }
  if (state.has_rx) {
    Serial.printf(" Rx=%ld", static_cast<long>(state.rx));
  }
  if (state.has_ry) {
    Serial.printf(" Ry=%ld", static_cast<long>(state.ry));
  }
  if (state.has_rz) {
    Serial.printf(" Rz=%ld", static_cast<long>(state.rz));
  }
  if (state.has_slider) {
    Serial.printf(" Slider=%ld", static_cast<long>(state.slider));
  }
  if (state.has_dial) {
    Serial.printf(" Dial=%ld", static_cast<long>(state.dial));
  }
  if (state.has_wheel) {
    Serial.printf(" Wheel=%ld", static_cast<long>(state.wheel));
  }
  if (state.has_hat) {
    Serial.printf(" Hat=%u(%s)", state.hat, hatName(state.hat));
  }
  if (state.has_dpad_up || state.has_dpad_down || state.has_dpad_right ||
      state.has_dpad_left) {
    Serial.printf(" DPad[U=%u R=%u D=%u L=%u]", state.dpad_up, state.dpad_right,
                  state.dpad_down, state.dpad_left);
  }

  Serial.printf(" Buttons=0x%08lX", static_cast<unsigned long>(state.buttons));
  printPressedButtons(state.buttons);
  Serial.println();
  #endif
}

void printHexReport(const uint8_t *data, const size_t length) {
  Serial.print("raw:");
  for (size_t i = 0; i < length; ++i) {
    Serial.printf(" %02X", data[i]);
  }
  Serial.println();
}

void onInputReport(hid_host_device_handle_t device_handle) {
  InputCallbackScratch &scratch = g_input_callback_scratch;
  size_t data_length = 0;

  const esp_err_t err = hid_host_device_get_raw_input_report_data(
      device_handle, scratch.report_data, sizeof(scratch.report_data),
      &data_length);
  if (err != ESP_OK) {
    return;
  }

  scratch.layout = ParsedLayout{};
  if (!app::copyTrackedDeviceLayout(device_handle, &scratch.layout)) {
    return;
  }

  scratch.state = GamepadState{};
  if (decodeInputReport(scratch.layout, scratch.report_data, data_length,
                        &scratch.state)) {
    bool changed = false;
    if (!app::updateTrackedDeviceState(device_handle, scratch.state, &changed)) {
      return;
    }
    return;
  }
}

void handleConnectedDevice(hid_host_device_handle_t device_handle) {
  hid_host_dev_params_t dev_params = {};
  const esp_err_t params_err =
      hid_host_device_get_params(device_handle, &dev_params);
  if (params_err != ESP_OK) {
    Serial.printf("hid_host_device_get_params failed: %s\n",
                  esp_err_to_name(params_err));
    return;
  }

  if (!app::reserveDeviceSlot(device_handle, dev_params)) {
    Serial.println("No free device slots");
    return;
  }

  Serial.printf("HID connected: addr=%u iface=%u subclass=0x%02X proto=%s\n",
                dev_params.addr, dev_params.iface_num, dev_params.sub_class,
                app::protocolName(dev_params.proto));

  const hid_host_device_config_t dev_config = {
      .callback = hidHostInterfaceCallback,
      .callback_arg = nullptr,
  };

  const esp_err_t open_err = hid_host_device_open(device_handle, &dev_config);
  if (open_err != ESP_OK) {
    Serial.printf("hid_host_device_open failed: %s\n",
                  esp_err_to_name(open_err));
    app::clearTrackedDevice(device_handle);
    return;
  }

  if (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
    const esp_err_t protocol_err = hid_class_request_set_protocol(
        device_handle, HID_REPORT_PROTOCOL_BOOT);
    if (protocol_err != ESP_OK) {
      Serial.printf("set_protocol failed: %s\n",
                    esp_err_to_name(protocol_err));
    }
    if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
      const esp_err_t idle_err = hid_class_request_set_idle(device_handle, 0, 0);
      if (idle_err != ESP_OK) {
        Serial.printf("set_idle failed: %s\n", esp_err_to_name(idle_err));
      }
    }
  }

  size_t report_desc_len = 0;
  uint8_t *report_desc =
      hid_host_get_report_descriptor(device_handle, &report_desc_len);
  ParsedLayout parsed_layout = {};
  if (report_desc != nullptr) {
    Serial.printf("Report descriptor length: %u bytes\n",
                  static_cast<unsigned>(report_desc_len));
    parseReportDescriptor(report_desc, report_desc_len, &parsed_layout);
    printParsedLayout(parsed_layout);
  } else {
    Serial.println("Report descriptor unavailable");
  }

  app::updateTrackedDeviceLayout(device_handle, dev_params, parsed_layout);

  const esp_err_t start_err = hid_host_device_start(device_handle);
  if (start_err != ESP_OK) {
    Serial.printf("hid_host_device_start failed: %s\n",
                  esp_err_to_name(start_err));
    hid_host_device_close(device_handle);
    app::clearTrackedDevice(device_handle);
  }
}

void hidHostTask(void *parameter) {
  (void)parameter;

  HidDriverEvent driver_event = {};
  while (true) {
    if (xQueueReceive(g_hid_event_queue, &driver_event, portMAX_DELAY) ==
        pdTRUE) {
      if (driver_event.event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        handleConnectedDevice(driver_event.device_handle);
      }
    }
  }
}

void usbLibTask(void *parameter) {
  const usb_host_config_t host_config = {
      .skip_phy_setup = false,
      .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };

  ESP_ERROR_CHECK(usb_host_install(&host_config));
  xTaskNotifyGive(static_cast<TaskHandle_t>(parameter));

  while (true) {
    uint32_t event_flags = 0;
    usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
      Serial.println("USB host: NO_CLIENTS");
    }
    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
      Serial.println("USB host: ALL_FREE");
    }
  }
}

}  // namespace

extern "C" void hidHostInterfaceCallback(
    hid_host_device_handle_t device_handle,
    const hid_host_interface_event_t event, void *arg) {
  (void)arg;

  hid_host_dev_params_t dev_params = {};
  if (hid_host_device_get_params(device_handle, &dev_params) != ESP_OK) {
    return;
  }

  switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
      onInputReport(device_handle);
      break;

    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
      Serial.printf("HID disconnected, proto=%s\n",
                    app::protocolName(dev_params.proto));
      app::clearTrackedDevice(device_handle);
      hid_host_device_close(device_handle);
      break;

    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
      Serial.printf("HID transfer error, proto=%s\n",
                    app::protocolName(dev_params.proto));
      break;
  }
}

extern "C" void hidHostDeviceCallback(
    hid_host_device_handle_t device_handle, const hid_host_driver_event_t event,
    void *arg) {
  HidDriverEvent driver_event = {
      .device_handle = device_handle,
      .event = event,
      .arg = arg,
  };

  if (g_hid_event_queue != nullptr) {
    xQueueSend(g_hid_event_queue, &driver_event, 0);
  }
}

namespace app {

bool startUsbHostJoypad() {
  g_hid_event_queue = xQueueCreate(10, sizeof(HidDriverEvent));
  if (g_hid_event_queue == nullptr) {
    Serial.println("Queue alloc failed");
    return false;
  }

  const BaseType_t usb_task_ok = xTaskCreatePinnedToCore(
      usbLibTask, "usb_events", kUsbLibTaskStackSize,
      xTaskGetCurrentTaskHandle(), kUsbLibTaskPriority, nullptr, 0);
  if (usb_task_ok != pdPASS) {
    Serial.println("USB library task creation failed");
    return false;
  }

  const uint32_t notified = ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(1000));
  if (notified == 0) {
    Serial.println("USB library start timeout");
    return false;
  }

  const hid_host_driver_config_t hid_config = {
      .create_background_task = true,
      .task_priority = kHidBackgroundTaskPriority,
      .stack_size = kHidBackgroundTaskStackSize,
      .core_id = 0,
      .callback = hidHostDeviceCallback,
      .callback_arg = nullptr,
  };

  const esp_err_t install_err = hid_host_install(&hid_config);
  if (install_err != ESP_OK) {
    Serial.printf("hid_host_install failed: %s\n",
                  esp_err_to_name(install_err));
    return false;
  }

  const BaseType_t hid_task_ok =
      xTaskCreate(hidHostTask, "hid_task", kHidDispatchTaskStackSize, nullptr,
                  kHidDispatchTaskPriority, nullptr);
  if (hid_task_ok != pdPASS) {
    Serial.println("HID host task creation failed");
    return false;
  }

  return true;
}

}  // namespace app
