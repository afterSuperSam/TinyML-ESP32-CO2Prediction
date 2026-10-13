#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "data.h"
#include "recording.h"

// SCD41 周期采集、CRC 校验和分钟汇总；不修改校准参数或启动风扇。
namespace Sensor {
enum class Status { Waiting, Ready, BusFailed, ReadFailed, CrcFailed, InvalidValue, Stale };
inline Status status = Status::Waiting;
inline bool started = false;
inline bool measurementReady = false, recovering = false, recoveryPending = false;
inline uint32_t recoveryStarted = 0, lastRecovery = 0, recoveryAttempts = 0, errorCount = 0;
inline Recording::SampleError sampleError();
inline uint32_t lastPoll = 0, lastValid = 0, bucketStarted = 0, bucketMinute = 0;
inline unsigned samples = 0;
inline unsigned bucketErrors = 0;
inline Data::MinuteFailure bucketFailureReason = Data::MinuteFailure::None;
inline uint32_t lastError = 0;
inline Status lastErrorStatus = Status::Waiting;
inline bool hasValidReading = false, hasError = false, hasRecovery = false;
inline float concentrationSum = 0, temperatureSum = 0, humiditySum = 0;
inline bool bucketFailed = false;
inline const char* statusLabel(Status value = status) {
    switch (value) {
        case Status::Ready: return "正常";
        case Status::BusFailed: return "连接失败";
        case Status::ReadFailed: return "读取失败";
        case Status::CrcFailed: return "校验失败";
        case Status::InvalidValue: return "无效数据";
        case Status::Stale: return "数据过期";
        default: return "等待数据";
    }
}
inline Recording::SampleError sampleError(Status value) {
    if (value == Status::Ready) return Recording::SampleError::None;
    if (value == Status::InvalidValue) return Recording::SampleError::InvalidValue;
    if (value == Status::ReadFailed) return Recording::SampleError::ReadFailed;
    if (value == Status::CrcFailed) return Recording::SampleError::CrcFailed;
    if (value == Status::BusFailed) return Recording::SampleError::BusFailed;
    if (value == Status::Stale) return Recording::SampleError::Stale;
    return Recording::SampleError::Unavailable;
}
inline Recording::SampleError sampleError() { return sampleError(status); }
inline void fail(Status failure) {
    status = failure;
    Data::sensorLabel = statusLabel();
    Data::current = {NAN, NAN, NAN, bucketMinute, false};
    bucketFailed = true;
    ++bucketErrors;
    if (bucketFailureReason == Data::MinuteFailure::None) {
        switch (failure) {
            case Status::ReadFailed: bucketFailureReason = Data::MinuteFailure::ReadFailed; break;
            case Status::CrcFailed: bucketFailureReason = Data::MinuteFailure::CrcFailed; break;
            case Status::InvalidValue: bucketFailureReason = Data::MinuteFailure::InvalidValue; break;
            case Status::BusFailed: bucketFailureReason = Data::MinuteFailure::BusFailed; break;
            case Status::Stale: bucketFailureReason = Data::MinuteFailure::Stale; break;
            default: bucketFailureReason = Data::MinuteFailure::MissingMinute; break;
        }
    }
    hasError = true;
    lastError = millis();
    lastErrorStatus = failure;
    ++errorCount;
    Recording::event("sensor_fault", Recording::errorName(sampleError()), sampleError());
    if (Serial) Serial.printf("{\"event\":\"sensor_error\",\"status\":%u}\n", unsigned(status));
}
inline bool command(uint16_t value) {
    Wire.beginTransmission(Configuration::sensorAddress);
    Wire.write(uint8_t(value >> 8));
    Wire.write(uint8_t(value));
    return Wire.endTransmission() == 0;
}
inline uint8_t checksum(const uint8_t* bytes) {
    uint8_t crc = 0xff;
    for (unsigned index = 0; index < 2; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? uint8_t((crc << 1) ^ 0x31) : uint8_t(crc << 1);
    }
    return crc;
}
inline bool readWords(uint16_t instruction, uint16_t* words, unsigned count) {
    if (!command(instruction)) { fail(Status::ReadFailed); return false; }
    delay(1);
    const unsigned length = count * 3;
    uint8_t bytes[9];
    if (Wire.requestFrom(Configuration::sensorAddress, uint8_t(length)) != length) {
        while (Wire.available()) Wire.read();
        fail(Status::ReadFailed); return false;
    }
    for (unsigned index = 0; index < length; ++index) bytes[index] = Wire.read();
    for (unsigned index = 0; index < count; ++index) {
        const uint8_t* word = bytes + index * 3;
        if (checksum(word) != word[2]) { fail(Status::CrcFailed); return false; }
        words[index] = uint16_t(word[0]) << 8 | word[1];
    }
    return true;
}
inline void resetBucket() {
    samples = 0;
    bucketErrors = 0;
    bucketFailureReason = Data::MinuteFailure::None;
    concentrationSum = temperatureSum = humiditySum = 0;
    bucketFailed = false;
}
inline void begin() {
    Data::live = true;
    Data::clear();
    resetBucket();
    status = Status::Waiting;
    hasValidReading = hasError = hasRecovery = false;
    errorCount = recoveryAttempts = lastError = lastRecovery = 0;
    Data::sensorLabel = statusLabel();
    started = false;
    bucketStarted = millis();
    bucketMinute = 0;
    lastValid = bucketStarted;
    if (!Wire.begin(Configuration::sensorData, Configuration::sensorClock, Configuration::sensorBusFrequency)) {
        fail(Status::BusFailed); return;
    }
    Wire.setTimeOut(50);
    if (!command(0x3f86)) { fail(Status::BusFailed); return; }
    delay(500);
    if (!command(0x21b1)) { fail(Status::BusFailed); return; }
    started = true;
    lastPoll = millis();
    Serial.println("{\"event\":\"sensor_started\",\"source\":\"classroom_measurement\",\"address\":98,\"sda\":8,\"scl\":9}");
}
// 恢复仅重启测量，不清除历史、不换场次；停止等待期间主循环继续运行。
inline void requestRecovery(uint32_t now) {
    if (recovering) return;
    ++recoveryAttempts;
    hasRecovery = true;
    lastRecovery = recoveryStarted = now;
    recovering = true;
    recoveryPending = true;
    started = false;
    bucketFailed = true;
    if (bucketFailureReason == Data::MinuteFailure::None) bucketFailureReason = Data::MinuteFailure::Recovery;
    Recording::event("sensor_recovery_begin");
    Wire.end();
    if (!Wire.begin(Configuration::sensorData, Configuration::sensorClock, Configuration::sensorBusFrequency)) {
        recovering = false;
        fail(Status::BusFailed);
        return;
    }
    Wire.setTimeOut(50);
    // 传感器刚复位时停止指令也可能失败，仍等待后尝试启动周期测量。
    if (!command(0x3f86)) fail(Status::ReadFailed);
}
inline bool tick(uint32_t now) {
    measurementReady = false;
    bool changed = false;
    const uint32_t elapsedMinutes = (now - bucketStarted) / Configuration::measurementMinuteDuration;
    if (elapsedMinutes) {
        const bool valid = !bucketFailed && samples >= Configuration::minimumMinuteSamples;
        const auto reason = valid ? Data::MinuteFailure::None : bucketFailed ? bucketFailureReason : Data::MinuteFailure::TooFewSamples;
        Data::append({valid ? concentrationSum / samples : NAN, valid ? temperatureSum / samples : NAN,
            valid ? humiditySum / samples : NAN, bucketMinute, valid, samples, bucketErrors, reason});
        char detail[128];
        snprintf(detail, sizeof(detail), "minute=%lu;samples=%u;errors=%u;reason=%s;elapsed_minutes=%lu",
            static_cast<unsigned long>(bucketMinute), samples, bucketErrors, Data::minuteFailureName(reason),
            static_cast<unsigned long>(elapsedMinutes));
        Recording::event("minute_summary", detail);
        if (Serial) Serial.printf("{\"event\":\"minute_summary\",\"minute\":%lu,\"valid\":%s,\"samples\":%u,\"errors\":%u,\"reason\":\"%s\",\"elapsed_minutes\":%lu}\n",
            static_cast<unsigned long>(bucketMinute), valid ? "true" : "false", samples, bucketErrors,
            Data::minuteFailureName(reason), static_cast<unsigned long>(elapsedMinutes));
        for (uint32_t index = 1; index < elapsedMinutes && index < Configuration::historyCapacity; ++index)
            Data::append({NAN, NAN, NAN, bucketMinute + index, false, 0, 0, Data::MinuteFailure::MissingMinute});
        bucketMinute += elapsedMinutes;
        bucketStarted += elapsedMinutes * Configuration::measurementMinuteDuration;
        resetBucket();
        changed = true;
    }
    if (recovering) {
        if (now - recoveryStarted < Configuration::sensorStopDelay) return changed;
        recovering = false;
        if (!command(0x21b1)) { fail(Status::BusFailed); return true; }
        started = true;
        lastPoll = now;
        Recording::event("sensor_restart_sent");
        return true;
    }
    if ((!started || now - lastValid >= Configuration::sensorRecoveryInterval) &&
        now - lastRecovery >= Configuration::sensorRecoveryInterval) {
        requestRecovery(now);
        return true;
    }
    if (!started || now - lastPoll < Configuration::sensorPollInterval) return changed;
    lastPoll = now;
    uint16_t ready;
    if (!readWords(0xe4b8, &ready, 1)) return true;
    if (!(ready & 0x07ff)) {
        if (now - lastValid >= Configuration::sensorStaleInterval && status != Status::Stale) {
            fail(Status::Stale); return true;
        }
        return changed;
    }
    uint16_t words[3];
    if (!readWords(0xec05, words, 3)) return true;
    const bool wasFaulted = status != Status::Ready && status != Status::Waiting;
    status = Status::Ready;
    Data::sensorLabel = statusLabel();
    Data::current = {float(words[0]), -45.0f + 175.0f * words[1] / 65535.0f,
        100.0f * words[2] / 65535.0f, bucketMinute, true};
    measurementReady = true;
    if (words[0] == 0) {
        const auto raw = Data::current;
        fail(Status::InvalidValue);
        Data::current = raw;
        Data::current.valid = false;
        return true;
    }
    if (wasFaulted || recoveryPending) Recording::event("sensor_recovered");
    recoveryPending = false;
    lastValid = now;
    hasValidReading = true;
    ++samples;
    concentrationSum += Data::current.concentration;
    temperatureSum += Data::current.temperature;
    humiditySum += Data::current.humidity;
    if (Serial) Serial.printf("{\"event\":\"sensor_reading\",\"co2_ppm\":%u,\"temperature_c\":%.2f,\"humidity_pct\":%.2f,\"history_minutes\":%u}\n",
        words[0], Data::current.temperature, Data::current.humidity, Data::count);
    return true;
}
}
