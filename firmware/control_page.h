#pragma once
#include "widgets.h"

// 展示自动和手动档位预览。
namespace ControlPage {
inline void render() {
    using namespace Configuration;
    using namespace Canvas;
    Interface::pill(14, 12, 98, "风扇未连接", surface, muted);
    rounded(142, 9, 164, 36, 8, divider);
    rounded(Control::automatic ? 144 : 225, 11, 79, 32, 6, surface);
    centered(144, 17, 79, "自动", Control::automatic ? ink : muted);
    centered(225, 17, 79, "手动", !Control::automatic ? ink : muted);
    text(14, 62, "目标档位", muted, FontAssets::small);
    char value[24];
    snprintf(value, sizeof(value), "%u", Control::target());
    text(108, 87, value, ink, FontAssets::number);
    text(184, 108, "%", muted);
    text(14, 135, "档位仅预览", muted, FontAssets::small);
    for (unsigned index = 0; index < 4; ++index) {
        const int x = 14 + index * 75;
        const bool selected = !Control::automatic && Control::manualLevel == fanLevels[index];
        rounded(x, 157, 67, 30, 7, selected ? peachWash : surface);
        snprintf(value, sizeof(value), "%u%%", fanLevels[index]);
        centered(x, 162, 67, value, selected ? terracotta : ink);
    }
}
inline void tap(int x, int y) {
    if (y >= 9 && y < 45 && x >= 142 && x <= 306) Control::automatic = x < 224;
    if (y >= 157 && y < 187 && x >= 14 && x < 306 && (x - 14) % 75 < 67) Control::select((x - 14) / 75);
}
}
