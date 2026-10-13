#pragma once
#include <math.h>
#include <stdint.h>
#include "config.h"
#include "model_assets.h"

// 保存有时间序号的连续历史；不负责采集、预测或页面。
namespace Data {
enum class MinuteFailure { None, TooFewSamples, ReadFailed, CrcFailed, InvalidValue, BusFailed,
    Stale, Recovery, MissingMinute, NonFinite, SequenceGap };
struct Sample {
    float concentration, temperature, humidity;
    uint32_t minute;
    bool valid;
    unsigned sampleCount = 0, errorCount = 0;
    MinuteFailure failureReason = MinuteFailure::None;
};
struct HistoryDiagnostics {
    unsigned continuousValidMinutes = 0;
    bool ready = false, hasBadMinute = false;
    uint32_t firstBadMinute = 0;
    unsigned firstBadOffsetMinutes = 0, firstBadSamples = 0, firstBadErrors = 0;
    MinuteFailure firstBadReason = MinuteFailure::None;
};
inline const char* minuteFailureLabel(MinuteFailure reason) {
    switch (reason) {
        case MinuteFailure::TooFewSamples: return "样本不足";
        case MinuteFailure::ReadFailed: return "读取失败";
        case MinuteFailure::CrcFailed: return "校验失败";
        case MinuteFailure::InvalidValue: return "无效读数";
        case MinuteFailure::BusFailed: return "连接失败";
        case MinuteFailure::Stale: return "读数过期";
        case MinuteFailure::Recovery: return "恢复测量";
        case MinuteFailure::MissingMinute: return "采样暂停";
        case MinuteFailure::NonFinite: return "无效均值";
        case MinuteFailure::SequenceGap: return "分钟不连续";
        default: return "无";
    }
}
inline const char* minuteFailureName(MinuteFailure reason) {
    switch (reason) {
        case MinuteFailure::TooFewSamples: return "too_few_samples";
        case MinuteFailure::ReadFailed: return "read_failed";
        case MinuteFailure::CrcFailed: return "crc_failed";
        case MinuteFailure::InvalidValue: return "invalid_value";
        case MinuteFailure::BusFailed: return "bus_failed";
        case MinuteFailure::Stale: return "stale";
        case MinuteFailure::Recovery: return "recovery";
        case MinuteFailure::MissingMinute: return "missing_minute";
        case MinuteFailure::NonFinite: return "non_finite";
        case MinuteFailure::SequenceGap: return "sequence_gap";
        default: return "none";
    }
}
inline Sample history[Configuration::historyCapacity];
inline unsigned count = 0;
inline unsigned historyMinutes = 30;
inline bool live = false;
inline Sample current = {NAN, NAN, NAN, 0, false};
inline const char* sensorLabel = "等待数据";
inline const Sample& latest() { return live ? current : history[Configuration::historyCapacity - 1]; }
inline void clear() {
    count = 0;
    current = {NAN, NAN, NAN, 0, false};
    for (auto& sample : history) sample = {NAN, NAN, NAN, 0, false};
}
inline void append(const Sample& sample) {
    for (unsigned index = 1; index < Configuration::historyCapacity; ++index) history[index - 1] = history[index];
    history[Configuration::historyCapacity - 1] = sample;
    if (count < Configuration::historyCapacity) ++count;
}
inline HistoryDiagnostics historyDiagnostics() {
    HistoryDiagnostics result;
    for (unsigned offset = 0; offset < count; ++offset) {
        const unsigned index = Configuration::historyCapacity - 1 - offset;
        const auto& sample = history[index];
        if (!sample.valid || !isfinite(sample.concentration) ||
            (offset && sample.minute + 1 != history[index + 1].minute)) break;
        ++result.continuousValidMinutes;
    }
    const unsigned width = Configuration::predictionHistoryMinutes;
    const unsigned available = count < width ? count : width;
    const unsigned first = Configuration::historyCapacity - available;
    for (unsigned offset = 0; offset < available; ++offset) {
        const auto& sample = history[first + offset];
        MinuteFailure reason = sample.failureReason;
        if (!sample.valid && reason == MinuteFailure::None) reason = MinuteFailure::TooFewSamples;
        if (sample.valid && !isfinite(sample.concentration)) reason = MinuteFailure::NonFinite;
        if (reason == MinuteFailure::None && offset && sample.minute != history[first + offset - 1].minute + 1)
            reason = MinuteFailure::SequenceGap;
        if (reason == MinuteFailure::None) continue;
        result.hasBadMinute = true;
        result.firstBadMinute = sample.minute;
        result.firstBadOffsetMinutes = width - available + offset + 1;
        result.firstBadSamples = sample.sampleCount;
        result.firstBadErrors = sample.errorCount;
        result.firstBadReason = reason;
        break;
    }
    result.ready = count >= width && !result.hasBadMinute;
    return result;
}
inline bool predictionHistory(float* values) {
    if (!values || !historyDiagnostics().ready) return false;
    const unsigned first = Configuration::historyCapacity - Configuration::predictionHistoryMinutes;
    for (unsigned index = 0; index < Configuration::predictionHistoryMinutes; ++index)
        values[index] = history[first + index].concentration;
    return true;
}
}
