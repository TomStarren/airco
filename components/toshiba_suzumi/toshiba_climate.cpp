#include "toshiba_climate.h"
#include "toshiba_climate_mode.h"
#include "esphome/core/log.h"

namespace esphome {
namespace toshiba_suzumi {

using namespace esphome::climate;

static const int RECEIVE_TIMEOUT = 200;
static const int COMMAND_DELAY = 100;

/**
 * Checksum is calculated from all bytes excluding start byte.
 * It's (256 - (sum % 256)).
 */
uint8_t checksum(std::vector<uint8_t> data, uint8_t length) {
  uint8_t sum = 0;
  for (size_t i = 1; i < length; i++) {
    sum += data[i];
  }
  return 256 - sum;
}

/**
 * Send the command to UART interface.
 */
void ToshibaClimateUart::send_to_uart(ToshibaCommand command) {
  this->last_command_timestamp_ = millis();
  ESP_LOGV(TAG, "Sending: [%s]", format_hex_pretty(command.payload).c_str());
  this->write_array(command.payload);
}

/**
 * Send starting handshake to initialize communication with the unit.
 */
void ToshibaClimateUart::start_handshake() {
  ESP_LOGCONFIG(TAG, "Sending handshake...");
  enqueue_command_(ToshibaCommand{.cmd = ToshibaCommandType::HANDSHAKE, .payload = HANDSHAKE[0]});
  enqueue_command_(ToshibaCommand{.cmd = ToshibaCommandType::HANDSHAKE, .payload = HANDSHAKE[1]});
  enqueue_command_(ToshibaCommand{.cmd = ToshibaCommandType::HANDSHAKE, .payload = HANDSHAKE[2]});
  enqueue_command_(ToshibaCommand{.cmd = ToshibaCommandType::HANDSHAKE, .payload = HANDSHAKE[3]});
  enqueue_command_(ToshibaCommand{.cmd = ToshibaCommandType::HANDSHAKE, .payload = HANDSHAKE[4]});
  enqueue_command_(ToshibaCommand{.cmd = ToshibaCommandType::HANDSHAKE, .payload = HANDSHAKE[5]});
  enqueue_command_(ToshibaCommand{.cmd = ToshibaCommandType::DELAY, .delay = 2000});
  enqueue_command_(ToshibaCommand{.cmd = ToshibaCommandType::HANDSHAKE, .payload = AFTER_HANDSHAKE[0]});
  enqueue_command_(ToshibaCommand{.cmd = ToshibaCommandType::HANDSHAKE, .payload = AFTER_HANDSHAKE[1]});
}

/**
 * Handle data in RX buffer, validate message for content and checksum.
 * Since we know the format only of some messages (expected length), unknown messages
 * are ended via RECIEVE timeout.
 */
bool ToshibaClimateUart::validate_message_() {
  uint8_t at = this->rx_message_.size() - 1;
  auto *data = &this->rx_message_[0];
  uint8_t new_byte = data[at];

  // Byte 0: HEADER (always 0x02)
  if (at == 0)
    return new_byte == 0x02;

  // always get first three bytes
  if (at < 2) {
    return true;
  }

  // Byte 3
  if (data[2] != 0x03) {
    // Normal commands starts with 0x02 0x00 0x03 and have length between 15-17 bytes.
    // however there are some special unknown handshake commands which has non-standard replies.
    // Since we don't know their format, we can't validate them.
    return true;
  }

  if (at <= 5) {
    // no validation for these fields
    return true;
  }

  // Byte 7: LENGTH
  uint8_t length = 6 + data[6] + 1;  // prefix + data + checksum

  // wait until all data is read
  if (at < length)
    return true;

  // last byte: CHECKSUM
  uint8_t rx_checksum = new_byte;
  uint8_t calc_checksum = checksum(this->rx_message_, at);

  if (rx_checksum != calc_checksum) {
    ESP_LOGW(TAG, "Received invalid message checksum %02X!=%02X DATA=[%s]", rx_checksum, calc_checksum,
             format_hex_pretty(data, length).c_str());
    return false;
  }

  // valid message
  ESP_LOGV(TAG, "Received: DATA=[%s]", format_hex_pretty(data, length).c_str());
  this->parseResponse(this->rx_message_);

  // return false to reset rx buffer
  return false;
}

void ToshibaClimateUart::enqueue_command_(const ToshibaCommand &command) {
  this->command_queue_.push_back(command);
  this->process_command_queue_();
}

void ToshibaClimateUart::sendCmd(ToshibaCommandType cmd, uint8_t value) {
  std::vector<uint8_t> payload = {2, 0, 3, 16, 0, 0, 7, 1, 48, 1, 0, 2};
  payload.push_back(static_cast<uint8_t>(cmd));
  payload.push_back(value);
  payload.push_back(checksum(payload, payload.size()));
  ESP_LOGD(TAG, "Sending ToshibaCommand: %d, value: %d, checksum: %d", cmd, value, payload[14]);
  this->enqueue_command_(ToshibaCommand{.cmd = cmd, .payload = std::vector<uint8_t>{payload}});
}

void ToshibaClimateUart::requestData(ToshibaCommandType cmd) {
  std::vector<uint8_t> payload = {2, 0, 3, 16, 0, 0, 6, 1, 48, 1, 0, 1};
  payload.push_back(static_cast<uint8_t>(cmd));
  payload.push_back(checksum(payload, payload.size()));
  ESP_LOGD(TAG, "Requesting data from sensor %d, checksum: %d", payload[12], payload[13]);
  this->enqueue_command_(ToshibaCommand{.cmd = cmd, .payload = std::vector<uint8_t>{payload}});
}

void ToshibaClimateUart::getInitData() {
  ESP_LOGD(TAG, "Requesting initial data from AC unit");
  this->requestData(ToshibaCommandType::POWER_STATE);
  this->requestData(ToshibaCommandType::MODE);
  this->requestData(ToshibaCommandType::TARGET_TEMP);
  this->requestData(ToshibaCommandType::FAN);
  this->requestData(ToshibaCommandType::SPECIAL);
  this->requestData(ToshibaCommandType::POWER_SEL);
  this->requestData(ToshibaCommandType::SWING);
  this->requestData(ToshibaCommandType::ROOM_TEMP);
  this->requestData(ToshibaCommandType::OUTDOOR_TEMP);
}

void ToshibaClimateUart::setup() {
  // establish communication
  this->start_handshake();
  // load initial sensor data from the unit
  this->getInitData();
}

/**
 * Detect RX timeout and send next command in the queue to the unit.
 */
void ToshibaClimateUart::process_command_queue_() {
  uint32_t now = millis();
  uint32_t cmdDelay = now - this->last_command_timestamp_;

  // when we have not processed message and timeout since last received byte has expired,
  // we likely won't receive any more data and there is nothing we can do with the message as it's
  // format is was not recognized by validate_message_ function.
  // Nothing to do - drop the message to free up communication and allow to send next command.
  if (now - this->last_rx_char_timestamp_ > RECEIVE_TIMEOUT) {
    this->rx_message_.clear();
  }

  // when there is no RX message and there is a command to send
  if (cmdDelay > COMMAND_DELAY && !this->command_queue_.empty() && this->rx_message_.empty()) {
    auto newCommand = this->command_queue_.front();
    if (newCommand.cmd == ToshibaCommandType::DELAY && cmdDelay < newCommand.delay) {
      // delay command did not finished yet
      return;
    }
    this->send_to_uart(this->command_queue_.front());
    this->command_queue_.erase(this->command_queue_.begin());
  }
}

/**
 * Handle received byte from UART
 */
void ToshibaClimateUart::handle_rx_byte_(uint8_t c) {
  this->rx_message_.push_back(c);
  if (!validate_message_()) {
    this->rx_message_.clear();
  } else {
    this->last_rx_char_timestamp_ = millis();
  }
}

void ToshibaClimateUart::loop() {
  while (available()) {
    uint8_t c;
    this->read_byte(&c);
    this->handle_rx_byte_(c);
  }
  this->process_command_queue_();
}

void ToshibaClimateUart::parseResponse(std::vector<uint8_t> rawData) {
  uint8_t length = rawData.size();
  ToshibaCommandType sensor;
  uint8_t value;

  switch (length) {
    case 15:  // response to requestData with the actual value of sensor/setting
      sensor = static_cast<ToshibaCommandType>(rawData[12]);
      value = rawData[13];
      break;
    case 16:  // probably ACK for issued command
      return;
    case 17:  // response to requestData with the actual value of sensor/setting
      sensor = static_cast<ToshibaCommandType>(rawData[14]);
      value = rawData[15];
      break;
    default:
      return;
  }
  switch (sensor) {
    case ToshibaCommandType::TARGET_TEMP:
      ESP_LOGI(TAG, "Received target temp: %d", value);
      this->target_temperature = value;
      break;
    case ToshibaCommandType::FAN: {
      if (static_cast<FAN>(value) == FAN::AUTO) {
        ESP_LOGI(TAG, "Received fan mode: AUTO");
        this->set_fan_mode_(CLIMATE_FAN_AUTO);
      } else {
        auto fanMode = IntToCustomFanMode(static_cast<FAN>(value));
        ESP_LOGI(TAG, "Received fan mode: %s", fanMode.c_str());
        this->set_custom_fan_mode_(fanMode);
      }
      break;
    }
    case ToshibaCommandType::SPECIAL: {
      //auto specialMode = StringToSpecialMode(static_cast<SPECIAL>(value));
      //ESP_LOGI(TAG, "Received special mode: %s", specialMode.c_str());
      ESP_LOGI(TAG, "Received special mode: BLUB");
      //this->set_custom_preset_(specialMode);
      break;
    }
    case ToshibaCommandType::SWING: {
      auto swingMode = IntToClimateSwingMode(static_cast<SWING>(value));
      ESP_LOGI(TAG, "Received swing mode: %s", climate_swing_mode_to_string(swingMode));
      this->swing_mode = swingMode;
      break;
    }
    case ToshibaCommandType::MODE: {
      auto mode = IntToClimateMode(static_cast<MODE>(value));
      ESP_LOGI(TAG, "Received AC mode: %s", climate_mode_to_string(mode));
      if (this->power_state_ == STATE::ON) {
        this->mode = mode;
      }
      break;
    }
    case ToshibaCommandType::ROOM_TEMP:
      ESP_LOGI(TAG, "Received room temp: %d °C", value);
      this->current_temperature = value;
      break;
    case ToshibaCommandType::OUTDOOR_TEMP:
      if (outdoor_temp_sensor_ != nullptr) {
        ESP_LOGI(TAG, "Received outdoor temp: %d °C", (int8_t) value);
        outdoor_temp_sensor_->publish_state((int8_t) value);
      }
      break;
    case ToshibaCommandType::POWER_SEL: {
      auto pwr_level = IntToPowerLevel(static_cast<PWR_LEVEL>(value));
      ESP_LOGI(TAG, "Received power select: %d", value);
      if (pwr_select_ != nullptr) {
        pwr_select_->publish_state(pwr_level);
      }
      break;
    }
    case ToshibaCommandType::POWER_STATE: {
      auto climateState = static_cast<STATE>(value);
      ESP_LOGI(TAG, "Received AC unit power state: %s", climate_state_to_string(climateState));
      if (climateState == STATE::OFF) {
        this->mode = climate::CLIMATE_MODE_OFF;
      }
      this->power_state_ = climateState;
      break;
    }
    default:
      ESP_LOGW(TAG, "Unknown sensor: %d with value %d", sensor, value);
      break;
  }
  this->rx_message_.clear();  // message processed, clear buffer
  this->publish_state();      // publish current values to MQTT
}

void ToshibaClimateUart::dump_config() {
  ESP_LOGCONFIG(TAG, "ToshibaClimate:");
  LOG_CLIMATE("", "Thermostat", this);
  if (outdoor_temp_sensor_ != nullptr) {
    LOG_SENSOR("", "Outdoor Temp", this->outdoor_temp_sensor_);
  }
  if (pwr_select_ != nullptr) {
    LOG_SELECT("", "Power selector", this->pwr_select_);
  }
}

/**
 * Periodically request room and outdoor temperature.
 * It servers two purposes - updates data and is like "watchdog" because
 * some people reported that without communication, the unit might stop responding.
 */
void ToshibaClimateUart::update() {
  this->requestData(ToshibaCommandType::ROOM_TEMP);
  if (outdoor_temp_sensor_ != nullptr) {
    this->requestData(ToshibaCommandType::OUTDOOR_TEMP);
  }
}

void ToshibaClimateUart::control(const climate::ClimateCall &call) {
  if (call.get_mode().has_value()) {
    ClimateMode mode = *call.get_mode();
    ESP_LOGD(TAG, "Setting mode to %s", climate_mode_to_string(mode));
    if (this->mode == CLIMATE_MODE_OFF && mode != CLIMATE_MODE_OFF) {
      ESP_LOGD(TAG, "Setting AC unit power state to ON.");
      this->sendCmd(ToshibaCommandType::POWER_STATE, static_cast<uint8_t>(STATE::ON));
    }
    if (mode == CLIMATE_MODE_OFF) {
      ESP_LOGD(TAG, "Setting AC unit power state to OFF.");
      this->sendCmd(ToshibaCommandType::POWER_STATE, static_cast<uint8_t>(STATE::OFF));
    } else {
      auto requestedMode = ClimateModeToInt(mode);
      this->sendCmd(ToshibaCommandType::MODE, static_cast<uint8_t>(requestedMode));
    }
    this->mode = mode;
  }

  if (call.get_target_temperature().has_value()) {
    auto target_temp = *call.get_target_temperature();
    uint8_t intTemp = (uint8_t) target_temp;
    ESP_LOGD(TAG, "Setting target temp to %d", intTemp);
    this->target_temperature = target_temp;
    this->sendCmd(ToshibaCommandType::TARGET_TEMP, intTemp);
  }

  if (call.get_fan_mode().has_value()) {
    auto fan_mode = *call.get_fan_mode();
    if (fan_mode == CLIMATE_FAN_AUTO) {
      ESP_LOGD(TAG, "Setting fan mode to %s", climate_fan_mode_to_string(fan_mode));
      this->set_fan_mode_(fan_mode);
      this->sendCmd(ToshibaCommandType::FAN, static_cast<uint8_t>(FAN::AUTO));
    }
  }

  if (call.get_custom_fan_mode().has_value()) {
    auto fan_mode = *call.get_custom_fan_mode();
    auto payload = StringToFanLevel(fan_mode);
    if (payload.has_value()) {
      ESP_LOGD(TAG, "Setting fan mode to %s", fan_mode);
      this->set_custom_fan_mode_(fan_mode);
      this->sendCmd(ToshibaCommandType::FAN, static_cast<uint8_t>(payload.value()));
    }
  }
  
  if (call.get_custom_preset().has_value()) {
    auto special_mode = *call.get_custom_preset();
    auto payload = StringToSpecialMode(special_mode);
    if (payload.has_value()) {
      ESP_LOGD(TAG, "Setting special mode to %s", special_mode);
      this->set_custom_preset_(special_mode);
      this->sendCmd(ToshibaCommandType::SPECIAL, static_cast<uint8_t>(payload.value()));
    }
  }

  if (call.get_swing_mode().has_value()) {
    auto swing_mode = *call.get_swing_mode();
    auto function_value = ClimateSwingModeToInt(swing_mode);
    ESP_LOGD(TAG, "Setting swing mode to %s", climate_swing_mode_to_string(swing_mode));
    this->swing_mode = swing_mode;
    this->sendCmd(ToshibaCommandType::SWING, static_cast<uint8_t>(function_value));
  }

  this->publish_state();
}

ClimateTraits ToshibaClimateUart::traits() {
  auto traits = climate::ClimateTraits();
  traits.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_AUTO, climate::CLIMATE_MODE_COOL,
                              climate::CLIMATE_MODE_HEAT, climate::CLIMATE_MODE_DRY, climate::CLIMATE_MODE_FAN_ONLY});
  traits.set_supported_swing_modes({climate::CLIMATE_SWING_OFF, climate::CLIMATE_SWING_VERTICAL});
  traits.set_supports_current_temperature(true);

