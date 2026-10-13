#pragma once
#include "widgets.h"

// 展示 CO2 历史、基线预测和风扇情景预测；风扇曲线由模型层明确标记为模拟值。
namespace HomePage {
struct ChartBounds { float lowest, highest; };
inline bool hasPrediction() {
    return Prediction::enabled && Prediction::result.status == Prediction::Status::Success;
}
inline ChartBounds chartBounds() {
    using namespace Configuration;
    float lowest = minimumChartConcentration, highest = minimumChartMaximum;
    const unsigned start = historyCapacity - 1 - Data::historyMinutes;
    for (unsigned index = start; index < historyCapacity; ++index) {
        const auto& sample = Data::history[index];
        if (!sample.valid || !isfinite(sample.concentration)) continue;
        lowest = fminf(lowest, sample.concentration - chartStep / 4);
        highest = fmaxf(highest, sample.concentration + chartStep / 4);
    }
    if (hasPrediction()) {
        const auto& value = Prediction::result;
        const float values[] = {value.nearConcentration, value.farConcentration,
            value.fanNearConcentration, value.fanFarConcentration};
        for (float point : values) {
            if (!isfinite(point)) continue;
            lowest = fminf(lowest, point - chartStep / 4);
            highest = fmaxf(highest, point + chartStep / 4);
        }
    }
    return {floorf(lowest / chartStep) * chartStep, ceilf(highest / chartStep) * chartStep};
}
inline int chartY(float value, const ChartBounds& bounds, int top, int height) {
    const float fraction = (value - bounds.lowest) / (bounds.highest - bounds.lowest);
    return top + height - lroundf(fraction * height);
}
inline void dashedPrediction(int startX, int endX, int top, int chartHeight, const ChartBounds& bounds,
                             float origin, float nearValue, float farValue, uint16_t color) {
    const float values[] = {origin, nearValue, farValue};
    const unsigned minutes[] = {0, Configuration::predictionNearMinutes, Configuration::predictionFarMinutes};
    for (int section = 0; section < 2; ++section) {
        const int x = startX + (endX - startX) * int(minutes[section]) / Configuration::predictionFarMinutes;
        const int end = startX + (endX - startX) * int(minutes[section + 1]) / Configuration::predictionFarMinutes;
        if (end <= x || !isfinite(values[section]) || !isfinite(values[section + 1])) continue;
        for (int point = x; point < end; point += 6) {
            const int next = point + 3 < end ? point + 3 : end;
            const float fraction = float(point - x) / float(end - x);
            const float nextFraction = float(next - x) / float(end - x);
            const float a = values[section] + (values[section + 1] - values[section]) * fraction;
            const float b = values[section] + (values[section + 1] - values[section]) * nextFraction;
            Canvas::line(point, chartY(a, bounds, top, chartHeight),
                         next, chartY(b, bounds, top, chartHeight), color);
        }
    }
}
inline void trend() {
    using namespace Configuration;
    using namespace Canvas;
    text(142, 5, "趋势");
    char label[16];
    snprintf(label, sizeof(label), "%um", Data::historyMinutes);
    text(270, 8, label, muted, FontAssets::small);
    rectangle(142, 34, 12, 2, sea);
    text(158, 27, "历史", muted, FontAssets::small);
    rectangle(207, 34, 12, 2, terracotta);
    text(223, 27, "延续", muted, FontAssets::small);
    rectangle(263, 34, 12, 2, peach);
    text(278, 27, "风扇", muted, FontAssets::small);
    constexpr int left = 164, top = 55, chartWidth = 138, chartHeight = 66;
    const int nowX = left + chartWidth * int(Data::historyMinutes) /
        (int(Data::historyMinutes) + int(predictionFarMinutes));
    const ChartBounds bounds = chartBounds();
    const unsigned start = historyCapacity - 1 - Data::historyMinutes;
    const auto& prediction = Prediction::result;
    rectangle(nowX + 1, top, left + chartWidth - nowX, chartHeight + 1, peachWash);
    for (int i = 0; i < 3; ++i) rectangle(left, top + i * chartHeight / 2, chartWidth, 1, divider);
    snprintf(label, sizeof(label), "%.0f", bounds.highest);
    text(left - 4 - textWidth(label, FontAssets::small), top - 7, label, muted, FontAssets::small);
    snprintf(label, sizeof(label), "%.0f", bounds.lowest);
    text(left - 4 - textWidth(label, FontAssets::small), top + chartHeight - 7, label, muted, FontAssets::small);
    for (unsigned minute = 1; minute <= Data::historyMinutes; ++minute) {
        const auto& before = Data::history[start + minute - 1];
        const auto& after = Data::history[start + minute];
        if (!before.valid || !after.valid || after.minute != before.minute + 1 ||
            !isfinite(before.concentration) || !isfinite(after.concentration)) continue;
        const int x = left + (nowX - left) * int(minute - 1) / int(Data::historyMinutes);
        const int nextX = left + (nowX - left) * int(minute) / int(Data::historyMinutes);
        line(x, chartY(before.concentration, bounds, top, chartHeight),
             nextX, chartY(after.concentration, bounds, top, chartHeight), sea);
    }
    for (int y = top; y <= top + chartHeight; y += 4) rectangle(nowX, y, 1, 2, muted);
    const auto& origin = Data::history[historyCapacity - 1];
    if (origin.valid && isfinite(origin.concentration)) {
        if (hasPrediction()) {
            dashedPrediction(nowX, left + chartWidth, top, chartHeight, bounds,
                origin.concentration, prediction.nearConcentration, prediction.farConcentration, terracotta);
            dashedPrediction(nowX, left + chartWidth, top, chartHeight, bounds,
                origin.concentration, prediction.fanNearConcentration, prediction.fanFarConcentration, peach);
        }
        circle(nowX, chartY(origin.concentration, bounds, top, chartHeight), 2, sea);
    }
    snprintf(label, sizeof(label), "-%um", Data::historyMinutes);
    text(148, 126, label, muted, FontAssets::small);
    snprintf(label, sizeof(label), "+%um", predictionFarMinutes);
    text(270, 126, label, muted, FontAssets::small);
    rectangle(142, 145, 162, 1, divider);
    snprintf(label, sizeof(label), "+%um %.0f", predictionFarMinutes, prediction.farConcentration);
    text(142, 147, hasPrediction() && isfinite(prediction.farConcentration) ? label : "延续 --", terracotta, FontAssets::small);
    snprintf(label, sizeof(label), "风扇 %.0f", prediction.fanFarConcentration);
    text(224, 147, hasPrediction() && isfinite(prediction.fanFarConcentration) ? label : "风扇 --", peach, FontAssets::small);
}
inline void render() {
    using namespace Configuration;
    using namespace Canvas;
    text(14, 7, "当前", muted, FontAssets::small);
    char value[24];
    if (Data::latest().valid) snprintf(value, sizeof(value), "%.0f", Data::latest().concentration);
    else snprintf(value, sizeof(value), "--");
    text(12, 32, value, ink, FontAssets::number);
    text(16, 72, "ppm", muted, FontAssets::small);
    Interface::pill(14, 93, 98, Data::live && !Data::latest().valid ? Data::sensorLabel : Prediction::statusLabel(), surface,
        Prediction::result.status == Prediction::Status::Success ? ink : terracotta);
    text(14, 123, "温度", muted, FontAssets::small);
    if (Data::latest().valid) snprintf(value, sizeof(value), "%.1f°C", Data::latest().temperature);
    else snprintf(value, sizeof(value), "--");
    text(55, 119, value);
    text(14, 146, "湿度", muted, FontAssets::small);
    if (Data::latest().valid) snprintf(value, sizeof(value), "%.0f%%", Data::latest().humidity);
    else snprintf(value, sizeof(value), "--");
    text(55, 142, value);
    rectangle(126, 8, 1, 144, divider);
    trend();
}
}
