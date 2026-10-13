#pragma once
#include "widgets.h"

// 提供采样、校准、记录、诊断与预测开关入口。
namespace SettingsPage {
constexpr int tileWidth = 94, tileHeight = 72, tileGap = 8, firstX = 14;
constexpr int firstRowY = 30, secondRowY = 112;
inline int tileX(unsigned column) { return firstX + int(column) * (tileWidth + tileGap); }
inline bool hitTile(int x, int y, unsigned column, unsigned row) {
    const int left = tileX(column), top = row == 0 ? firstRowY : secondRowY;
    return x >= left && x < left + tileWidth && y >= top && y < top + tileHeight;
}
inline void render() {
    char value[32];
    snprintf(value, sizeof(value), "固件 v%s", Configuration::version);
    Canvas::text(14, 6, value, Configuration::muted, FontAssets::small);
    Interface::settingTile(tileX(0), firstRowY, tileWidth, tileHeight,
        Data::live ? "SCD41" : "回放", Data::live ? Data::sensorLabel : (Replay::paused ? "点击继续" : "点击暂停"));
    Interface::settingTile(tileX(1), firstRowY, tileWidth, tileHeight,
        Data::live ? "传感器" : "回放", Data::live ? "恢复连接" : "从头开始");
    Interface::settingTile(tileX(2), firstRowY, tileWidth, tileHeight,
        "触摸校准", Interface::calibrationSaved ? "已保存" : "需要校准", !Interface::calibrationSaved);
    Interface::settingTile(tileX(0), secondRowY, tileWidth, tileHeight, "数据记录", "打开记录页");
    Interface::settingTile(tileX(1), secondRowY, tileWidth, tileHeight, "调试", "查看运行状态");
    Interface::settingTile(tileX(2), secondRowY, tileWidth, tileHeight,
        "TinyML", Prediction::enabled ? "已开启 · 关闭" : "已关闭 · 开启", Prediction::enabled);
    if (!Interface::diagnostics.predictionSettingSaved)
        Canvas::text(14, 186, Interface::diagnostics.predictionSettingError, Configuration::terracotta, FontAssets::small);
}
inline Interface::Action tap(int x, int y, uint32_t now) {
    if (hitTile(x, y, 0, 0)) { if (!Data::live) Replay::togglePause(now); return Interface::Action::None; }
    if (hitTile(x, y, 1, 0)) return Interface::Action::RestartReplay;
    if (hitTile(x, y, 2, 0)) return Interface::Action::Calibrate;
    if (hitTile(x, y, 0, 1)) { Interface::page = Interface::Page::Recording; return Interface::Action::None; }
    if (hitTile(x, y, 1, 1)) { Interface::page = Interface::Page::Debug; return Interface::Action::None; }
    if (hitTile(x, y, 2, 1)) return Interface::Action::TogglePrediction;
    return Interface::Action::None;
}
}

