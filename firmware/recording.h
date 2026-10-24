#pragma once
#include <stdio.h>
#include <string.h>
#include "data.h"
#include "prediction.h"
#ifndef AIR_UI_HOST
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <esp_system.h>
#endif

// 按场次逐条保存观测，管理存储状态和 USB 导出；不采集传感器或同步时钟。
namespace Recording {
enum class Status { Unavailable, Stopped, Active, Full, WriteFailed, ReadFailed };
enum class Source { PublicOfficeReplay, ClassroomMeasurement };
enum class SampleError { None, Unavailable, ReadFailed, InvalidValue, BusFailed, CrcFailed, Stale };
struct Session {
    Source source;
    const char* sampleOrigin;
    const char* modelSha256;
    uint64_t startedUptimeMilliseconds;
    const char* clockSource = "none";
    int32_t timezoneOffsetMinutes = 0;
    int64_t clockSetEpochMilliseconds = 0;
    uint64_t clockSetUptimeMilliseconds = 0;
    const char* environment = "unspecified";
};
struct Observation {
    Data::Sample sample;
    uint64_t uptimeMilliseconds;
    const char* timestamp = nullptr;
    const char* collectionTimestamp = nullptr;
    SampleError error = SampleError::None;
    const char* recordType = "measurement";
    const char* detail = "";
};
// 按分钟写预测快照，开关变化另写即时状态，便于按发布时间匹配实测值。
// baseline 是模型延续值，fan 是带假设通风情景的模拟值，两者都不是实测标签。
struct ForecastSnapshot {
    uint64_t issueUptimeMilliseconds = 0;
    uint32_t issueMinute = 0;
    const char* issueTimestamp = nullptr;
    uint32_t target5Minute = 0;
    uint32_t target15Minute = 0;
    float prediction5 = NAN;
    float prediction15 = NAN;
    float baseline5 = NAN;
    float baseline15 = NAN;
    float fan5 = NAN;
    float fan15 = NAN;
    unsigned status = 0;
    const char* modelSha256 = "";
    const char* detail = "";
};
inline Status status = Status::Unavailable;
inline bool mounted = false;
inline uint32_t totalRecords = 0, sessionRecords = 0, sessionCount = 0, sessionId = 0;
inline uint32_t totalBytes = 0, remainingBytes = 0;
inline bool resumeRequested = false;
inline uint32_t faultEvents = 0;
inline uint64_t lastMeasurementUptime = 0;
inline uint32_t measurementAgeSeconds = 0;
inline bool active() { return status == Status::Active; }
inline unsigned selectedEnvironment = 0;
inline const char* environmentName() { return Configuration::environmentNames[selectedEnvironment]; }
inline void selectEnvironment(unsigned index) {
    if (!active() && index < Configuration::environmentCount) selectedEnvironment = index;
}
inline const char* statusLabel() {
    switch (status) {
        case Status::Stopped: return "已停止";
        case Status::Active: return "记录中";
        case Status::Full: return "空间不足";
        case Status::WriteFailed: return "写入失败";
        case Status::ReadFailed: return "读取失败";
        default: return "存储未就绪";
    }
}
inline const char* errorName(SampleError error) {
    switch (error) {
        case SampleError::Unavailable: return "unavailable";
        case SampleError::ReadFailed: return "read_failed";
        case SampleError::InvalidValue: return "invalid_value";
        case SampleError::BusFailed: return "bus_failed";
        case SampleError::CrcFailed: return "crc_failed";
        case SampleError::Stale: return "stale";
        default: return "none";
    }
}
#ifndef AIR_UI_HOST
inline char currentPath[64];
inline Preferences settings;
inline bool settingsReady = false;
inline bool event(const char* code, const char* detail = "", SampleError error = SampleError::None);
inline bool remember(bool requested) {
    if (!settingsReady || settings.putUInt("environment", selectedEnvironment) != sizeof(uint32_t) ||
        settings.putBool("resume", requested) != 1) {
        Serial.println("{\"event\":\"recording_error\",\"code\":\"preferences_failed\"}");
        return false;
    }
    resumeRequested = requested;
    return true;
}
enum class LineResult { Complete, End, Incomplete, TooLong };
inline LineResult readLine(File& file, char* line) {
    unsigned length = 0;
    while (file.available()) {
        const int value = file.read();
        if (value < 0) return LineResult::Incomplete;
        if (value == '\n') { line[length] = 0; return LineResult::Complete; }
        if (length + 1 >= Configuration::recordingLineCapacity) return LineResult::TooLong;
        line[length++] = char(value);
    }
    line[length] = 0;
    return length ? LineResult::Incomplete : LineResult::End;
}
inline void updateSpace() {
    totalBytes = LittleFS.totalBytes();
    const size_t used = LittleFS.usedBytes();
    remainingBytes = used < totalBytes ? totalBytes - used : 0;
}
inline void fail(Status failure, const char* code) {
    status = failure;
    if (settingsReady && settings.putString("last_failure", code) == 0)
        Serial.println("{\"event\":\"recording_error\",\"code\":\"failure_metadata_write_failed\"}");
    if (mounted) updateSpace();
    Serial.printf("{\"event\":\"recording_error\",\"code\":\"%s\"}\n", code);
}
inline bool begin() {
    totalRecords = sessionRecords = sessionCount = sessionId = totalBytes = remainingBytes = 0;
    if (!settingsReady) settingsReady = settings.begin("air-record", false);
    resumeRequested = settingsReady && settings.getBool("resume", false);
    selectedEnvironment = settingsReady ? settings.getUInt("environment", 0) : 0;
    if (selectedEnvironment >= Configuration::environmentCount) selectedEnvironment = 0;
    mounted = LittleFS.begin(false);
    status = mounted ? Status::Stopped : Status::Unavailable;
    if (!mounted) { fail(Status::Unavailable, "mount_failed"); return false; }
    if (!LittleFS.exists(Configuration::recordingDirectory) && !LittleFS.mkdir(Configuration::recordingDirectory)) {
        fail(Status::WriteFailed, "directory_failed"); return false;
    }
    File directory = LittleFS.open(Configuration::recordingDirectory);
    if (!directory || !directory.isDirectory()) { fail(Status::ReadFailed, "directory_unreadable"); return false; }
    char line[Configuration::recordingLineCapacity];
    for (File file = directory.openNextFile(); file; file = directory.openNextFile()) {
        unsigned long identity = 0;
        int consumed = 0;
        if (file.isDirectory() || sscanf(file.name(), "%10lu.csv%n", &identity, &consumed) != 1 ||
            consumed == 0 || file.name()[consumed] != 0) { file.close(); continue; }
        if (identity > sessionId) sessionId = uint32_t(identity);
        ++sessionCount;
        if (readLine(file, line) != LineResult::Complete) { file.close(); continue; }
        LineResult result;
        while ((result = readLine(file, line)) == LineResult::Complete) ++totalRecords;
        file.close();
        if (result == LineResult::TooLong) { fail(Status::ReadFailed, "record_unreadable"); return false; }
    }
    directory.close();
    updateSpace();
    return true;
}
inline bool initialize() {
    if (mounted) { Serial.println("{\"event\":\"recording_error\",\"code\":\"already_initialized\"}"); return false; }
    if (!LittleFS.format() || !begin()) { fail(Status::Unavailable, "initialize_failed"); return false; }
    Serial.println("{\"event\":\"recording_initialized\"}");
    return true;
}
inline void stop() {
    if (active()) event("recording_stop");
    if (!remember(false)) { fail(Status::WriteFailed, "preferences_failed"); return; }
    if (active()) status = Status::Stopped;
}
inline bool writeLine(const char* text) {
    updateSpace();
    const size_t length = strlen(text);
    if (remainingBytes <= Configuration::recordingReserveBytes + length) { fail(Status::Full, "storage_full"); return false; }
    File file = LittleFS.open(currentPath, FILE_APPEND);
    if (!file) { fail(Status::WriteFailed, "open_failed"); return false; }
    const size_t previousSize = file.size();
    const size_t written = file.write(reinterpret_cast<const uint8_t*>(text), length);
    file.flush();
    const bool complete = written == length && file.size() == previousSize + length && file.getWriteError() == 0;
    file.close();
    if (!complete) { fail(Status::WriteFailed, "write_failed"); return false; }
    updateSpace();
    return true;
}
inline bool start(const Session& session) {
    if (!mounted || status == Status::ReadFailed || status == Status::WriteFailed) return false;
    if (!remember(true)) { fail(Status::WriteFailed, "preferences_failed"); return false; }
    if (sessionId == UINT32_MAX) { fail(Status::WriteFailed, "session_id_exhausted"); return false; }
    ++sessionId;
    snprintf(currentPath, sizeof(currentPath), "%s/%010lu.csv", Configuration::recordingDirectory,
        static_cast<unsigned long>(sessionId));
    if (LittleFS.exists(currentPath)) { fail(Status::WriteFailed, "session_exists"); return false; }
    char header[Configuration::recordingLineCapacity];
    const int length = snprintf(header, sizeof(header),
        "{\"event\":\"recording_session\",\"format\":%u,\"device_id\":\"%012llx\",\"session_id\":\"%010lu\",\"source\":\"%s\",\"sample_origin\":\"%s\",\"model_sha256\":\"%s\",\"started_uptime_ms\":%llu,\"clock_source\":\"%s\",\"timezone_offset_minutes\":%ld,\"clock_set_epoch_ms\":%lld,\"clock_set_uptime_ms\":%llu,\"environment\":\"%s\"}\n",
        Configuration::recordingFormatVersion, static_cast<unsigned long long>(ESP.getEfuseMac()),
        static_cast<unsigned long>(sessionId), session.source == Source::PublicOfficeReplay ? "public_office_replay" : "classroom_measurement",
        session.sampleOrigin ? session.sampleOrigin : "", session.modelSha256 ? session.modelSha256 : "",
        static_cast<unsigned long long>(session.startedUptimeMilliseconds), session.clockSource ? session.clockSource : "none",
        long(session.timezoneOffsetMinutes), static_cast<long long>(session.clockSetEpochMilliseconds),
        static_cast<unsigned long long>(session.clockSetUptimeMilliseconds), session.environment);
    if (length < 0 || unsigned(length) >= sizeof(header)) { fail(Status::WriteFailed, "metadata_too_long"); return false; }
    if (!writeLine(header)) return false;
    ++sessionCount;
    sessionRecords = 0;
    status = Status::Active;
    char reason[48];
    snprintf(reason, sizeof(reason), "reset_reason=%u", unsigned(esp_reset_reason()));
    event("session_start", reason);
    if (settingsReady) {
        const String previous = settings.getString("last_failure", "");
        if (previous.length() && event("previous_storage_failure", previous.c_str())) settings.remove("last_failure");
    }
    return active();
}
inline bool append(const Observation& observation, const Prediction::Result& prediction) {
    if (!active()) return false;
    const auto& sample = observation.sample;
    const bool valid = sample.valid && isfinite(sample.concentration) && isfinite(sample.temperature) && isfinite(sample.humidity) &&
        observation.error == SampleError::None;
    const SampleError error = !valid && strcmp(observation.recordType, "measurement") == 0 && observation.error == SampleError::None ? SampleError::InvalidValue : observation.error;
    char line[Configuration::recordingLineCapacity];
    const int length = snprintf(line, sizeof(line), "%lu,%llu,%lu,%s,%s,%u,%.3f,%.3f,%.3f,%u,%s,%u,%.3f,%.3f,%s,%s\n",
        static_cast<unsigned long>(sessionRecords + 1), static_cast<unsigned long long>(observation.uptimeMilliseconds),
        static_cast<unsigned long>(sample.minute), observation.timestamp ? observation.timestamp : "",
        observation.collectionTimestamp ? observation.collectionTimestamp : "",
        observation.collectionTimestamp && observation.collectionTimestamp[0] ? 1u : 0u,
        sample.concentration, sample.temperature, sample.humidity, valid ? 1u : 0u, errorName(error),
        unsigned(prediction.status), prediction.nearConcentration, prediction.farConcentration, observation.recordType, observation.detail);
    if (length < 0 || unsigned(length) >= sizeof(line)) { fail(Status::WriteFailed, "record_too_long"); return false; }
    if (!writeLine(line)) return false;
    ++sessionRecords;
    ++totalRecords;
    if (strcmp(observation.recordType, "measurement") == 0) lastMeasurementUptime = observation.uptimeMilliseconds;
    if (observation.error != SampleError::None) ++faultEvents;
    return true;
}
// 预测快照使用 P 标记保存在 LittleFS 内部，导出时转换为 recording_forecast 行。
// 这样既不改变旧 measurement 行的字段顺序，也能让旧格式场次继续被读取和导出。
inline bool appendForecast(const ForecastSnapshot& forecast) {
    if (!active()) return false;
    char line[Configuration::recordingLineCapacity];
    const int length = snprintf(line, sizeof(line),
        "P,%lu,%llu,%lu,%s,%u,%lu,%lu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%s,%s\n",
        static_cast<unsigned long>(sessionRecords + 1),
        static_cast<unsigned long long>(forecast.issueUptimeMilliseconds),
        static_cast<unsigned long>(forecast.issueMinute), forecast.issueTimestamp ? forecast.issueTimestamp : "",
        forecast.issueTimestamp && forecast.issueTimestamp[0] ? 1u : 0u,
        static_cast<unsigned long>(forecast.target5Minute), static_cast<unsigned long>(forecast.target15Minute),
        forecast.prediction5, forecast.prediction15, forecast.baseline5, forecast.baseline15,
        forecast.fan5, forecast.fan15, forecast.status,
        forecast.modelSha256 ? forecast.modelSha256 : "", forecast.detail ? forecast.detail : "");
    if (length < 0 || unsigned(length) >= sizeof(line)) { fail(Status::WriteFailed, "forecast_too_long"); return false; }
    if (!writeLine(line)) return false;
    ++sessionRecords;
    ++totalRecords;
    return true;
}
inline bool event(const char* code, const char* detail, SampleError error) {
    const uint64_t uptime = uint64_t(esp_timer_get_time() / 1000);
    return append({{NAN, NAN, NAN, uint32_t(uptime / Configuration::measurementMinuteDuration), false},
        uptime, nullptr, nullptr, error, code, detail}, {});
}
inline void report() {
    if (mounted) updateSpace();
    Serial.printf("{\"event\":\"recording_status\",\"mounted\":%s,\"active\":%s,\"status\":%u,\"session_id\":\"%010lu\",\"sessions\":%lu,\"records\":%lu,\"session_records\":%lu,\"total_bytes\":%lu,\"remaining_bytes\":%lu}\n",
        mounted ? "true" : "false", active() ? "true" : "false", unsigned(status),
        static_cast<unsigned long>(sessionId), static_cast<unsigned long>(sessionCount), static_cast<unsigned long>(totalRecords),
        static_cast<unsigned long>(sessionRecords), static_cast<unsigned long>(totalBytes), static_cast<unsigned long>(remainingBytes));
}
// USB 导出统一处理连接暂态、发送背压与短写；底层队列接受不代表主机已收到。
inline bool writeExportBytes(const char* data, size_t length) {
    size_t offset = 0;
    uint32_t progressed = millis();
    while (offset < length) {
        if (uint32_t(millis() - progressed) >= Configuration::recordingExportProgressTimeout) return false;
        if (Serial) {
            const int available = Serial.availableForWrite();
            if (available > 0) {
                size_t count = length - offset;
                if (count > size_t(available)) count = size_t(available);
                if (count > Configuration::recordingExportChunkBytes) count = Configuration::recordingExportChunkBytes;
                const size_t written = Serial.write(reinterpret_cast<const uint8_t*>(data + offset), count);
                if (written > count) return false;
                if (written) {
                    offset += written;
                    progressed = millis();
                    delay(0);
                    continue;
                }
            }
        }
        delay(Configuration::recordingExportRetryInterval);
    }
    return true;
}
inline bool writeExportLine(const char* line, const char* prefix = "") {
    return writeExportBytes(prefix, strlen(prefix)) && writeExportBytes(line, strlen(line)) &&
        writeExportBytes("\r\n", 2);
}
inline void abortExport() {
    event("export_aborted", "usb_export_timeout");
    // 换行隔开可能只发出一半的行；完全断连时仅能保留板端事件，主机仍会超时。
    writeExportLine("{\"event\":\"recording_error\",\"code\":\"usb_export_timeout\"}", "\r\n");
}
inline void exportAll() {
    event("export_begin");
    if (!mounted) { fail(Status::Unavailable, "storage_unavailable"); return; }
    File directory = LittleFS.open(Configuration::recordingDirectory);
    if (!directory || !directory.isDirectory()) { fail(Status::ReadFailed, "directory_unreadable"); return; }
    char line[Configuration::recordingLineCapacity];
    snprintf(line, sizeof(line), "{\"event\":\"recording_export_begin\",\"format\":%u}", Configuration::recordingFormatVersion);
    if (!writeExportLine(line)) { directory.close(); abortExport(); return; }
    uint32_t exportedRecords = 0, exportedSessions = 0, incompleteRows = 0;
    for (File file = directory.openNextFile(); file; file = directory.openNextFile()) {
        unsigned long identity = 0;
        int consumed = 0;
        if (file.isDirectory() || sscanf(file.name(), "%10lu.csv%n", &identity, &consumed) != 1 ||
            consumed == 0 || file.name()[consumed] != 0) { file.close(); continue; }
        const auto headerResult = readLine(file, line);
        if (headerResult == LineResult::End || headerResult == LineResult::Incomplete) { ++incompleteRows; file.close(); continue; }
        if (headerResult != LineResult::Complete) { file.close(); directory.close(); fail(Status::ReadFailed, "metadata_unreadable"); return; }
        if (!writeExportLine(line)) { file.close(); directory.close(); abortExport(); return; }
        ++exportedSessions;
        LineResult result;
        while ((result = readLine(file, line)) == LineResult::Complete) {
            const bool forecast = line[0] == 'P' && line[1] == ',';
            if (!writeExportLine(forecast ? line + 2 : line, forecast ? "recording_forecast," : "recording_row,")) {
                file.close(); directory.close(); abortExport(); return;
            }
            ++exportedRecords;
        }
        file.close();
        if (result == LineResult::TooLong) { directory.close(); fail(Status::ReadFailed, "record_unreadable"); return; }
        if (result == LineResult::Incomplete) ++incompleteRows;
    }
    directory.close();
    snprintf(line, sizeof(line), "{\"event\":\"recording_export_end\",\"records\":%lu,\"sessions\":%lu,\"incomplete_rows\":%lu}",
        static_cast<unsigned long>(exportedRecords), static_cast<unsigned long>(exportedSessions), static_cast<unsigned long>(incompleteRows));
    if (!writeExportLine(line)) { abortExport(); return; }
    event("export_end");
}
#endif
}
