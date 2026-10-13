#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "config.h"

// 本模块只负责 ILI9341 初始化与像素传输。
namespace Display {
inline void command(uint8_t value, const uint8_t* data = nullptr, size_t count = 0) {
    SPI.beginTransaction(SPISettings(Configuration::displayFrequency, MSBFIRST, SPI_MODE0));
    digitalWrite(Configuration::displaySelect, LOW);
    digitalWrite(Configuration::displayCommand, LOW);
    SPI.transfer(value);
    digitalWrite(Configuration::displayCommand, HIGH);
    if (count) SPI.writeBytes(data, count);
    digitalWrite(Configuration::displaySelect, HIGH);
    SPI.endTransaction();
}
inline void begin() {
    using namespace Configuration;
    pinMode(displayCommand, OUTPUT);
    pinMode(displayReset, OUTPUT);
    pinMode(displaySelect, OUTPUT);
    digitalWrite(displaySelect, HIGH);
    SPI.begin(displayClock, -1, displayData, displaySelect);
    digitalWrite(displayReset, HIGH);
    delay(10);
    digitalWrite(displayReset, LOW);
    delay(20);
    digitalWrite(displayReset, HIGH);
    delay(150);
    command(0x01);
    delay(150);
    const uint8_t powerOne[] = {0x23};
    const uint8_t powerTwo[] = {0x10};
    const uint8_t commonVoltage[] = {0x3E, 0x28};
    const uint8_t commonVoltageTwo[] = {0x86};
    const uint8_t orientation[] = {displayOrientation};
    const uint8_t pixelFormat[] = {0x55};
    const uint8_t frameRate[] = {0x00, 0x18};
    const uint8_t displayFunction[] = {0x08, 0x82, 0x27};
    command(0xC0, powerOne, sizeof(powerOne));
    command(0xC1, powerTwo, sizeof(powerTwo));
    command(0xC5, commonVoltage, sizeof(commonVoltage));
    command(0xC7, commonVoltageTwo, sizeof(commonVoltageTwo));
    command(0x36, orientation, sizeof(orientation));
    command(0x3A, pixelFormat, sizeof(pixelFormat));
    command(0xB1, frameRate, sizeof(frameRate));
    command(0xB6, displayFunction, sizeof(displayFunction));
    command(0x11);
    delay(120);
    command(0x29);
    delay(20);
}
inline void writeRegion(int x, int y, int regionWidth, int regionHeight, const uint16_t* pixels) {
    using namespace Configuration;
    const int right = x + regionWidth - 1;
    const int bottom = y + regionHeight - 1;
    const uint8_t columns[] = {uint8_t(x >> 8), uint8_t(x), uint8_t(right >> 8), uint8_t(right)};
    const uint8_t rows[] = {uint8_t(y >> 8), uint8_t(y), uint8_t(bottom >> 8), uint8_t(bottom)};
    command(0x2A, columns, sizeof(columns));
    command(0x2B, rows, sizeof(rows));
    SPI.beginTransaction(SPISettings(displayFrequency, MSBFIRST, SPI_MODE0));
    digitalWrite(displaySelect, LOW);
    digitalWrite(displayCommand, LOW);
    SPI.transfer(0x2C);
    digitalWrite(displayCommand, HIGH);
    for (int row = 0; row < regionHeight; ++row) {
        SPI.writePixels(pixels + (y + row) * width + x, regionWidth * sizeof(uint16_t));
    }
    digitalWrite(displaySelect, HIGH);
    SPI.endTransaction();
}
}
