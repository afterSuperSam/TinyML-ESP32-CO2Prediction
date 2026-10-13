#pragma once
#include "data.h"

// 统一模型调用、历史校验和失败状态，不让页面管理推理细节。
namespace Prediction {
enum class Status { Success = 0, NotReady = 1, InvalidHistory = 2, OutsideCalibration = 3, InferenceFailed = 4, Disabled = 5 };
struct Result {
    Status status = Status::NotReady;
    float nearConcentration = NAN, farConcentration = NAN;
    // 基线预测之外的风扇情景仅用于界面模拟，不代表已测得的风量效果。
    float fanNearConcentration = NAN, fanFarConcentration = NAN;
    uint32_t durationMicroseconds = 0;
};
using Runner = Result (*)(const float*, bool);
inline Runner runner = nullptr;
inline Result result;
inline bool enabled = Configuration::predictionDefaultEnabled;
inline bool settingSaved = true;
inline const char* settingError = "";
inline uint32_t lastInferenceMinute = UINT32_MAX;
inline void applyFanSimulation(Result& value) {
    if (value.status != Status::Success) return;
    value.fanNearConcentration = value.nearConcentration -
        fmaxf(0.0f, value.nearConcentration - Configuration::fanSimulationOutdoorPpm) *
        Configuration::fanSimulationRemovalNear;
    value.fanFarConcentration = value.farConcentration -
        fmaxf(0.0f, value.farConcentration - Configuration::fanSimulationOutdoorPpm) *
        Configuration::fanSimulationRemovalFar;
}
inline void begin(Runner function) { runner = function; result = {}; lastInferenceMinute = UINT32_MAX; }
inline void setEnabled(bool requested) {
    enabled = requested;
    result = {};
    lastInferenceMinute = UINT32_MAX;
    if (!enabled) result.status = Status::Disabled;
}
inline void update() {
    if (!enabled) { result = {}; result.status = Status::Disabled; return; }
    if (Data::live && !Data::latest().valid) { result = {}; result.status = Status::InvalidHistory; return; }
    float values[Configuration::predictionHistoryMinutes * ModelAssets::featureCount];
    if (!Data::predictionHistory(values)) { result = {}; result.status = Status::InvalidHistory; return; }
    if (!runner) { result = {}; return; }
    const uint32_t minute = Data::history[Configuration::historyCapacity - 1].minute;
    // 原始读数变化不改变已结算的模型输入，同一分钟成功结果直接复用。
    if (result.status == Status::Success && lastInferenceMinute == minute) return;
    result = runner(values, true);
    lastInferenceMinute = minute;
    if (result.status != Status::Success) {
        result.nearConcentration = result.farConcentration = NAN;
        result.fanNearConcentration = result.fanFarConcentration = NAN;
    }
}
inline const char* statusLabel() {
    switch (result.status) {
        case Status::Success: return "可预测";
        case Status::InvalidHistory:
            return Data::live && Data::count < Configuration::predictionHistoryMinutes ? "采集中" : "历史缺口";
        case Status::OutsideCalibration: return "未覆盖";
        case Status::InferenceFailed: return "预测失败";
        case Status::Disabled: return "已关闭";
        default: return "未就绪";
    }
}
static_assert(unsigned(Status::Success) == 0 && unsigned(Status::InvalidHistory) == 2 &&
    unsigned(Status::InferenceFailed) == 4 && unsigned(Status::Disabled) == 5, "Recording status compatibility");
}
