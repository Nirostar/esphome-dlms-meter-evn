#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#endif

#if defined(USE_ESP32)
// ESP-IDF replaces mbedtls_gcm_* with hardware-accelerated esp_aes_gcm_* when
// MBEDTLS_HARDWARE_AES is enabled (default), so we call the ESP port API.
#include "aes/esp_aes_gcm.h"
#include "mbedtls/cipher.h"
#elif defined(USE_ESP8266)
#include "mbedtls/gcm.h"
#endif

namespace esphome {
namespace espdm {

class DlmsMeter : public Component, public uart::UARTDevice {
 public:
  DlmsMeter() = default;

  void setup() override;
  void loop() override;
  void dump_config() override;

  void set_voltage_sensors(sensor::Sensor *voltage_l1, sensor::Sensor *voltage_l2, sensor::Sensor *voltage_l3);
  void set_current_sensors(sensor::Sensor *current_l1, sensor::Sensor *current_l2, sensor::Sensor *current_l3);

  void set_active_power_sensors(sensor::Sensor *active_power_plus, sensor::Sensor *active_power_minus,
                                sensor::Sensor *power_factor);
  void set_active_energy_sensors(sensor::Sensor *active_energy_plus, sensor::Sensor *active_energy_minus);
  void set_reactive_energy_sensors(sensor::Sensor *reactive_energy_plus, sensor::Sensor *reactive_energy_minus);
  void set_timestamp_sensor(text_sensor::TextSensor *timestamp);
  void set_meternumber_sensor(text_sensor::TextSensor *meternumber);

  void set_key(const uint8_t *key, size_t key_length);
  void set_key_hex(const std::string &key_hex);

#ifdef USE_MQTT
  void enable_mqtt(mqtt::MQTTClientComponent *mqtt_client, const std::string &topic);
#endif

 protected:
  std::vector<uint8_t> receive_buffer_;
  unsigned long last_read_{0};
  int read_timeout_{1000};

  uint8_t key_[16]{};
  size_t key_length_{0};

#if defined(USE_ESP32)
  esp_gcm_context aes_{};
#endif

  sensor::Sensor *voltage_l1_{nullptr};
  sensor::Sensor *voltage_l2_{nullptr};
  sensor::Sensor *voltage_l3_{nullptr};

  sensor::Sensor *current_l1_{nullptr};
  sensor::Sensor *current_l2_{nullptr};
  sensor::Sensor *current_l3_{nullptr};

  sensor::Sensor *active_power_plus_{nullptr};
  sensor::Sensor *active_power_minus_{nullptr};
  sensor::Sensor *power_factor_{nullptr};

  sensor::Sensor *active_energy_plus_{nullptr};
  sensor::Sensor *active_energy_minus_{nullptr};

  sensor::Sensor *reactive_energy_plus_{nullptr};
  sensor::Sensor *reactive_energy_minus_{nullptr};

  text_sensor::TextSensor *timestamp_{nullptr};
  text_sensor::TextSensor *meternumber_{nullptr};

#ifdef USE_MQTT
  mqtt::MQTTClientComponent *mqtt_client_{nullptr};
  std::string mqtt_topic_;
#endif

  static uint16_t swap_uint16(uint16_t val);
  static uint32_t swap_uint32(uint32_t val);
  void log_packet_(const std::vector<uint8_t> &data);
  void abort_frame_();
};

}  // namespace espdm
}  // namespace esphome
