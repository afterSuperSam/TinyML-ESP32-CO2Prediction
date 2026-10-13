#pragma once

#include <stdint.h>

// 集中管理接线、回放、控制预览与界面参数。
namespace Configuration {
constexpr char version[] = "1.12.0";
constexpr bool predictionDefaultEnabled = true;
constexpr char predictionSettingsNamespace[] = "air-predict";
constexpr char predictionEnabledKey[] = "enabled";
constexpr int sensorData = 8;
constexpr int sensorClock = 9;
constexpr uint8_t sensorAddress = 0x62;
constexpr uint32_t sensorBusFrequency = 100000;
constexpr uint32_t sensorPollInterval = 1000;
constexpr uint32_t sensorStaleInterval = 15000;
constexpr uint32_t sensorRecoveryInterval = 30000;
constexpr uint32_t sensorStopDelay = 500;
constexpr uint32_t recordingHeartbeatInterval = 60000;
constexpr uint32_t measurementMinuteDuration = 60000;
constexpr unsigned minimumMinuteSamples = 8;
constexpr int displayClock = 12;
constexpr int displayData = 11;
constexpr int displayCommand = 7;
constexpr int displayReset = 6;
constexpr int displaySelect = 10;
constexpr int touchClock = 16;
constexpr int touchDataOut = 15;
constexpr int touchDataIn = 13;
constexpr int touchSelect = 5;
constexpr int touchInterrupt = 4;
constexpr uint32_t displayFrequency = 8000000;
constexpr uint32_t serialBaud = 115200;
constexpr uint32_t startupDelay = 1000;
constexpr uint32_t touchInterval = 30;
constexpr uint32_t touchHalfPeriod = 2;
constexpr uint16_t calibrationMargin = 24;
constexpr uint8_t calibrationSamples = 7;
constexpr uint16_t calibrationSpread = 180;
constexpr uint32_t releaseDuration = 200;
constexpr float minimumCalibrationArea = 100000.0f;
constexpr float maximumCalibrationError = 25.0f;
constexpr uint16_t markerRadius = 8;
constexpr uint16_t width = 320;
constexpr uint16_t height = 240;
constexpr uint8_t displayOrientation = 0x28;
constexpr uint32_t interfaceInterval = 100;
constexpr uint32_t replayMinuteDuration = 5000;
constexpr uint32_t loopInterval = 5;
constexpr unsigned historyCapacity = 61;
constexpr unsigned predictionNearMinutes = 5;
constexpr unsigned predictionFarMinutes = 15;
constexpr unsigned predictionHistoryMinutes = 20;
constexpr float fanSimulationOutdoorPpm = 420.0f;
constexpr float fanSimulationRemovalNear = 0.18f;
constexpr float fanSimulationRemovalFar = 0.35f;
constexpr float minimumChartConcentration = 400.0f;
constexpr float minimumChartMaximum = 1200.0f;
constexpr float chartStep = 200.0f;
constexpr unsigned comfortableConcentration = 800;
constexpr unsigned elevatedConcentration = 1000;
constexpr unsigned highConcentration = 1500;
constexpr uint8_t fanLevels[] = {0, 33, 66, 100};
constexpr uint32_t statisticsInterval = 2000;
constexpr char recordingDirectory[] = "/recordings";
constexpr uint32_t recordingReserveBytes = 16384;
constexpr unsigned recordingLineCapacity = 768;
constexpr unsigned environmentCount = 4;
constexpr const char* environmentNames[] = {"classroom", "bedroom", "outdoor", "enclosed"};
constexpr const char* environmentLabels[] = {"教室", "卧室", "户外", "密闭"};
static_assert(sizeof(environmentNames) / sizeof(environmentNames[0]) == environmentCount &&
    sizeof(environmentLabels) / sizeof(environmentLabels[0]) == environmentCount, "Invalid environment configuration");
// format4 在保留旧 measurement 行的前提下增加 recording_forecast 快照行。
constexpr unsigned recordingFormatVersion = 4;
constexpr unsigned refreshTileSize = 16;
constexpr uint32_t calibrationVersion = 2;
constexpr uint32_t calibrationMagic = 0x5343414C;
constexpr float calibrationCoefficientLimit = 2.0f;
constexpr float calibrationOffsetLimit = 10000.0f;
constexpr uint16_t background = 0xF79D; // #F7F3E8
constexpr uint16_t surface = 0xFFDD; // #FCF8EF
constexpr uint16_t ink = 0x21E9; // #263C48
constexpr uint16_t muted = 0x6C32; // #6F8790
constexpr uint16_t sea = 0x53F2; // #527D93
constexpr uint16_t peach = 0xE551; // #E7AA8D
constexpr uint16_t terracotta = 0xA2E8; // #A65F47
constexpr uint16_t divider = 0xDF1A; // #DCE0D3
constexpr uint16_t seaWash = 0xFFDD; // #FCF8EF
constexpr uint16_t peachWash = 0xF73B; // #F5E7DB
constexpr uint16_t healthy = 0x4BF0; // #4C7E81
constexpr uint16_t predictionReadyColor = 0x4BE8;
constexpr uint16_t predictionWaitingColor = 0xE5C8;
constexpr uint16_t predictionFaultColor = 0xC226;
constexpr uint16_t predictionDisabledColor = 0x8C51;
constexpr int pins[] = {displayClock, displayData, displayCommand, displayReset,
    displaySelect, touchClock, touchDataOut, touchDataIn, touchSelect, touchInterrupt, sensorData, sensorClock};
constexpr bool validPins() {
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if (pins[i] < 0 || pins[i] > 48) return false;
        for (unsigned j = i + 1; j < sizeof(pins) / sizeof(pins[0]); ++j) {
            if (pins[i] == pins[j]) return false;
        }
    }
    return true;
}
static_assert(validPins(), "Invalid or duplicate GPIO configuration");
static_assert(sizeof(predictionSettingsNamespace) > 1 && sizeof(predictionSettingsNamespace) <= 16 &&
    sizeof(predictionEnabledKey) > 1 && sizeof(predictionEnabledKey) <= 16, "Invalid prediction settings keys");
