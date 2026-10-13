#pragma once
#include "data.h"
#include "replay_assets.h"

// 回放连续公开实测，暂停和循环时保持历史完整。
namespace Replay {
inline unsigned index = 0;
inline uint32_t lastSample = 0;
inline bool paused = false;
inline Data::Sample sampleAt(unsigned position) {
    const auto& values = ReplayAssets::samples[position];
    return {values[0], values[1], values[2], position, true};
}
inline void restart(uint32_t now) {
    paused = false;
    Data::clear();
    index = Configuration::historyCapacity - 1;
    for (unsigned position = 0; position <= index; ++position) Data::append(sampleAt(position));
    lastSample = now;
}
inline void togglePause(uint32_t now) { paused = !paused; lastSample = now; }
inline bool tick(uint32_t now) {
    if (paused || now - lastSample < Configuration::replayMinuteDuration) return false;
    lastSample = now;
    if (++index >= ReplayAssets::count) restart(now);
    else Data::append(sampleAt(index));
    return true;
}
static_assert(ReplayAssets::count >= Configuration::historyCapacity, "Replay history is too short");
}
