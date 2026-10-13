#pragma once
#include "widgets.h"
#include "recording.h"

// 将持久记录操作和导出说明集中到独立页面。
namespace RecordingPage {
inline bool needsRestart() {
    return Recording::status == Recording::Status::WriteFailed || Recording::status == Recording::Status::ReadFailed;
}
inline void render() {
    using namespace Configuration;
    using namespace Canvas;
    text(14, 11, Recording::statusLabel(), ink);
    Interface::pill(198, 8, 108, needsRestart() ? "请重启" : !Recording::mounted ? "初始化存储" :
        Recording::active() ? "停止记录" : "开始记录", surface, needsRestart() ? muted : ink);
    for (unsigned index = 0; index < environmentCount; ++index) {
        const bool selected = Recording::selectedEnvironment == index;
        Interface::pill(14 + index * 74, 48, 70, environmentLabels[index],
            selected ? peachWash : surface, selected ? ink : muted);
    }
    text(14, 85, Recording::active() ? "记录中，停止后可切换环境" : "先选环境，再开始记录", muted, FontAssets::small);
    char value[48];
    snprintf(value, sizeof(value), "本场 %lu 条 / 剩余 %lu KB", static_cast<unsigned long>(Recording::sessionRecords),
        static_cast<unsigned long>(Recording::remainingBytes / 1024));
    text(14, 112, value, ink, FontAssets::small);
    text(14, 137, Clock::valid ? "已校时 · 记录真实时间" : "未校时 · 记录相对时间", muted, FontAssets::small);
    snprintf(value, sizeof(value), "异常 %lu / 读数距今 %lu 秒", static_cast<unsigned long>(Recording::faultEvents),
        static_cast<unsigned long>(Recording::measurementAgeSeconds));
    text(14, 162, value, muted, FontAssets::small);
    text(14, 185, needsRestart() ? "记录异常，重启后检查并恢复" :
        Recording::mounted ? "USB 导出后自动继续记录" : "首次初始化会清空存储", muted, FontAssets::small);
}
inline Interface::Action tap(int x, int y) {
    if (x >= 14 && x < 310 && y >= 43 && y < 78 && (x - 14) % 74 < 70) {
        Recording::selectEnvironment(unsigned(x - 14) / 74);
        return Interface::Action::None;
    }
    if (x < 190 || x >= 310 || y < 4 || y >= 39) return Interface::Action::None;
    if (needsRestart()) return Interface::Action::None;
    return !Recording::mounted ? Interface::Action::InitializeRecording : Recording::active() ?
        Interface::Action::StopRecording : Interface::Action::StartRecording;
}
}
