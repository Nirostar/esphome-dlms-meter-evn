#include "espdm.h"
#include "espdm_mbus.h"
#include "espdm_dlms.h"
#include "espdm_obis.h"

#if defined(ESP8266)
#include <bearssl/bearssl.h>
#endif

namespace esphome {
namespace espdm {

static const char *const TAG = "espdm";
static const char *const ESPDM_VERSION = "0.9.1-extcomp";

void DlmsMeter::setup() {
  ESP_LOGI(TAG, "DLMS smart meter component v%s started", ESPDM_VERSION);
}

void DlmsMeter::dump_config() {
  ESP_LOGCONFIG(TAG, "DLMS Meter:");
  ESP_LOGCONFIG(TAG, "  Decryption key length: %u bytes", static_cast<unsigned>(this->key_length_));
#ifdef USE_MQTT
  if (this->mqtt_client_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  MQTT topic: %s", this->mqtt_topic_.c_str());
  }
#endif
  this->check_uart_settings(2400);
}

void DlmsMeter::loop() {
  // Drain whatever bytes the UART has buffered right now without blocking.
  // The full frame (~250 bytes at 2400 baud, ~1 s wire time) is assembled
  // across many loop() calls; end-of-frame is detected by the silence check
  // below. The old implementation called delay(10) per byte to coalesce
  // reads, which made a single loop() call block for ~2.5 s and tripped
  // ESPHome's "operation took a long time" watchdog.
  unsigned long currentTime = millis();

  uint8_t c;
  while (this->available()) {
    if (!this->read_byte(&c)) {
      break;
    }
    this->receive_buffer_.push_back(c);
    this->last_read_ = currentTime;
  }

  if (this->receive_buffer_.empty() || currentTime - this->last_read_ <= static_cast<unsigned long>(this->read_timeout_)) {
    return;
  }

  log_packet_(this->receive_buffer_);

  // Verify and parse M-Bus frames
  ESP_LOGV(TAG, "Parsing M-Bus frames");

  uint16_t frameOffset = 0;
  std::vector<uint8_t> mbusPayload;

  while (true) {
    ESP_LOGV(TAG, "MBUS: Parsing frame");

    if (this->receive_buffer_[frameOffset + MBUS_START1_OFFSET] != 0x68 ||
        this->receive_buffer_[frameOffset + MBUS_START2_OFFSET] != 0x68) {
      ESP_LOGE(TAG, "MBUS: Start bytes do not match");
      return abort_frame_();
    }

    if (this->receive_buffer_[frameOffset + MBUS_LENGTH1_OFFSET] !=
        this->receive_buffer_[frameOffset + MBUS_LENGTH2_OFFSET]) {
      ESP_LOGE(TAG, "MBUS: Length bytes do not match");
      return abort_frame_();
    }

    uint8_t frameLength = this->receive_buffer_[frameOffset + MBUS_LENGTH1_OFFSET];

    if (this->receive_buffer_.size() - frameOffset < static_cast<size_t>(frameLength + 3)) {
      ESP_LOGE(TAG, "MBUS: Frame too big for received data");
      return abort_frame_();
    }

    if (this->receive_buffer_[frameOffset + frameLength + MBUS_HEADER_INTRO_LENGTH + MBUS_FOOTER_LENGTH - 1] != 0x16) {
      ESP_LOGE(TAG, "MBUS: Invalid stop byte");
      return abort_frame_();
    }

    mbusPayload.insert(mbusPayload.end(),
                       &this->receive_buffer_[frameOffset + MBUS_FULL_HEADER_LENGTH],
                       &this->receive_buffer_[frameOffset + MBUS_HEADER_INTRO_LENGTH + frameLength]);

    frameOffset += MBUS_HEADER_INTRO_LENGTH + frameLength + MBUS_FOOTER_LENGTH;

    if (frameOffset >= this->receive_buffer_.size()) {
      break;
    }
  }

  // Verify and parse DLMS header
  ESP_LOGV(TAG, "Parsing DLMS header");

  if (mbusPayload.size() < 20) {
    ESP_LOGE(TAG, "DLMS: Payload too short");
    return abort_frame_();
  }

  if (mbusPayload[DLMS_CIPHER_OFFSET] != 0xDB) {
    ESP_LOGE(TAG, "DLMS: Unsupported cipher");
    return abort_frame_();
  }

  uint8_t systitleLength = mbusPayload[DLMS_SYST_OFFSET];

  if (systitleLength != 0x08) {
    ESP_LOGE(TAG, "DLMS: Unsupported system title length");
    return abort_frame_();
  }

  uint16_t messageLength = mbusPayload[DLMS_LENGTH_OFFSET];
  int headerOffset = 0;

  if (messageLength == 0x82) {
    ESP_LOGV(TAG, "DLMS: Message length > 127");

    memcpy(&messageLength, &mbusPayload[DLMS_LENGTH_OFFSET + 1], 2);
    messageLength = swap_uint16(messageLength);

    headerOffset = DLMS_HEADER_EXT_OFFSET;
  } else {
    ESP_LOGV(TAG, "DLMS: Message length <= 127");
  }

  messageLength -= DLMS_LENGTH_CORRECTION;

  if (mbusPayload.size() - DLMS_HEADER_LENGTH - headerOffset != messageLength) {
    ESP_LOGE(TAG, "DLMS: Message has invalid length. Expected %u/Actual: %u",
             static_cast<unsigned>(mbusPayload.size() - DLMS_HEADER_LENGTH - headerOffset),
             static_cast<unsigned>(messageLength));
    return abort_frame_();
  }

  if (mbusPayload[headerOffset + DLMS_SECBYTE_OFFSET] != 0x21 &&
      mbusPayload[headerOffset + DLMS_SECBYTE_OFFSET] != 0x20) {
    ESP_LOGE(TAG, "DLMS: Unsupported security control byte");
    return abort_frame_();
  }

  // Decryption
  ESP_LOGV(TAG, "Decrypting payload");

  uint8_t iv[12];
  memcpy(&iv[0], &mbusPayload[DLMS_SYST_OFFSET + 1], systitleLength);
  memcpy(&iv[8], &mbusPayload[headerOffset + DLMS_FRAMECOUNTER_OFFSET], DLMS_FRAMECOUNTER_LENGTH);

  std::vector<uint8_t> plaintext_vec(messageLength);
  uint8_t *plaintext = plaintext_vec.data();

#if defined(ESP8266)
  memcpy(plaintext, &mbusPayload[headerOffset + DLMS_PAYLOAD_OFFSET], messageLength);
  br_gcm_context gcmCtx;
  br_aes_ct_ctr_keys bc;
  br_aes_ct_ctr_init(&bc, this->key_, this->key_length_);
  br_gcm_init(&gcmCtx, &bc.vtable, br_ghash_ctmul32);
  br_gcm_reset(&gcmCtx, iv, sizeof(iv));
  br_gcm_flip(&gcmCtx);
  br_gcm_run(&gcmCtx, 0, plaintext, messageLength);
#elif defined(USE_ESP32)
  esp_aes_gcm_init(&this->aes_);
  esp_aes_gcm_setkey(&this->aes_, MBEDTLS_CIPHER_ID_AES, this->key_, this->key_length_ * 8);

  esp_aes_gcm_auth_decrypt(&this->aes_, messageLength, iv, sizeof(iv), NULL, 0, NULL, 0,
                           &mbusPayload[headerOffset + DLMS_PAYLOAD_OFFSET], plaintext);

  esp_aes_gcm_free(&this->aes_);
#else
#error "Invalid Platform"
#endif

  if (plaintext[0] != 0x0F || plaintext[5] != 0x0C) {
    ESP_LOGE(TAG, "OBIS: Packet was decrypted but data is invalid");
    return abort_frame_();
  }

  // Decoding
  ESP_LOGV(TAG, "Decoding");
  ESP_LOGV(TAG, "Plaintext Packet:");
  log_packet_(std::vector<uint8_t>(plaintext, plaintext + messageLength));
  int currentPosition = DECODER_START_OFFSET;

  do {
    ESP_LOGV(TAG, "currentPosition: %d", currentPosition);
    ESP_LOGV(TAG, "OBIS header type: %d", plaintext[currentPosition + OBIS_TYPE_OFFSET]);
    if (plaintext[currentPosition + OBIS_TYPE_OFFSET] != DataType::OctetString) {
      ESP_LOGE(TAG, "Unsupported OBIS header type");
      return abort_frame_();
    }

    uint8_t obisCodeLength = plaintext[currentPosition + OBIS_LENGTH_OFFSET];
    ESP_LOGV(TAG, "OBIS code/header length: %d", obisCodeLength);

    if (obisCodeLength != 0x06 && obisCodeLength != 0x0C) {
      ESP_LOGE(TAG, "Unsupported OBIS header length");
      return abort_frame_();
    }

    std::vector<uint8_t> obisCodeVec(obisCodeLength);
    uint8_t *obisCode = obisCodeVec.data();
    memcpy(&obisCode[0], &plaintext[currentPosition + OBIS_CODE_OFFSET], obisCodeLength);

    bool timestampFound = false;
    bool meterNumberFound = false;
    if ((obisCodeLength == 0x0C) && (currentPosition == DECODER_START_OFFSET)) {
      timestampFound = true;
    } else if ((currentPosition != DECODER_START_OFFSET) && plaintext[currentPosition - 1] == 0xFF) {
      meterNumberFound = true;
    } else {
      currentPosition += obisCodeLength + 2;
    }

    uint8_t dataType = plaintext[currentPosition];
    currentPosition++;

    uint8_t dataLength = 0x00;

    CodeType codeType = CodeType::Unknown;

    ESP_LOGV(TAG, "obisCode (OBIS_A): %d", obisCode[OBIS_A]);
    ESP_LOGV(TAG, "currentPosition: %d", currentPosition);

    if (obisCode[OBIS_A] == Medium::Electricity) {
      if (memcmp(&obisCode[OBIS_C], ESPDM_VOLTAGE_L1, 2) == 0) {
        codeType = CodeType::VoltageL1;
      } else if (memcmp(&obisCode[OBIS_C], ESPDM_VOLTAGE_L2, 2) == 0) {
        codeType = CodeType::VoltageL2;
      } else if (memcmp(&obisCode[OBIS_C], ESPDM_VOLTAGE_L3, 2) == 0) {
        codeType = CodeType::VoltageL3;
      } else if (memcmp(&obisCode[OBIS_C], ESPDM_CURRENT_L1, 2) == 0) {
        codeType = CodeType::CurrentL1;
      } else if (memcmp(&obisCode[OBIS_C], ESPDM_CURRENT_L2, 2) == 0) {
        codeType = CodeType::CurrentL2;
      } else if (memcmp(&obisCode[OBIS_C], ESPDM_CURRENT_L3, 2) == 0) {
        codeType = CodeType::CurrentL3;
      } else if (memcmp(&obisCode[OBIS_C], ESPDM_ACTIVE_POWER_PLUS, 2) == 0) {
        codeType = CodeType::ActivePowerPlus;
      } else if (memcmp(&obisCode[OBIS_C], ESPDM_ACTIVE_POWER_MINUS, 2) == 0) {
        codeType = CodeType::ActivePowerMinus;
      } else if (memcmp(&obisCode[OBIS_C], ESPDM_POWER_FACTOR, 2) == 0) {
        codeType = CodeType::PowerFactor;
      } else if (memcmp(&obisCode[OBIS_C], ESPDM_ACTIVE_ENERGY_PLUS, 2) == 0) {
        codeType = CodeType::ActiveEnergyPlus;
      } else if (memcmp(&obisCode[OBIS_C], ESPDM_ACTIVE_ENERGY_MINUS, 2) == 0) {
        codeType = CodeType::ActiveEnergyMinus;
      } else if (memcmp(&obisCode[OBIS_C], ESPDM_REACTIVE_ENERGY_PLUS, 2) == 0) {
        codeType = CodeType::ReactiveEnergyPlus;
      } else if (memcmp(&obisCode[OBIS_C], ESPDM_REACTIVE_ENERGY_MINUS, 2) == 0) {
        codeType = CodeType::ReactiveEnergyMinus;
      } else {
        ESP_LOGW(TAG, "Unsupported OBIS code OBIS_C: %d, OBIS_D: %d", obisCode[OBIS_C], obisCode[OBIS_D]);
      }
    } else if (obisCode[OBIS_A] == Medium::Abstract) {
      if (memcmp(&obisCode[OBIS_C], ESPDM_TIMESTAMP, 2) == 0) {
        codeType = CodeType::Timestamp;
      } else if (memcmp(&obisCode[OBIS_C], ESPDM_SERIAL_NUMBER, 2) == 0) {
        codeType = CodeType::SerialNumber;
      } else if (memcmp(&obisCode[OBIS_C], ESPDM_DEVICE_NAME, 2) == 0) {
        codeType = CodeType::DeviceName;
      } else {
        ESP_LOGW(TAG, "Unsupported OBIS code OBIS_C: %d, OBIS_D: %d", obisCode[OBIS_C], obisCode[OBIS_D]);
      }
    } else if (timestampFound == true) {
      ESP_LOGV(TAG, "Found Timestamp without obisMedium");
      codeType = CodeType::Timestamp;
    } else if (meterNumberFound == true) {
      ESP_LOGV(TAG, "Found MeterNumber without obisMedium");
      codeType = CodeType::MeterNumber;
    } else {
      ESP_LOGE(TAG, "Unsupported OBIS medium");
      return abort_frame_();
    }

    uint16_t uint16Value;
    uint32_t uint32Value;
    float floatValue;

    switch (dataType) {
      case DataType::DoubleLongUnsigned:
        dataLength = 4;

        memcpy(&uint32Value, &plaintext[currentPosition], 4);
        uint32Value = swap_uint32(uint32Value);

        floatValue = uint32Value;

        if (codeType == CodeType::ActivePowerPlus && this->active_power_plus_ != nullptr &&
            this->active_power_plus_->state != floatValue)
          this->active_power_plus_->publish_state(floatValue);
        else if (codeType == CodeType::ActivePowerMinus && this->active_power_minus_ != nullptr &&
                 this->active_power_minus_->state != floatValue)
          this->active_power_minus_->publish_state(floatValue);

        else if (codeType == CodeType::ActiveEnergyPlus && this->active_energy_plus_ != nullptr &&
                 this->active_energy_plus_->state != floatValue)
          this->active_energy_plus_->publish_state(floatValue);
        else if (codeType == CodeType::ActiveEnergyMinus && this->active_energy_minus_ != nullptr &&
                 this->active_energy_minus_->state != floatValue)
          this->active_energy_minus_->publish_state(floatValue);

        else if (codeType == CodeType::ReactiveEnergyPlus && this->reactive_energy_plus_ != nullptr &&
                 this->reactive_energy_plus_->state != floatValue)
          this->reactive_energy_plus_->publish_state(floatValue);
        else if (codeType == CodeType::ReactiveEnergyMinus && this->reactive_energy_minus_ != nullptr &&
                 this->reactive_energy_minus_->state != floatValue)
          this->reactive_energy_minus_->publish_state(floatValue);

        break;
      case DataType::LongUnsigned:
        dataLength = 2;

        memcpy(&uint16Value, &plaintext[currentPosition], 2);
        uint16Value = swap_uint16(uint16Value);

        if (plaintext[currentPosition + 5] == Accuracy::SingleDigit)
          floatValue = uint16Value / 10.0;
        else if (plaintext[currentPosition + 5] == Accuracy::DoubleDigit)
          floatValue = uint16Value / 100.0;
        else
          floatValue = uint16Value;

        if (codeType == CodeType::VoltageL1 && this->voltage_l1_ != nullptr &&
            this->voltage_l1_->state != floatValue)
          this->voltage_l1_->publish_state(floatValue);
        else if (codeType == CodeType::VoltageL2 && this->voltage_l2_ != nullptr &&
                 this->voltage_l2_->state != floatValue)
          this->voltage_l2_->publish_state(floatValue);
        else if (codeType == CodeType::VoltageL3 && this->voltage_l3_ != nullptr &&
                 this->voltage_l3_->state != floatValue)
          this->voltage_l3_->publish_state(floatValue);

        else if (codeType == CodeType::CurrentL1 && this->current_l1_ != nullptr &&
                 this->current_l1_->state != floatValue)
          this->current_l1_->publish_state(floatValue);
        else if (codeType == CodeType::CurrentL2 && this->current_l2_ != nullptr &&
                 this->current_l2_->state != floatValue)
          this->current_l2_->publish_state(floatValue);
        else if (codeType == CodeType::CurrentL3 && this->current_l3_ != nullptr &&
                 this->current_l3_->state != floatValue)
          this->current_l3_->publish_state(floatValue);

        else if (codeType == CodeType::PowerFactor && this->power_factor_ != nullptr &&
                 this->power_factor_->state != floatValue)
          this->power_factor_->publish_state(floatValue / 1000.0);

        break;
      case DataType::OctetString:
        ESP_LOGV(TAG, "Arrived on OctetString");
        ESP_LOGV(TAG, "currentPosition: %d, plaintext: %d", currentPosition, plaintext[currentPosition]);

        dataLength = plaintext[currentPosition];
        currentPosition++;

        if (codeType == CodeType::Timestamp && this->timestamp_ != nullptr) {
          char timestamp[27];

          uint16_t year;
          uint8_t month;
          uint8_t day;

          uint8_t hour;
          uint8_t minute;
          uint8_t second;

          memcpy(&uint16Value, &plaintext[currentPosition], 2);
          year = swap_uint16(uint16Value);

          memcpy(&month, &plaintext[currentPosition + 2], 1);
          memcpy(&day, &plaintext[currentPosition + 3], 1);

          memcpy(&hour, &plaintext[currentPosition + 5], 1);
          memcpy(&minute, &plaintext[currentPosition + 6], 1);
          memcpy(&second, &plaintext[currentPosition + 7], 1);

          sprintf(timestamp, "%04u-%02u-%02uT%02u:%02u:%02uZ", year, month, day, hour, minute, second);

          this->timestamp_->publish_state(timestamp);
        } else if (codeType == CodeType::MeterNumber && this->meternumber_ != nullptr) {
          ESP_LOGV(TAG, "Constructing MeterNumber...");
          char meterNumber[13];

          memcpy(meterNumber, &plaintext[currentPosition], dataLength);
          meterNumber[12] = '\0';

          this->meternumber_->publish_state(meterNumber);
        }

        break;
      default:
        ESP_LOGE(TAG, "Unsupported OBIS data type");
        return abort_frame_();
    }

    currentPosition += dataLength;

    if (timestampFound == false) {
      currentPosition += 2;
    }

    if (plaintext[currentPosition] == 0x0F) {
      // on EVN Meters the additional data (no additional Break) is only 3 Bytes + 1 Byte for the "there is data" Byte
      currentPosition += 4;
    }
  } while (currentPosition < messageLength);

  this->receive_buffer_.clear();

  ESP_LOGI(TAG, "Received valid data");

#ifdef USE_MQTT
  if (this->mqtt_client_ != nullptr) {
    this->mqtt_client_->publish_json(this->mqtt_topic_, [this](JsonObject root) {
      if (this->voltage_l1_ != nullptr) {
        root["voltage_l1"] = this->voltage_l1_->state;
        root["voltage_l2"] = this->voltage_l2_->state;
        root["voltage_l3"] = this->voltage_l3_->state;
      }
      if (this->current_l1_ != nullptr) {
        root["current_l1"] = this->current_l1_->state;
        root["current_l2"] = this->current_l2_->state;
        root["current_l3"] = this->current_l3_->state;
      }
      if (this->active_power_plus_ != nullptr) {
        root["active_power_plus"] = this->active_power_plus_->state;
        root["active_power_minus"] = this->active_power_minus_->state;
      }
      if (this->active_energy_plus_ != nullptr) {
        root["active_energy_plus"] = this->active_energy_plus_->state;
        root["active_energy_minus"] = this->active_energy_minus_->state;
      }
      if (this->reactive_energy_plus_ != nullptr) {
        root["reactive_energy_plus"] = this->reactive_energy_plus_->state;
        root["reactive_energy_minus"] = this->reactive_energy_minus_->state;
      }
      if (this->timestamp_ != nullptr) {
        root["timestamp"] = this->timestamp_->state;
      }
      if (this->power_factor_ != nullptr) {
        root["power_factor"] = this->power_factor_->state;
      }
      if (this->meternumber_ != nullptr) {
        root["meternumber"] = this->meternumber_->state;
      }
    });
  }
#endif
}

void DlmsMeter::abort_frame_() { this->receive_buffer_.clear(); }

uint16_t DlmsMeter::swap_uint16(uint16_t val) { return (val << 8) | (val >> 8); }

uint32_t DlmsMeter::swap_uint32(uint32_t val) {
  val = ((val << 8) & 0xFF00FF00) | ((val >> 8) & 0xFF00FF);
  return (val << 16) | (val >> 16);
}

void DlmsMeter::set_key(const uint8_t *key, size_t key_length) {
  if (key_length > sizeof(this->key_))
    key_length = sizeof(this->key_);
  memcpy(&this->key_[0], &key[0], key_length);
  this->key_length_ = key_length;
}

void DlmsMeter::set_key_hex(const std::string &key_hex) {
  std::string cleaned;
  cleaned.reserve(key_hex.size());
  for (char c : key_hex) {
    if (c == ':' || c == ' ' || c == '-')
      continue;
    cleaned.push_back(c);
  }

  size_t bytes = cleaned.size() / 2;
  if (bytes > sizeof(this->key_))
    bytes = sizeof(this->key_);

  for (size_t i = 0; i < bytes; i++) {
    auto hex_nibble = [](char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      return 0;
    };
    this->key_[i] =
        static_cast<uint8_t>((hex_nibble(cleaned[i * 2]) << 4) | hex_nibble(cleaned[i * 2 + 1]));
  }
  this->key_length_ = bytes;
}

void DlmsMeter::set_voltage_sensors(sensor::Sensor *voltage_l1, sensor::Sensor *voltage_l2,
                                    sensor::Sensor *voltage_l3) {
  this->voltage_l1_ = voltage_l1;
  this->voltage_l2_ = voltage_l2;
  this->voltage_l3_ = voltage_l3;
}

void DlmsMeter::set_current_sensors(sensor::Sensor *current_l1, sensor::Sensor *current_l2,
                                    sensor::Sensor *current_l3) {
  this->current_l1_ = current_l1;
  this->current_l2_ = current_l2;
  this->current_l3_ = current_l3;
}

void DlmsMeter::set_active_power_sensors(sensor::Sensor *active_power_plus, sensor::Sensor *active_power_minus,
                                         sensor::Sensor *power_factor) {
  this->active_power_plus_ = active_power_plus;
  this->active_power_minus_ = active_power_minus;
  this->power_factor_ = power_factor;
}

void DlmsMeter::set_active_energy_sensors(sensor::Sensor *active_energy_plus, sensor::Sensor *active_energy_minus) {
  this->active_energy_plus_ = active_energy_plus;
  this->active_energy_minus_ = active_energy_minus;
}

void DlmsMeter::set_reactive_energy_sensors(sensor::Sensor *reactive_energy_plus,
                                            sensor::Sensor *reactive_energy_minus) {
  this->reactive_energy_plus_ = reactive_energy_plus;
  this->reactive_energy_minus_ = reactive_energy_minus;
}

void DlmsMeter::set_timestamp_sensor(text_sensor::TextSensor *timestamp) { this->timestamp_ = timestamp; }

void DlmsMeter::set_meternumber_sensor(text_sensor::TextSensor *meternumber) { this->meternumber_ = meternumber; }

#ifdef USE_MQTT
void DlmsMeter::enable_mqtt(mqtt::MQTTClientComponent *mqtt_client, const std::string &topic) {
  this->mqtt_client_ = mqtt_client;
  this->mqtt_topic_ = topic;
}
#endif

void DlmsMeter::log_packet_(const std::vector<uint8_t> &data) {
  ESP_LOGV(TAG, "%s", format_hex_pretty(data).c_str());
}

}  // namespace espdm
}  // namespace esphome
