#pragma once
#include <Arduino.h>
#include <esp_timer.h>
#include "display.h"
#include "touch.h"
#include "interface.h"
#include "sensor.h"
#include "clock.h"
#include "predictor.h"

// 唯一集成模块：协调回放、模型、记录、触摸与显示，驱动各自留在独立文件。
namespace Application {
inline char commandLine[64];
inline unsigned commandLength = 0;
inline bool collectingClock = false;
inline uint32_t lastForecastMinute = UINT32_MAX;
inline uint32_t lastFrame = 0, statisticsStarted = 0, renderedFrames = 0, totalFrames = 0;
inline Preferences predictionSettings;
inline bool predictionSettingsReady = false;
inline void predictionSettingsFailed(const char* code, const char* message) {
    Prediction::settingSaved = false;
    Prediction::settingError = message;
    Serial.printf("{\"event\":\"prediction_settings_error\",\"code\":\"%s\"}\n", code);
    Recording::event("prediction_settings_error", code);
}
inline void loadPredictionSettings() {
    Prediction::settingSaved = true;
    Prediction::settingError = "";
    Prediction::setEnabled(Configuration::predictionDefaultEnabled);
    predictionSettingsReady = predictionSettings.begin(Configuration::predictionSettingsNamespace, false);
    if (!predictionSettingsReady) { predictionSettingsFailed("preferences_open_failed", "开关存储失败"); return; }
    if (!predictionSettings.isKey(Configuration::predictionEnabledKey)) return;
    if (predictionSettings.getType(Configuration::predictionEnabledKey) != PT_U8) {
        predictionSettingsFailed("preferences_type_invalid", "开关数据损坏"); return;
    }
    const uint8_t stored = predictionSettings.getUChar(Configuration::predictionEnabledKey, 2);
    if (stored > 1) { predictionSettingsFailed("preferences_read_failed", "开关读取失败"); return; }
    Prediction::setEnabled(stored == 1);
}
inline Prediction::Result runModel(const float* values, bool continuous) {
    const auto prediction = Predictor::predict(values, continuous);
    Prediction::Status status = Prediction::Status::NotReady;
    switch (prediction.status) {
        case Predictor::Status::Success: status = Prediction::Status::Success; break;
        case Predictor::Status::NotReady: status = Prediction::Status::NotReady; break;
        case Predictor::Status::InvalidHistory: status = Prediction::Status::InvalidHistory; break;
        case Predictor::Status::OutsideCalibration: status = Prediction::Status::OutsideCalibration; break;
        case Predictor::Status::InferenceFailed: status = Prediction::Status::InferenceFailed; break;
    }
    Prediction::Result result;
    result.status = status;
    result.nearConcentration = prediction.nearConcentration;
    result.farConcentration = prediction.farConcentration;
    result.durationMicroseconds = prediction.durationMicroseconds;
    Prediction::applyFanSimulation(result);
    return result;
}
inline void recordObservation() {
    char timestamp[32];
    const bool clockReady = Clock::isoNow(timestamp, sizeof(timestamp));
    Recording::append({Data::latest(), uint64_t(esp_timer_get_time() / 1000),
        clockReady ? timestamp : nullptr, clockReady ? timestamp : nullptr, Sensor::sampleError()}, Prediction::result);
}
inline void recordForecastSnapshot() {
    if (!Recording::active() || Data::count == 0) return;
    const auto& sample = Data::history[Configuration::historyCapacity - 1];
    if (sample.minute == lastForecastMinute) return;
    char timestamp[32];
    const bool clockReady = Clock::isoNow(timestamp, sizeof(timestamp));
    const auto& prediction = Prediction::result;
    char detail[112];
    snprintf(detail, sizeof(detail), "fan_simulation=%.1fppm;removal=%.3f/%.3f",
        Configuration::fanSimulationOutdoorPpm, Configuration::fanSimulationRemovalNear,
        Configuration::fanSimulationRemovalFar);
    if (Recording::appendForecast({uint64_t(esp_timer_get_time() / 1000), sample.minute,
        clockReady ? timestamp : nullptr, sample.minute + Configuration::predictionNearMinutes,
        sample.minute + Configuration::predictionFarMinutes, prediction.nearConcentration,
        prediction.farConcentration, prediction.nearConcentration, prediction.farConcentration,
        prediction.fanNearConcentration, prediction.fanFarConcentration, unsigned(prediction.status),
        ModelAssets::modelSha256, detail})) lastForecastMinute = sample.minute;
}
inline bool startRecording() {
    if (!Recording::start({Recording::Source::ClassroomMeasurement, "", ModelAssets::modelSha256,
        uint64_t(esp_timer_get_time() / 1000), Clock::valid ? Clock::source : "none", Clock::valid ? Clock::offsetMinutes : 0,
        Clock::valid ? Clock::anchorEpochMilliseconds : 0, Clock::valid ? Clock::anchorUptimeMilliseconds : 0,
        Recording::environmentName()})) return false;
    lastForecastMinute = UINT32_MAX;
    Recording::event("sensor_snapshot", Recording::errorName(Sensor::sampleError()), Sensor::sampleError());
    return true;
}
inline void restartReplay() {
    Sensor::requestRecovery(millis());
}
inline void logPrediction() {
    const auto& value = Prediction::result;
    char nearValue[24], farValue[24], fanNearValue[24], fanFarValue[24];
    if (isfinite(value.nearConcentration)) snprintf(nearValue, sizeof(nearValue), "%.5f", value.nearConcentration);
    else strcpy(nearValue, "null");
    if (isfinite(value.farConcentration)) snprintf(farValue, sizeof(farValue), "%.5f", value.farConcentration);
    else strcpy(farValue, "null");
    if (isfinite(value.fanNearConcentration)) snprintf(fanNearValue, sizeof(fanNearValue), "%.3f", value.fanNearConcentration);
    else strcpy(fanNearValue, "null");
    if (isfinite(value.fanFarConcentration)) snprintf(fanFarValue, sizeof(fanFarValue), "%.3f", value.fanFarConcentration);
    else strcpy(fanFarValue, "null");
    Serial.printf("{\"event\":\"ui_prediction\",\"source\":\"classroom_measurement\",\"index\":%u,\"status\":%u,\"prediction_5_ppm\":%s,\"prediction_15_ppm\":%s,\"fan_prediction_5_ppm\":%s,\"fan_prediction_15_ppm\":%s,\"duration_us\":%lu,\"free_heap_bytes\":%u}\n",
        Data::count, unsigned(value.status), nearValue, farValue,
        fanNearValue, fanFarValue,
        static_cast<unsigned long>(value.durationMicroseconds), unsigned(ESP.getFreeHeap()));
}
inline void togglePrediction() {
    Prediction::setEnabled(!Prediction::enabled);
    Prediction::settingSaved = true;
    Prediction::settingError = "";
    if (!predictionSettingsReady)
        predictionSettingsReady = predictionSettings.begin(Configuration::predictionSettingsNamespace, false);
    if (!predictionSettingsReady || predictionSettings.putBool(Configuration::predictionEnabledKey, Prediction::enabled) != 1 ||
        predictionSettings.getUChar(Configuration::predictionEnabledKey, 2) != unsigned(Prediction::enabled))
        predictionSettingsFailed("preferences_write_failed", "开关保存失败");
    Prediction::update();
    lastForecastMinute = UINT32_MAX;
    Recording::event(Prediction::enabled ? "prediction_enabled" : "prediction_disabled",
        Prediction::settingSaved ? "saved" : "not_saved");
    Serial.printf("{\"event\":\"prediction_setting\",\"enabled\":%s,\"saved\":%s}\n",
        Prediction::enabled ? "true" : "false", Prediction::settingSaved ? "true" : "false");
    recordForecastSnapshot();
    logPrediction();
}
inline void reportStatus() {
    const auto history = Data::historyDiagnostics();
    const uint32_t now = millis();
    Serial.printf("{\"event\":\"ui_status\",\"version\":\"%s\",\"prediction_enabled\":%s,\"prediction_setting_saved\":%s,\"prediction_setting_error\":\"%s\",\"model_sha256\":\"%s\",\"sample_interval_ms\":%lu,\"clock_valid\":%s,\"calibration_saved\":%s,\"calibrating\":%s,\"frames\":%lu,\"history_minutes\":%u,\"continuous_valid_minutes\":%u,\"has_bad_minute\":%s,\"first_bad_minute\":%lu,\"first_bad_offset\":%u,\"first_bad_reason\":\"%s\",\"first_bad_samples\":%u,\"first_bad_errors\":%u,\"current_minute\":%lu,\"current_minute_samples\":%u,\"current_minute_errors\":%u,\"current_minute_reason\":\"%s\",\"sensor_status\":\"%s\",\"sensor_errors\":%lu,\"recovery_attempts\":%lu,\"has_valid_reading\":%s,\"measurement_age_ms\":%lu,\"has_sensor_error\":%s,\"last_sensor_error\":\"%s\",\"last_error_age_ms\":%lu,\"has_recovery\":%s,\"last_recovery_age_ms\":%lu}\n",
        Configuration::version, Prediction::enabled ? "true" : "false", Prediction::settingSaved ? "true" : "false",
        Prediction::settingError, ModelAssets::modelSha256, static_cast<unsigned long>(Configuration::measurementMinuteDuration),
        Clock::valid ? "true" : "false", Touch::saved ? "true" : "false", Touch::active ? "true" : "false", static_cast<unsigned long>(totalFrames),
        Data::count, history.continuousValidMinutes, history.hasBadMinute ? "true" : "false", static_cast<unsigned long>(history.firstBadMinute),
        history.firstBadOffsetMinutes, Data::minuteFailureName(history.firstBadReason), history.firstBadSamples, history.firstBadErrors,
        static_cast<unsigned long>(Sensor::bucketMinute), Sensor::samples, Sensor::bucketErrors, Data::minuteFailureName(Sensor::bucketFailureReason),
        Recording::errorName(Sensor::sampleError()), static_cast<unsigned long>(Sensor::errorCount), static_cast<unsigned long>(Sensor::recoveryAttempts),
        Sensor::hasValidReading ? "true" : "false", static_cast<unsigned long>(Sensor::hasValidReading ? now - Sensor::lastValid : 0),
        Sensor::hasError ? "true" : "false", Sensor::hasError ? Recording::errorName(Sensor::sampleError(Sensor::lastErrorStatus)) : "none",
        static_cast<unsigned long>(Sensor::hasError ? now - Sensor::lastError : 0), Sensor::hasRecovery ? "true" : "false",
        static_cast<unsigned long>(Sensor::hasRecovery ? now - Sensor::lastRecovery : 0));
    logPrediction();
}
inline void refresh() {
    lastFrame = millis();
    Recording::measurementAgeSeconds = (millis() - Sensor::lastValid) / 1000;
    Interface::calibrationSaved = Touch::saved;
    Interface::freeMemory = ESP.getFreeHeap();
    const uint32_t now = millis();
    const auto history = Data::historyDiagnostics();
    auto& diagnostic = Interface::diagnostics;
    diagnostic.currentMinute = Sensor::bucketMinute;
    diagnostic.currentMinuteSamples = Sensor::samples;
    diagnostic.currentMinuteErrors = Sensor::bucketErrors;
    diagnostic.sensorErrors = Sensor::errorCount;
    diagnostic.recoveryAttempts = Sensor::recoveryAttempts;
    diagnostic.hasValidReading = Sensor::hasValidReading;
    diagnostic.measurementAgeSeconds = Sensor::hasValidReading ? (now - Sensor::lastValid) / 1000 : 0;
    diagnostic.hasSensorError = Sensor::hasError;
    diagnostic.lastSensorError = Sensor::hasError ? Sensor::statusLabel(Sensor::lastErrorStatus) : "无";
    diagnostic.lastErrorAgeSeconds = Sensor::hasError ? (now - Sensor::lastError) / 1000 : 0;
    diagnostic.hasRecovery = Sensor::hasRecovery;
    diagnostic.lastRecoveryAgeSeconds = Sensor::hasRecovery ? (now - Sensor::lastRecovery) / 1000 : 0;
    diagnostic.sensorStatus = Sensor::statusLabel();
    diagnostic.continuousValidMinutes = history.continuousValidMinutes;
    diagnostic.hasBadMinute = history.hasBadMinute;
    diagnostic.firstBadMinute = history.firstBadMinute;
    diagnostic.firstBadOffset = history.firstBadOffsetMinutes;
    diagnostic.firstBadSamples = history.firstBadSamples;
    diagnostic.firstBadErrors = history.firstBadErrors;
    diagnostic.firstBadReason = Data::minuteFailureLabel(history.firstBadReason);
    diagnostic.predictionSettingSaved = Prediction::settingSaved;
    diagnostic.predictionSettingError = Prediction::settingError;
    diagnostic.tensorArenaUsedBytes = Predictor::interpreter ? Predictor::interpreter->arena_used_bytes() : 0;
    diagnostic.tensorArenaCapacityBytes = ModelAssets::tensorArenaBytes;
    diagnostic.modelHash = ModelAssets::modelSha256;
    diagnostic.uptimeMilliseconds = uint64_t(esp_timer_get_time() / 1000);
    diagnostic.storageTotalBytes = Recording::totalBytes;
    diagnostic.storageRemainingBytes = Recording::remainingBytes;
    diagnostic.recordingSessions = Recording::sessionCount;
    diagnostic.recordingStatus = Recording::statusLabel();
    diagnostic.storageMounted = Recording::mounted;
    Interface::render();
    Canvas::present();
    ++renderedFrames;
    ++totalFrames;
    const uint32_t elapsed = millis() - statisticsStarted;
    if (elapsed >= Configuration::statisticsInterval) {
        Interface::framesPerSecond = renderedFrames * 1000.0f / elapsed;
        statisticsStarted = millis();
        renderedFrames = 0;
    }
}
inline void begin() {
    Serial.begin(Configuration::serialBaud);
    delay(Configuration::startupDelay);
    Display::begin();
    Recording::begin();
    loadPredictionSettings();
    Clock::valid = false;
    Sensor::begin();
    const bool ready = Predictor::begin();
    Prediction::begin(ready ? runModel : nullptr);
    Prediction::update();
    Touch::begin();
    if (Recording::resumeRequested && startRecording()) Recording::event("resume_after_boot");
    statisticsStarted = millis();
    Serial.printf("{\"event\":\"ui_ready\",\"version\":\"%s\",\"model_ready\":%s,\"model_sha256\":\"%s\",\"arena_used_bytes\":%u,\"touch_mapping\":\"%s\"}\n",
        Configuration::version, ready ? "true" : "false", ModelAssets::modelSha256,
        ready ? unsigned(Predictor::interpreter->arena_used_bytes()) : 0, Touch::saved ? "saved" : "default");
    logPrediction();
    Recording::report();
    if (!Touch::active) refresh();
}
inline void tick() {
    const bool wasCalibrating = Touch::active;
    bool redraw = false;
    int x = 0, y = 0;
    while (Serial.available()) {
        const char command = char(Serial.read());
        if (collectingClock) {
            if (command == '\n' || command == '\r') {
                commandLine[commandLength] = 0;
                const bool accepted = Clock::setFromIso(commandLine + 1);
                Serial.printf("{\"event\":\"clock_command\",\"accepted\":%s}\n", accepted ? "true" : "false");
                if (accepted) Recording::event("clock_set", commandLine + 1);
                commandLength = 0; collectingClock = false;
            } else if (commandLength + 1 < sizeof(commandLine)) commandLine[commandLength++] = command;
            continue;
        }
        if (command == 'T' || command == 't') { collectingClock = true; commandLength = 1; commandLine[0] = command; continue; }
        bool redrawCommand = false;
        if (command == 'r') Touch::restart();
        else if (command == 'q') restartReplay();
        else if (command == 'b') { if (!Recording::active()) startRecording(); Recording::report(); redrawCommand = true; }
        else if (command == 'x') { Recording::stop(); Recording::report(); redrawCommand = true; }
        else if (command == 'l') Recording::report();
        else if (command == 'i') { Recording::initialize(); Recording::report(); redrawCommand = true; }
        else if (command == 'e') { Recording::exportAll(); redrawCommand = true; }
        else if (command == 'p') { togglePrediction(); redrawCommand = true; }
        else if (command == 's') {
            reportStatus();
        }
        redraw = redraw || redrawCommand;
    }
    if (Touch::poll(x, y, millis())) {
        const auto action = Interface::tap(x, y, millis());
        if (action == Interface::Action::Calibrate) Touch::restart();
        else if (action == Interface::Action::RestartReplay) restartReplay();
        else if (action == Interface::Action::StartRecording) startRecording();
        else if (action == Interface::Action::StopRecording) Recording::stop();
        else if (action == Interface::Action::InitializeRecording) Recording::initialize();
        else if (action == Interface::Action::TogglePrediction) togglePrediction();
        redraw = true;
    }
    const bool sensorChanged = Sensor::tick(millis());
    if (sensorChanged) {
        Prediction::update();
        if (Sensor::measurementReady) recordObservation();
        if (Data::live && Data::count > 0) recordForecastSnapshot();
        if (Interface::debugLogs) logPrediction();
        redraw = true;
    }
    if (!Touch::active && (wasCalibrating || redraw || millis() - lastFrame >= Configuration::interfaceInterval)) refresh();
    static uint32_t lastHeartbeat = 0;
    if (millis() - lastHeartbeat >= Configuration::recordingHeartbeatInterval) {
        lastHeartbeat = millis();
        char detail[192];
        snprintf(detail, sizeof(detail), "sensor=%s;age_ms=%lu;errors=%lu;recoveries=%lu;free_bytes=%lu;heap=%lu",
            Recording::errorName(Sensor::sampleError()), static_cast<unsigned long>(millis() - Sensor::lastValid),
            static_cast<unsigned long>(Sensor::errorCount), static_cast<unsigned long>(Sensor::recoveryAttempts),
            static_cast<unsigned long>(Recording::remainingBytes), static_cast<unsigned long>(ESP.getFreeHeap()));
        Recording::event("heartbeat", detail, Sensor::sampleError());
    }
    delay(Configuration::loopInterval);
}
static_assert(Configuration::predictionHistoryMinutes == ModelAssets::windowMinutes, "Model history mismatch");
static_assert(ModelAssets::featureCount == 1, "Runtime minute history supports only one CO2 feature");
static_assert(Configuration::predictionNearMinutes == 5 && Configuration::predictionFarMinutes == 15,
    "Interface and recording protocol require 5/15 minute horizons");
static_assert(Configuration::predictionNearMinutes == ModelAssets::nearMinutes &&
    Configuration::predictionFarMinutes == ModelAssets::farMinutes, "Model horizons mismatch");
}
