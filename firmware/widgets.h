#pragma once
#include <stdio.h>
#include <string.h>
#include "canvas.h"
#include "control.h"
#include "replay.h"
#include "clock.h"

// 页面共用状态与导航，业务动作交给集成模块。
namespace Interface {
enum class Page : uint8_t { Home, Control, Settings, Debug, Recording };
enum class Action : uint8_t { None, Calibrate, RestartReplay, StartRecording, StopRecording, InitializeRecording, TogglePrediction };
struct Diagnostics {
    uint32_t currentMinute = 0, currentMinuteSamples = 0, currentMinuteErrors = 0;
    uint32_t sensorErrors = 0, recoveryAttempts = 0, measurementAgeSeconds = 0;
    uint32_t continuousValidMinutes = 0, firstBadMinute = 0, firstBadSamples = 0, firstBadErrors = 0;
    unsigned firstBadOffset = 0;
    uint32_t lastErrorAgeSeconds = 0, lastRecoveryAgeSeconds = 0;
    bool hasBadMinute = false, hasValidReading = false, hasSensorError = false, hasRecovery = false;
    const char* firstBadReason = "无";
    const char* sensorStatus = "等待数据";
    const char* lastSensorError = "无";
    uint32_t tensorArenaUsedBytes = 0, tensorArenaCapacityBytes = 0;
    const char* modelHash = "";
    uint64_t uptimeMilliseconds = 0;
    uint32_t storageTotalBytes = 0, storageRemainingBytes = 0, recordingSessions = 0;
    const char* recordingStatus = "未挂载";
    bool storageMounted = false, predictionSettingSaved = true;
    const char* predictionSettingError = "";
};
inline Diagnostics diagnostics;
inline Page page = Page::Home;
inline uint32_t freeMemory = 0;
inline float framesPerSecond = 0;
inline bool calibrationSaved = false, debugLogs = false;
inline void pill(int x, int y, int width, const char* label, uint16_t fill, uint16_t ink) {
    Canvas::rounded(x, y, width, 24, 6, fill);
    Canvas::centered(x, y + 3, width, label, ink, FontAssets::small);
}
inline uint16_t predictionColor() {
    if (!Prediction::enabled) return Configuration::predictionDisabledColor;
    if ((Data::live && !Data::latest().valid) || Prediction::result.status == Prediction::Status::InferenceFailed)
        return Configuration::predictionFaultColor;
    return Prediction::result.status == Prediction::Status::Success ?
        Configuration::predictionReadyColor : Configuration::predictionWaitingColor;
}
inline void navigation() {
    using namespace Configuration;
    Canvas::rectangle(0, 204, 320, 36, surface);
    Canvas::rectangle(14, 204, 292, 1, divider);
    const char* labels[] = {"主页", "控制", "设置"};
    const unsigned active = page == Page::Debug || page == Page::Recording ? 2 : unsigned(page);
    for (unsigned index = 0; index < 3; ++index) {
        const int x = index * 106;
        Canvas::centered(x, 212, 106, labels[index], active == index ? ink : muted);
        if (index == active) Canvas::rounded(x + 40, 234, 26, 3, 1, peach);
    }
}
inline void settingRow(int y, const char* label, const char* value) {
    using namespace Configuration;
    Canvas::text(16, y + 7, label);
    Canvas::text(304 - Canvas::textWidth(value, FontAssets::small), y + 10, value, ink, FontAssets::small);
    Canvas::rectangle(16, y + 33, 288, 1, divider);
}
// 设置页使用较大的卡片，给触摸留出稳定的有效区域。
inline void settingTile(int x, int y, int width, int height, const char* label,
                        const char* value, bool selected = false) {
    using namespace Configuration;
    Canvas::rounded(x, y, width, height, 8, selected ? peachWash : surface);
    Canvas::text(x + 10, y + 9, label, ink, FontAssets::small);
    Canvas::text(x + 10, y + 31, value, selected ? terracotta : muted, FontAssets::small);
    Canvas::rectangle(x + 8, y + height - 1, width - 16, 1, selected ? peach : divider);
}
}