  traits.add_supported_fan_mode(CLIMATE_FAN_AUTO);
  // Toshiba AC has more FAN levels that standard climate component, we have to use custom.
  traits.add_supported_custom_fan_mode(CUSTOM_FAN_LEVEL_1);
  traits.add_supported_custom_fan_mode(CUSTOM_FAN_LEVEL_2);
  traits.add_supported_custom_fan_mode(CUSTOM_FAN_LEVEL_3);
  traits.add_supported_custom_fan_mode(CUSTOM_FAN_LEVEL_4);
  traits.add_supported_custom_fan_mode(CUSTOM_FAN_LEVEL_5);
  traits.add_supported_custom_fan_mode(CUSTOM_FAN_LEVEL_QUIET);

  traits.add_supported_custom_preset(CUSTOM_SPECIAL_MODE_OFF);
  traits.add_supported_custom_preset(CUSTOM_SPECIAL_MODE_HI_POWER);
  traits.add_supported_custom_preset(CUSTOM_SPECIAL_MODE_ECO);
  traits.add_supported_custom_preset(CUSTOM_SPECIAL_MODE_SILENT);
  
  traits.set_visual_temperature_step(1);
  traits.set_visual_min_temperature(MIN_TEMP);
  traits.set_visual_max_temperature(MAX_TEMP);
  return traits;
}

void ToshibaClimateUart::on_set_pwr_level(const std::string &value) {
  ESP_LOGD(TAG, "Setting power level to %s", value.c_str());
  auto pwr_level = StringToPwrLevel(value);
  this->sendCmd(ToshibaCommandType::POWER_SEL, static_cast<uint8_t>(pwr_level.value()));
  pwr_select_->publish_state(value);
}

void ToshibaPwrModeSelect::control(const std::string &value) { parent_->on_set_pwr_level(value); }

}  // namespace toshiba_suzumi
}  // namespace esphome
