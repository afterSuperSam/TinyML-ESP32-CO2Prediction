#pragma once
#include "widgets.h"

// 诊断数据由应用层快照提供，页面只管理三组信息和日志开关。
namespace DebugPage {
inline unsigned section = 0;
inline void row(int y, const char* label, const char* value, uint16_t color = Configuration::ink) {
    Canvas::text(14, y, label, Configuration::muted, FontAssets::small);
    Canvas::text(306 - Canvas::textWidth(value, FontAssets::small), y, value, color, FontAssets::small);
}
inline void render() {
    using namespace Configuration;
    using namespace Canvas;
    const char* tabs[] = {"采样历史", "模型性能", "存储运行"};
    for (unsigned index = 0; index < 3; ++index) {
        const int x = 8 + int(index) * 103;
        rounded(x, 6, 98, 32, 6, section == index ? peachWash : surface);
        centered(x, 12, 98, tabs[index], section == index ? ink : muted, FontAssets::small);
    }
    const auto& state = Interface::diagnostics;
    char value[96];
    if (section == 0) {
        row(44, "采样", Data::live ? state.sensorStatus : "回放", Interface::predictionColor());
        snprintf(value, sizeof(value), "%lu / %u 分", (unsigned long)state.continuousValidMinutes, predictionHistoryMinutes);
        row(64, "连续有效", value);
        snprintf(value, sizeof(value), "%lu 条 / 错误 %lu", (unsigned long)state.currentMinuteSamples, (unsigned long)state.currentMinuteErrors);
        row(84, "当前分钟", value);
        snprintf(value, sizeof(value), "%lu / %lu 次", (unsigned long)state.sensorErrors, (unsigned long)state.recoveryAttempts);
        row(104, "累计错误 / 恢复", value);
        if (state.hasValidReading) snprintf(value, sizeof(value), "%lu 秒", (unsigned long)state.measurementAgeSeconds);
        else snprintf(value, sizeof(value), "无有效读数");
        row(124, "读数距今", value);
        if (state.hasBadMinute) snprintf(value, sizeof(value), "%lu 分 / 窗口 %u", (unsigned long)state.firstBadMinute, state.firstBadOffset);
        else snprintf(value, sizeof(value), "无");
        row(144, "首个坏分钟", value);
        row(164, "原因", state.firstBadReason, state.hasBadMinute ? terracotta : ink);
        if (state.hasBadMinute) snprintf(value, sizeof(value), "%lu 条 / 错误 %lu", (unsigned long)state.firstBadSamples, (unsigned long)state.firstBadErrors);
        else snprintf(value, sizeof(value), "--");
        row(184, "坏分钟样本", value);
    } else if (section == 1) {
        row(44, "预测", Prediction::statusLabel(), Interface::predictionColor());
        snprintf(value, sizeof(value), "%lu us", (unsigned long)Prediction::result.durationMicroseconds);
        row(64, "推理耗时", value);
        snprintf(value, sizeof(value), "%lu KB / %.1f fps", (unsigned long)(Interface::freeMemory / 1024), Interface::framesPerSecond);
        row(84, "空闲内存 / 重绘", value);
        snprintf(value, sizeof(value), "%lu / %lu B", (unsigned long)state.tensorArenaUsedBytes, (unsigned long)state.tensorArenaCapacityBytes);
        row(104, "张量用量", value);
        text(14, 125, "模型 SHA256", muted, FontAssets::small);
        snprintf(value, sizeof(value), "%.32s", state.modelHash);
        text(14, 143, value, ink, FontAssets::small);
        snprintf(value, sizeof(value), "%.32s", strlen(state.modelHash) > 32 ? state.modelHash + 32 : "");
        text(14, 158, value, ink, FontAssets::small);
        Interface::pill(210, 178, 96, Interface::debugLogs ? "日志 开" : "日志 关", surface, ink);
    } else {
        row(44, "存储", state.recordingStatus, state.storageMounted ? ink : terracotta);
        snprintf(value, sizeof(value), "%.2f / %.2f MB", state.storageRemainingBytes / 1048576.0f, state.storageTotalBytes / 1048576.0f);
        row(64, "剩余 / 总量", value);
        snprintf(value, sizeof(value), "%lu 场", (unsigned long)state.recordingSessions);
        row(84, "已保存场次", value);
        const uint64_t seconds = state.uptimeMilliseconds / 1000;
        snprintf(value, sizeof(value), "%llu:%02u:%02u", (unsigned long long)(seconds / 3600), unsigned(seconds / 60 % 60), unsigned(seconds % 60));
        row(104, "运行时长", value);
        row(124, "最近错误", state.lastSensorError, state.hasSensorError ? terracotta : ink);
        if (state.hasSensorError) snprintf(value, sizeof(value), "%lu 秒", (unsigned long)state.lastErrorAgeSeconds);
        else snprintf(value, sizeof(value), "无事件");
        row(144, "错误距今", value);
        if (state.hasRecovery) snprintf(value, sizeof(value), "%lu 秒", (unsigned long)state.lastRecoveryAgeSeconds);
        else snprintf(value, sizeof(value), "无事件");
        row(164, "恢复距今", value);
        row(184, "触摸校准", Interface::calibrationSaved ? "已保存" : "未保存");
    }
}
inline void tap(int x, int y) {
    if (y >= 6 && y < 38 && x >= 8 && x < 312) {
        const unsigned index = unsigned(x - 8) / 103;
        if (index < 3) section = index;
    } else if (section == 1 && x >= 200 && y >= 174 && y < 204)
        Interface::debugLogs = !Interface::debugLogs;
}
}
