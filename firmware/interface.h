#pragma once
#include "home_page.h"
#include "control_page.h"
#include "settings_page.h"
#include "debug_page.h"
#include "recording_page.h"

// 页面路由只分发渲染和触摸，不初始化或调用模型。
namespace Interface {
inline void render() {
    Canvas::clear(Configuration::background);
    switch (page) {
        case Page::Home: HomePage::render(); break;
        case Page::Control: ControlPage::render(); break;
        case Page::Settings: SettingsPage::render(); break;
        case Page::Debug: DebugPage::render(); break;
        case Page::Recording: RecordingPage::render(); break;
    }
    navigation();
}
inline Action tap(int physicalX, int physicalY, uint32_t now) {
    const int x = Canvas::logicalX(physicalX), y = Canvas::logicalY(physicalY);
    if (x < 0 || x >= 320 || y < 0 || y >= 240) return Action::None;
    if (y >= 204) page = x < 106 ? Page::Home : x < 212 ? Page::Control : Page::Settings;
    else if (page == Page::Home && x >= 134 && y >= 0 && y < 164) {
        // 趋势图列共用大触摸区域。
        switch (Data::historyMinutes) {
            case 5: Data::historyMinutes = 15; break;
            case 15: Data::historyMinutes = 30; break;
            case 30: Data::historyMinutes = 60; break;
            default: Data::historyMinutes = 5; break;
        }
    }
    else if (page == Page::Control) ControlPage::tap(x, y);
    else if (page == Page::Settings) return SettingsPage::tap(x, y, now);
    else if (page == Page::Debug) DebugPage::tap(x, y);
    else if (page == Page::Recording) return RecordingPage::tap(x, y);
    return Action::None;
}
}
