#define AIR_UI_HOST
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <cmath>
using std::isfinite;

// 桌面验证适配硬件边界，直接执行固件的数据、触摸映射和页面代码。
constexpr int LOW = 0, HIGH = 1, OUTPUT = 1, INPUT = 0, INPUT_PULLUP = 2;
void pinMode(int, int) {}
void digitalWrite(int, int) {}
bool touchPressed = false;
std::deque<int> touchBits;
int digitalRead(int pin) {
    if (pin == 4) return touchPressed ? LOW : HIGH;
    assert(pin == 13 && !touchBits.empty());
    const int value = touchBits.front(); touchBits.pop_front(); return value;
}
void queueCoordinate(uint16_t value) {
    for (int bit = 23; bit >= 0; --bit) touchBits.push_back(((uint32_t(value) << 3) >> bit) & 1);
}
void delayMicroseconds(unsigned) {}
struct SerialAdapter {
    void println(const char* value) { std::puts(value); }
    void printf(const char* format, ...) {
        va_list values; va_start(values, format); std::vprintf(format, values); va_end(values);
    }
} Serial;
std::vector<unsigned char> storage;
struct Preferences {
    bool begin(const char*, bool) { return true; }
    void end() {}
    size_t getBytesLength(const char*) { return storage.size(); }
    size_t getBytes(const char*, void* destination, size_t length) {
        if (length > storage.size()) return 0;
        std::memcpy(destination, storage.data(), length); return length;
    }
    size_t putBytes(const char*, const void* source, size_t length) {
        const auto* bytes = static_cast<const unsigned char*>(source);
        storage.assign(bytes, bytes + length); return length;
    }
};
namespace Display { void writeRegion(int, int, int, int, const uint16_t*) {} }

#include "../firmware/interface.h"
#include "../firmware/touch.h"

void snapshot(const char* directory, const char* name) {
    Interface::render();
    const std::string path = std::string(directory) + "/" + name + ".ppm";
    FILE* file = nullptr;
    assert(fopen_s(&file, path.c_str(), "wb") == 0);
    std::fprintf(file, "P6\n%u %u\n255\n", Configuration::width, Configuration::height);
    for (const uint16_t pixel : Canvas::pixels) {
        const unsigned char bytes[] = {
            static_cast<unsigned char>(((pixel >> 11) & 31) * 255 / 31),
            static_cast<unsigned char>(((pixel >> 5) & 63) * 255 / 63),
            static_cast<unsigned char>((pixel & 31) * 255 / 31)};
        std::fwrite(bytes, 1, sizeof(bytes), file);
    }
    std::fclose(file);
}

