#pragma once

#include <cstdint>
#include "esphome/core/log.h"
#include "esphome/components/climate/climate.h"

namespace esphome {
namespace toshiba_suzumi {

static const std::string CUSTOM_FAN_LEVEL_QUIET = "Quiet";
static const std::string CUSTOM_FAN_LEVEL_1 = "Level 1";
static const std::string CUSTOM_FAN_LEVEL_2 = "Level 2";
static const std::string CUSTOM_FAN_LEVEL_3 = "Level 3";
static const std::string CUSTOM_FAN_LEVEL_4 = "Level 4";
static const std::string CUSTOM_FAN_LEVEL_5 = "Level 5";

static const std::string &CUSTOM_PWR_LEVEL_50 = "50 %";
static const std::string &CUSTOM_PWR_LEVEL_75 = "75 %";
static const std::string &CUSTOM_PWR_LEVEL_100 = "100 %";

static const std::string &CUSTOM_SPECIAL_MODE_OFF = "off";
static const std::string &CUSTOM_SPECIAL_MODE_HI_POWER = "hi-power";
static const std::string &CUSTOM_SPECIAL_MODE_ECO = "eco";
static const std::string &CUSTOM_SPECIAL_MODE_SILENT = "silent";
  
enum class CustomFanModes { QUIET, LEVEL_1, LEVEL_2, LEVEL_3, LEVEL_4, LEVEL_5, AUTO };
enum class CustomSpecialModes { OFF, HI_POWER, ECO, SILENT };

// codes as reverse engineered from Toshiba AC communication with original Wifi module.
enum class MODE { AUTO = 65, COOL = 66, HEAT = 67, DRY = 68, FAN_ONLY = 69 };
enum class FAN {
  QUIET = 49,
  FANMODE_1 = 50,
  FANMODE_2 = 51,
  FANMODE_3 = 52,
  FANMODE_4 = 53,
  FANMODE_5 = 54,
  AUTO = 65
};
enum class SWING { OFF = 49, ON = 65 };
enum class STATE { ON = 48, OFF = 49 };
enum class PWR_LEVEL { PCT_50 = 50, PCT_75 = 75, PCT_100 = 100 };
enum class SPECIAL { OFF = 0, HI_POWER = 1, ECO = 3, SILENT = 2 };

enum class ToshibaCommandType : uint8_t {
  HANDSHAKE = 0,  // dummy command to handle all handshake requests
  DELAY = 1, // dummy command to issue a delay in communication
  POWER_STATE = 128,
  POWER_SEL = 135,
  COMFORT_SLEEP = 148, // { ON = 65, OFF = 66 }
  FAN = 160,
  SWING = 163,
  MODE = 176,
  TARGET_TEMP = 179,
  ROOM_TEMP = 187,
  OUTDOOR_TEMP = 190,
  SPECIAL = 247,
};

const MODE ClimateModeToInt(climate::ClimateMode mode);
const climate::ClimateMode IntToClimateMode(MODE mode);

const SWING ClimateSwingModeToInt(climate::ClimateSwingMode mode);
const climate::ClimateSwingMode IntToClimateSwingMode(SWING mode);

const LogString *climate_state_to_string(STATE mode);

const optional<FAN> StringToFanLevel(std::string mode);
const ::std::string IntToCustomFanMode(FAN mode);

const optional<PWR_LEVEL> StringToPwrLevel(const std::string &mode);
const std::string IntToPowerLevel(PWR_LEVEL mode);
  
const optional<SPECIAL> StringToSpecialMode(const std::string &mode);
const std::string IntToSpecialMode(SPECIAL mode);

}  // namespace toshiba_suzumi
}  // namespace esphome