static_assert(predictionReadyColor != predictionWaitingColor && predictionReadyColor != predictionFaultColor &&
    predictionReadyColor != predictionDisabledColor && predictionWaitingColor != predictionFaultColor &&
    predictionWaitingColor != predictionDisabledColor && predictionFaultColor != predictionDisabledColor,
    "Prediction states require distinct colors");
static_assert(sensorAddress == 0x62 && sensorBusFrequency <= 100000 && sensorBusFrequency > 0 &&
    sensorPollInterval >= 1000 && sensorStaleInterval > 5000 && measurementMinuteDuration == 60000 &&
    minimumMinuteSamples > 0 && minimumMinuteSamples <= 12, "Invalid sensor configuration");
static_assert(sensorRecoveryInterval > sensorStaleInterval && sensorStopDelay >= 500 &&
    recordingHeartbeatInterval >= 10000, "Invalid recovery configuration");
static_assert(displayFrequency > 0 && displayFrequency <= 10000000, "Invalid SPI frequency");
static_assert(touchHalfPeriod > 0 && touchInterval > 0 && interfaceInterval > 0 && startupDelay > 0, "Invalid timing");
static_assert(calibrationMargin > markerRadius && calibrationMargin < width / 2 && calibrationMargin < height / 2, "Invalid calibration margin");
static_assert(calibrationSamples >= 3 && calibrationSamples <= 15 && calibrationSamples % 2 == 1, "Calibration sample count must be odd and between 3 and 15");
static_assert(calibrationSpread > 0 && calibrationSpread < 4095 && releaseDuration > 0 && minimumCalibrationArea > 0 && maximumCalibrationError > 0, "Invalid calibration configuration");
static_assert(width >= 320 && height >= 240 && unsigned(width) * height <= 160000,
    "Invalid landscape framebuffer dimensions");
static_assert(historyCapacity >= 61 && replayMinuteDuration > 0 && loopInterval > 0 && refreshTileSize > 0,
    "Invalid replay or rendering configuration");
static_assert(comfortableConcentration < elevatedConcentration && elevatedConcentration < highConcentration,
    "Invalid concentration thresholds");
static_assert(predictionNearMinutes < predictionFarMinutes && minimumChartMaximum > minimumChartConcentration &&
    chartStep > 0 && fanSimulationOutdoorPpm >= 0 && fanSimulationRemovalNear >= 0 && fanSimulationRemovalNear <= 1 &&
    fanSimulationRemovalFar >= fanSimulationRemovalNear && fanSimulationRemovalFar <= 1, "Invalid prediction or chart configuration");
static_assert(predictionHistoryMinutes > 0 && predictionHistoryMinutes < historyCapacity &&
    statisticsInterval > 0, "Invalid statistics window");
static_assert(fanLevels[0] == 0 && fanLevels[0] < fanLevels[1] && fanLevels[1] < fanLevels[2] &&
    fanLevels[2] < fanLevels[3] && fanLevels[3] <= 100, "Invalid fan preview levels");
static_assert(recordingDirectory[0] == '/' && sizeof(recordingDirectory) < 32 &&
    recordingReserveBytes >= 4096 && recordingLineCapacity >= 384 && recordingLineCapacity <= 1024 &&
    recordingFormatVersion >= 1 && recordingFormatVersion <= 4, "Invalid recording configuration");
}