int main(int count, char** arguments) {
    assert(count == 2);
    Prediction::Result simulated;
    simulated.status = Prediction::Status::Success;
    simulated.nearConcentration = 1000;
    simulated.farConcentration = 1200;
    Prediction::applyFanSimulation(simulated);
    assert(std::fabs(simulated.fanNearConcentration - 895.6f) < 0.01f);
    assert(std::fabs(simulated.fanFarConcentration - 927.0f) < 0.01f);
    simulated.nearConcentration = 400;
    Prediction::applyFanSimulation(simulated);
    assert(simulated.fanNearConcentration == 400);
    Replay::restart(0);
    Prediction::begin([](const float* values, bool valid) -> Prediction::Result {
        assert(valid && values[19] == Data::latest().concentration);
        const auto& reference = ReplayAssets::reference[Data::latest().minute];
        Prediction::Result result;
        result.status = static_cast<Prediction::Status>(reference.status);
        result.nearConcentration = reference.nearConcentration;
        result.farConcentration = reference.farConcentration;
        Prediction::applyFanSimulation(result);
        result.durationMicroseconds = 219;
        return result;
    });
    Prediction::update();
    assert(Prediction::result.status == Prediction::Status::Success);
    assert(Data::latest().concentration == ReplayAssets::samples[Replay::index][0]);
    const auto retained = Data::history[10];
    Data::history[Configuration::historyCapacity - 1].valid = false;
    Prediction::update();
    assert(Prediction::result.status == Prediction::Status::InvalidHistory && !isfinite(Prediction::result.nearConcentration));
    assert(Data::history[10].concentration == retained.concentration);
    Replay::restart(0);
    Data::history[Configuration::historyCapacity - 3].minute += 1;
    Prediction::update();
    assert(Prediction::result.status == Prediction::Status::InvalidHistory);
    Replay::restart(0);
    assert(!Replay::tick(Configuration::replayMinuteDuration - 1));
    assert(Replay::tick(Configuration::replayMinuteDuration));
    Replay::togglePause(Configuration::replayMinuteDuration);
    const unsigned pausedIndex = Replay::index;
    assert(!Replay::tick(Configuration::replayMinuteDuration * 10) && Replay::index == pausedIndex);
    Replay::togglePause(Configuration::replayMinuteDuration * 10);
    Replay::index = ReplayAssets::count - 1;
    assert(Replay::tick(Configuration::replayMinuteDuration * 11));
    Prediction::update();
    assert(Replay::index == Configuration::historyCapacity - 1 && Prediction::result.status == Prediction::Status::Success);
    Control::automatic = false; Control::manualLevel = 66;
    assert(Control::target() == 66);
    Interface::page = Interface::Page::Home;
    Interface::tap(180, 100, 0);
    assert(Data::historyMinutes == 60);
    Interface::tap(180, 100, 0);
    assert(Data::historyMinutes == 5);
    Interface::tap(180, 100, 0);
    assert(Data::historyMinutes == 15);
    Interface::tap(180, 100, 0);
    assert(Data::historyMinutes == 30);
    Interface::tap(150, 220, 0);
    assert(Interface::page == Interface::Page::Control);
    Interface::tap(270, 170, 0);
    assert(!Control::automatic && Control::manualLevel == 100);
    Interface::tap(180, 25, 0);
    assert(Control::automatic);
    Interface::tap(280, 220, 0);
    assert(Interface::page == Interface::Page::Settings);
    Interface::tap(100, 60, 0);
    assert(Replay::paused);
    Interface::tap(100, 60, 0);
    assert(!Replay::paused);
    assert(Interface::tap(200, 85, 0) == Interface::Action::RestartReplay);
    Interface::tap(50, 147, 0);
    assert(Interface::page == Interface::Page::Recording);
    assert(Interface::tap(240, 20, 0) == Interface::Action::InitializeRecording);
    Recording::mounted = true; Recording::status = Recording::Status::Stopped;
    assert(Interface::tap(240, 20, 0) == Interface::Action::StartRecording);
    Recording::status = Recording::Status::Active;
    assert(Interface::tap(240, 20, 0) == Interface::Action::StopRecording);
    Recording::status = Recording::Status::WriteFailed;
    assert(Interface::tap(240, 20, 0) == Interface::Action::None);
    Recording::status = Recording::Status::ReadFailed;
    assert(Interface::tap(240, 20, 0) == Interface::Action::None);
    Recording::status = Recording::Status::Active;
    Interface::tap(280, 220, 0);
    assert(Interface::tap(265, 147, 0) == Interface::Action::TogglePrediction);
    Interface::tap(150, 147, 0);
    assert(Interface::page == Interface::Page::Debug);
    Interface::tap(150, 20, 0);
    assert(DebugPage::section == 1);
    Interface::tap(260, 188, 0);
    assert(Interface::debugLogs);
    Interface::tap(260, 20, 0);
    assert(DebugPage::section == 2);
    Interface::tap(50, 20, 0);
    assert(DebugPage::section == 0);

    // 无有效 NVS 时进入四角校准；完成计算后继续使用触摸。
    Touch::begin();
    assert(!Touch::saved && Touch::active && storage.empty());
    Touch::calibrationRaw[0][0] = 500; Touch::calibrationRaw[0][1] = 500;
    Touch::calibrationRaw[1][0] = 3500; Touch::calibrationRaw[1][1] = 500;
    Touch::calibrationRaw[2][0] = 3500; Touch::calibrationRaw[2][1] = 3500;
    Touch::calibrationRaw[3][0] = 500; Touch::calibrationRaw[3][1] = 3500;
    assert(Touch::calculate() == nullptr);
    Touch::active = false; Touch::waitingForRelease = true; Touch::tapDelivered = false;
    int touchX = -1, touchY = -1;
    assert(!Touch::poll(touchX, touchY, 1));
    assert(!Touch::poll(touchX, touchY, 202));
    touchPressed = true;
    queueCoordinate(2000); queueCoordinate(2000);
    assert(Touch::poll(touchX, touchY, 250));
    assert(touchX == 160 && touchY == 120 && touchBits.empty());
    assert(!Touch::poll(touchX, touchY, 300));
    touchPressed = false;
    assert(!Touch::poll(touchX, touchY, 350));
    assert(!Touch::poll(touchX, touchY, 551));
    touchPressed = true;
    queueCoordinate(2000); queueCoordinate(2000);
    assert(Touch::poll(touchX, touchY, 600));
    touchPressed = false;

    // 兼容旧版 NVS 仿射矩阵，缺失、损坏和方向不匹配不会写回或阻塞界面。
    Touch::CalibrationRecord record{};
    record.magic = Configuration::calibrationMagic; record.version = Configuration::calibrationVersion;
    record.width = Configuration::width; record.height = Configuration::height;
    record.orientation = Configuration::displayOrientation;
    record.matrix[0][0] = 0.08f; record.matrix[0][2] = -4.0f;
    record.matrix[1][1] = -0.06f; record.matrix[1][2] = 235.0f;
    record.checksum = Touch::checksum(record);
    Preferences preferences;
    preferences.putBytes("matrix", &record, sizeof(record));
    const auto originalStorage = storage;
    Touch::begin();
    assert(Touch::saved && storage == originalStorage);
    assert(Touch::mapCoordinates(2000, 2000, touchX, touchY) && touchX == 156 && touchY == 115);
    storage[0] ^= 1;
    const auto corruptStorage = storage;
    Touch::begin();
    assert(!Touch::saved);
    record.orientation ^= 1; record.checksum = Touch::checksum(record);
    preferences.putBytes("matrix", &record, sizeof(record));
    Touch::begin();
    assert(!Touch::saved);
    record.orientation = Configuration::displayOrientation;
    record.matrix[0][0] = NAN; record.checksum = Touch::checksum(record);
    assert(!Touch::validRecord(record));

    Interface::freeMemory = 126 * 1024; Interface::framesPerSecond = 1.9f;
    Interface::debugLogs = false; Data::historyMinutes = 30;
    Interface::page = Interface::Page::Home; snapshot(arguments[1], "home");
    Interface::page = Interface::Page::Control; snapshot(arguments[1], "control");
    Interface::page = Interface::Page::Settings; snapshot(arguments[1], "settings");
    Interface::page = Interface::Page::Debug; snapshot(arguments[1], "debug");
    Recording::totalBytes = 0xD60000; Recording::remainingBytes = Recording::totalBytes - 8192;
    Recording::totalRecords = Recording::sessionRecords = 61; Recording::sessionId = 1;
    Interface::page = Interface::Page::Recording; snapshot(arguments[1], "recording");
    Recording::mounted = false; Recording::status = Recording::Status::Unavailable;
    Recording::totalRecords = Recording::sessionRecords = Recording::sessionId = 0;
    Recording::totalBytes = Recording::remainingBytes = 0;
    snapshot(arguments[1], "recording_initial");
    Data::history[Configuration::historyCapacity - 1].valid = false; Prediction::update();
    Interface::page = Interface::Page::Home; snapshot(arguments[1], "disconnected");
    Replay::restart(0); Prediction::update();
    Data::historyMinutes = 60; snapshot(arguments[1], "rising");
    Data::live = true;
    Data::historyMinutes = 30;
    Data::clear();
    for (unsigned minute = 0; minute < Configuration::historyCapacity; ++minute)
        Data::append({620.0f + minute * 3.55f, 20.8f, 21.0f, minute, true, 12, 0, Data::MinuteFailure::None});
    Data::current = {900.0f, 20.8f, 21.0f, 61, true};
    Data::sensorLabel = "采样正常";
    Prediction::begin([](const float* values, bool valid) -> Prediction::Result {
        assert(valid && values[19] == Data::history[Configuration::historyCapacity - 1].concentration);
        assert(values[19] != Data::current.concentration);
        Prediction::Result result;
        result.status = Prediction::Status::Success;
        result.nearConcentration = 875; result.farConcentration = 960;
        result.durationMicroseconds = 219;
        Prediction::applyFanSimulation(result);
        return result;
    });
    Prediction::update();
    assert(Interface::predictionColor() == Configuration::predictionReadyColor);
    auto& diagnostic = Interface::diagnostics;
    diagnostic.currentMinute = 61; diagnostic.currentMinuteSamples = 9;
    diagnostic.continuousValidMinutes = 61; diagnostic.hasValidReading = true;
    diagnostic.sensorStatus = "采样正常"; diagnostic.measurementAgeSeconds = 2;
    diagnostic.tensorArenaUsedBytes = 2096; diagnostic.tensorArenaCapacityBytes = 16384;
    diagnostic.modelHash = ModelAssets::modelSha256;
    diagnostic.storageMounted = true; diagnostic.recordingStatus = "记录中";
    diagnostic.storageTotalBytes = 0xD60000; diagnostic.storageRemainingBytes = 13000000;
    diagnostic.recordingSessions = 6; diagnostic.uptimeMilliseconds = 86499000;
    Interface::calibrationSaved = true;
    Interface::page = Interface::Page::Home; snapshot(arguments[1], "home_live");
    const auto bounds = HomePage::chartBounds();
    constexpr int nowX = 164 + 138 * 30 / 45;
    const int originY = HomePage::chartY(Data::history[60].concentration, bounds, 55, 66);
    assert(Canvas::pixels[originY * Configuration::width + nowX] == Configuration::sea);
    for (unsigned minutes : {5u, 15u, 30u, 60u}) {
        Data::historyMinutes = minutes;
        char name[32]; snprintf(name, sizeof(name), "home_live_%um", minutes);
        snapshot(arguments[1], name);
    }
    Data::historyMinutes = 30;
    Interface::page = Interface::Page::Settings; snapshot(arguments[1], "settings_live");
    Interface::page = Interface::Page::Debug;
    for (unsigned section = 0; section < 3; ++section) {
        DebugPage::section = section;
        char name[32]; snprintf(name, sizeof(name), "debug_%u", section + 1);
        snapshot(arguments[1], name);
    }
    Data::history[50].valid = false;
    Data::history[50].failureReason = Data::MinuteFailure::CrcFailed;
    Data::history[50].errorCount = 1;
    Prediction::update();
    diagnostic.continuousValidMinutes = 10; diagnostic.hasBadMinute = true;
    diagnostic.firstBadMinute = 50; diagnostic.firstBadOffset = 10;
    diagnostic.firstBadReason = "校验失败"; diagnostic.firstBadSamples = 12; diagnostic.firstBadErrors = 1;
    assert(Interface::predictionColor() == Configuration::predictionWaitingColor);
    Interface::page = Interface::Page::Home; snapshot(arguments[1], "home_gap");
    Interface::page = Interface::Page::Debug; DebugPage::section = 0; snapshot(arguments[1], "debug_gap");
    Prediction::setEnabled(false); Prediction::update();
    assert(Prediction::result.status == Prediction::Status::Disabled && !HomePage::hasPrediction());
    assert(Interface::predictionColor() == Configuration::predictionDisabledColor);
    Interface::page = Interface::Page::Home; snapshot(arguments[1], "home_disabled");
    Interface::page = Interface::Page::Settings; snapshot(arguments[1], "settings_disabled");
    Prediction::setEnabled(true);
    Data::current.valid = false; Prediction::update();
    assert(Interface::predictionColor() == Configuration::predictionFaultColor);
    Interface::page = Interface::Page::Home; snapshot(arguments[1], "home_sensor_fault");
    Data::current.valid = true; Data::current.concentration = 10000; Data::current.temperature = -40; Data::current.humidity = 100;
    Prediction::result.status = Prediction::Status::OutsideCalibration;
    assert(Interface::predictionColor() == Configuration::predictionWaitingColor);
    snapshot(arguments[1], "home_high");
    char concentration[16]; snprintf(concentration, sizeof(concentration), "%.0f", Data::current.concentration);
    assert(Canvas::textWidth(concentration, FontAssets::number) + 12 < 126);
    assert(Canvas::textWidth("100%", FontAssets::body) + 55 < 126);
    assert(Canvas::textWidth("-40.0°C", FontAssets::body) + 55 < 126);
    for (unsigned index = 0; index < Configuration::historyCapacity; ++index)
        Data::history[index].concentration = 9000.0f + index * 16.5f;
    const auto highBounds = HomePage::chartBounds();
    assert(highBounds.highest > 10000 && highBounds.lowest < 9000);
    char maximum[16]; snprintf(maximum, sizeof(maximum), "%.0f", highBounds.highest);
    assert(Canvas::textWidth(maximum, FontAssets::small) <= 35);
    snapshot(arguments[1], "home_high_axis");
    for (auto& sample : Data::history) sample.concentration = 350.0f;
    const auto lowBounds = HomePage::chartBounds();
    assert(lowBounds.lowest < 350 && HomePage::chartY(350, lowBounds, 55, 66) < 121);
    snapshot(arguments[1], "home_low");
    Prediction::result.status = Prediction::Status::InferenceFailed;
    assert(Interface::predictionColor() == Configuration::predictionFaultColor);
    snapshot(arguments[1], "home_inference_fault");
    Interface::page = Interface::Page::Recording;
    Recording::mounted = true; Recording::status = Recording::Status::WriteFailed;
    snapshot(arguments[1], "recording_write_failed");
    std::puts("PASS: recorded replay, gaps, model output propagation, pause/restart/wrap, navigation, recording actions, control preview, saved/default touch mapping and tap delivery; rendered firmware pages");
}
