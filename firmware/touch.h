#pragma once
#ifndef AIR_UI_HOST
#include <Arduino.h>
#include <Preferences.h>
#endif
#include <stddef.h>
#include <string.h>
#include "canvas.h"

// 独立软件 SPI 与四角校准；校准按版本、分辨率及方向校验后存入 NVS。
namespace Touch {
inline bool active = true, saved = false;
inline float transformation[2][3];
inline uint16_t calibrationRaw[4][2];
inline uint16_t samples[2][Configuration::calibrationSamples];
inline uint8_t calibrationIndex = 0, sampleCount = 0;
inline bool waitingForRelease = true, tapDelivered = false;
inline uint32_t releaseStarted = 0, lastSample = 0;
inline const uint16_t targets[4][2] = {
    {Configuration::calibrationMargin, Configuration::calibrationMargin},
    {Configuration::width - 1 - Configuration::calibrationMargin, Configuration::calibrationMargin},
    {Configuration::width - 1 - Configuration::calibrationMargin, Configuration::height - 1 - Configuration::calibrationMargin},
    {Configuration::calibrationMargin, Configuration::height - 1 - Configuration::calibrationMargin}
};
struct CalibrationRecord {
    uint32_t magic, version, width, height, orientation;
    float matrix[2][3];
    uint32_t checksum;
};
inline uint32_t checksum(const CalibrationRecord& record) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < offsetof(CalibrationRecord, checksum); ++i) value = (value ^ bytes[i]) * 16777619u;
    return value;
}
inline bool validRecord(const CalibrationRecord& record) {
    using namespace Configuration;
    if (record.magic != calibrationMagic || record.version != calibrationVersion || record.width != width ||
        record.height != height || record.orientation != displayOrientation || record.checksum != checksum(record)) return false;
    for (unsigned axis = 0; axis < 2; ++axis) {
        for (unsigned coefficient = 0; coefficient < 3; ++coefficient) {
            const float limit = coefficient < 2 ? calibrationCoefficientLimit : calibrationOffsetLimit;
            if (!isfinite(record.matrix[axis][coefficient]) || fabsf(record.matrix[axis][coefficient]) > limit) return false;
        }
    }
    const float determinant = record.matrix[0][0] * record.matrix[1][1] - record.matrix[0][1] * record.matrix[1][0];
    return fabsf(determinant) > 0.000001f;
}
inline bool load() {
    saved = false;
    Preferences preferences;
    if (!preferences.begin("air-touch", true)) { Serial.println("Calibration storage unavailable or empty"); return false; }
    CalibrationRecord record{};
    const size_t size = preferences.getBytesLength("matrix");
    const size_t read = size == sizeof(record) ? preferences.getBytes("matrix", &record, sizeof(record)) : 0;
    preferences.end();
    if (read != sizeof(record) || !validRecord(record)) {
        Serial.println("Calibration missing, incompatible, or corrupt; recalibration required");
        return false;
    }
    memcpy(transformation, record.matrix, sizeof(transformation));
    saved = true;
    return true;
}
inline bool save() {
    using namespace Configuration;
    CalibrationRecord record{};
    record.magic = calibrationMagic; record.version = calibrationVersion;
    record.width = width; record.height = height; record.orientation = displayOrientation;
    memcpy(record.matrix, transformation, sizeof(transformation));
    record.checksum = checksum(record);
    if (!validRecord(record)) { Serial.println("Calibration save rejected: invalid matrix"); return false; }
    Preferences preferences;
    if (!preferences.begin("air-touch", false)) { Serial.println("Calibration storage open failed"); return false; }
    const size_t written = preferences.putBytes("matrix", &record, sizeof(record));
    preferences.end();
    if (written != sizeof(record)) { Serial.println("Calibration storage write failed"); return false; }
    return true;
}
inline bool mapCoordinates(uint16_t rawX, uint16_t rawY, int& x, int& y) {
    x = lroundf(transformation[0][0] * rawX + transformation[0][1] * rawY + transformation[0][2]);
    y = lroundf(transformation[1][0] * rawX + transformation[1][1] * rawY + transformation[1][2]);
    return x >= 0 && x < Configuration::width && y >= 0 && y < Configuration::height;
}
inline uint8_t transfer(uint8_t outgoing) {
    using namespace Configuration;
    uint8_t incoming = 0;
    for (int bit = 7; bit >= 0; --bit) {
        digitalWrite(touchDataOut, (outgoing >> bit) & 1);
        delayMicroseconds(touchHalfPeriod);
        digitalWrite(touchClock, HIGH);
        delayMicroseconds(touchHalfPeriod);
        incoming = uint8_t((incoming << 1) | digitalRead(touchDataIn));
        digitalWrite(touchClock, LOW);
    }
    return incoming;
}
inline uint16_t coordinate(uint8_t command) {
    digitalWrite(Configuration::touchSelect, LOW);
    transfer(command);
    delayMicroseconds(10);
    const uint16_t high = transfer(0), low = transfer(0);
    digitalWrite(Configuration::touchSelect, HIGH);
    return ((high << 8) | low) >> 3;
}
inline void showTarget(const char* message = nullptr) {
    using namespace Configuration;
    Canvas::clear(background);
    Canvas::centered(0, 83, 320, "触摸校准", ink);
    char label[48];
    snprintf(label, sizeof(label), "按住十字中心  %u / 4", calibrationIndex + 1);
    Canvas::centered(0, 110, 320, label, sea);
    Canvas::centered(0, 138, 320, "目标移动后松手，再按下一个", muted, FontAssets::small);
    if (message) Canvas::centered(0, 169, 320, message, terracotta, FontAssets::small);
    const int x = targets[calibrationIndex][0], y = targets[calibrationIndex][1];
    Canvas::rawRectangle(x - markerRadius, y - 1, markerRadius * 2 + 1, 3, sea);
    Canvas::rawRectangle(x - 1, y - markerRadius, 3, markerRadius * 2 + 1, sea);
    Canvas::present();
    Serial.printf("Calibration target %u/4: pixel=(%d,%d)\n", calibrationIndex + 1, x, y);
}
inline void restart(const char* message = nullptr) {
    active = true; calibrationIndex = sampleCount = 0;
    waitingForRelease = true; releaseStarted = 0; tapDelivered = false;
    showTarget(message);
}
inline uint16_t median(uint16_t* values) {
    for (uint8_t i = 1; i < Configuration::calibrationSamples; ++i) {
        const uint16_t value = values[i];
        int position = i - 1;
        while (position >= 0 && values[position] > value) { values[position + 1] = values[position]; --position; }
        values[position + 1] = value;
    }
    return values[Configuration::calibrationSamples / 2];
}
inline const char* calculate() {
    using namespace Configuration;
    const float horizontalX = float(calibrationRaw[1][0]) - calibrationRaw[0][0];
    const float horizontalY = float(calibrationRaw[1][1]) - calibrationRaw[0][1];
    const float verticalX = float(calibrationRaw[3][0]) - calibrationRaw[0][0];
    const float verticalY = float(calibrationRaw[3][1]) - calibrationRaw[0][1];
    const float determinant = horizontalX * verticalY - horizontalY * verticalX;
    if (fabsf(determinant) < minimumCalibrationArea) return "校准失败：坐标变化不足";
    for (unsigned axis = 0; axis < 2; ++axis) {
        const float horizontalPixels = float(targets[1][axis]) - targets[0][axis];
        const float verticalPixels = float(targets[3][axis]) - targets[0][axis];
        transformation[axis][0] = (horizontalPixels * verticalY - verticalPixels * horizontalY) / determinant;
        transformation[axis][1] = (verticalPixels * horizontalX - horizontalPixels * verticalX) / determinant;
        transformation[axis][2] = targets[0][axis] - transformation[axis][0] * calibrationRaw[0][0]
            - transformation[axis][1] * calibrationRaw[0][1];
        const float predicted = transformation[axis][0] * calibrationRaw[2][0] + transformation[axis][1] * calibrationRaw[2][1] + transformation[axis][2];
        if (!isfinite(predicted) || fabsf(predicted - targets[2][axis]) > maximumCalibrationError)
            return "校准失败：请准确点击四角";
    }
    return nullptr;
}
inline void collect(uint16_t x, uint16_t y) {
    using namespace Configuration;
    samples[0][sampleCount] = x; samples[1][sampleCount] = y;
    if (++sampleCount < calibrationSamples) return;
    const uint16_t medianX = median(samples[0]), medianY = median(samples[1]);
    sampleCount = 0;
    if (samples[0][calibrationSamples - 1] - samples[0][0] > calibrationSpread ||
        samples[1][calibrationSamples - 1] - samples[1][0] > calibrationSpread) {
        showTarget("读数不稳定，请按住中心"); return;
    }
    calibrationRaw[calibrationIndex][0] = medianX; calibrationRaw[calibrationIndex][1] = medianY;
    Serial.printf("Calibration captured: raw=(%u,%u)\n", medianX, medianY);
    if (++calibrationIndex < 4) showTarget();
    else {
        const char* error = calculate();
        if (error) { Serial.println(error); restart(error); }
        else {
            active = false; saved = save();
            Serial.println(saved ? "校准完成并已保存。" : "校准完成，但保存失败，下次启动需要重新校准。");
        }
    }
    waitingForRelease = true; releaseStarted = 0;
}
inline void begin() {
    using namespace Configuration;
    pinMode(touchClock, OUTPUT); pinMode(touchDataOut, OUTPUT); pinMode(touchDataIn, INPUT);
    pinMode(touchSelect, OUTPUT); pinMode(touchInterrupt, INPUT_PULLUP);
    digitalWrite(touchSelect, HIGH); digitalWrite(touchClock, LOW);
    active = !load();
    if (active) restart();
}
inline bool poll(int& x, int& y, uint32_t now) {
    using namespace Configuration;
    const bool pressed = digitalRead(touchInterrupt) == LOW;
    if (waitingForRelease) {
        if (pressed) releaseStarted = 0;
        else if (releaseStarted == 0) releaseStarted = now;
        else if (now - releaseStarted >= releaseDuration) { waitingForRelease = false; tapDelivered = false; }
        return false;
    }
    if (!pressed) {
        sampleCount = 0;
        if (tapDelivered) { waitingForRelease = true; releaseStarted = now; }
        return false;
    }
    if ((!active && tapDelivered) || now - lastSample < touchInterval) return false;
    lastSample = now;
    const uint16_t rawX = coordinate(0xD0), rawY = coordinate(0x90);
    if (!rawX || rawX == 4095 || !rawY || rawY == 4095) {
        sampleCount = 0; Serial.println("Invalid touch sample: check wiring and controller"); return false;
    }
    if (active) { collect(rawX, rawY); return false; }
    tapDelivered = true;
    return mapCoordinates(rawX, rawY, x, y);
}
}
