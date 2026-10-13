#pragma once
#include "prediction.h"

// 计算风扇目标预览，当前不输出 GPIO 或 PWM。
namespace Control {
inline bool automatic = true;
inline uint8_t manualLevel = Configuration::fanLevels[1];
inline uint8_t target() {
    if (!automatic) return manualLevel;
    if (!Data::latest().valid) return 0;
    float concentration = Data::latest().concentration;
    if (Prediction::result.status == Prediction::Status::Success)
        concentration = fmaxf(concentration, Prediction::result.nearConcentration);
    if (concentration < Configuration::comfortableConcentration) return Configuration::fanLevels[0];
    if (concentration < Configuration::elevatedConcentration) return Configuration::fanLevels[1];
    if (concentration < Configuration::highConcentration) return Configuration::fanLevels[2];
    return Configuration::fanLevels[3];
}
inline void select(unsigned index) {
    if (index >= sizeof(Configuration::fanLevels) / sizeof(Configuration::fanLevels[0])) return;
    automatic = false;
    manualLevel = Configuration::fanLevels[index];
}
}
