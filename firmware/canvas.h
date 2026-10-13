#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "config.h"
#include "font_assets.h"
#ifndef AIR_UI_HOST
#include "display.h"
#endif

// 逻辑画布负责字体、基础形状和分块刷新，界面统一使用 320×240 坐标。
namespace Canvas {
inline uint16_t pixels[unsigned(Configuration::width) * Configuration::height];
constexpr unsigned columns = (Configuration::width + Configuration::refreshTileSize - 1) / Configuration::refreshTileSize;
constexpr unsigned rows = (Configuration::height + Configuration::refreshTileSize - 1) / Configuration::refreshTileSize;
inline uint32_t hashes[columns * rows];
inline bool firstFrame = true;
inline float scale() { return fminf(Configuration::width / 320.0f, Configuration::height / 240.0f); }
inline int offsetX() { return (Configuration::width - lroundf(320 * scale())) / 2; }
inline int offsetY() { return (Configuration::height - lroundf(240 * scale())) / 2; }
inline int logicalX(int x) { return lroundf((x - offsetX()) / scale()); }
inline int logicalY(int y) { return lroundf((y - offsetY()) / scale()); }
inline uint16_t blend(uint16_t foreground, uint16_t background, uint8_t alpha) {
    const unsigned inverse = 255 - alpha;
    const unsigned red = (((foreground >> 11) & 31) * alpha + ((background >> 11) & 31) * inverse + 127) / 255;
    const unsigned green = (((foreground >> 5) & 63) * alpha + ((background >> 5) & 63) * inverse + 127) / 255;
    const unsigned blue = ((foreground & 31) * alpha + (background & 31) * inverse + 127) / 255;
    return (red << 11) | (green << 5) | blue;
}
inline void rawRectangle(int x, int y, int width, int height, uint16_t color, uint8_t alpha = 255) {
    const int left = x < 0 ? 0 : x;
    const int top = y < 0 ? 0 : y;
    const int right = x + width > Configuration::width ? Configuration::width : x + width;
    const int bottom = y + height > Configuration::height ? Configuration::height : y + height;
    for (int row = top; row < bottom; ++row) {
        for (int column = left; column < right; ++column) {
            uint16_t& pixel = pixels[row * Configuration::width + column];
            pixel = alpha == 255 ? color : blend(color, pixel, alpha);
        }
    }
}
inline void clear(uint16_t color) {
    for (uint16_t& pixel : pixels) pixel = color;
}
inline void rectangle(int x, int y, int width, int height, uint16_t color, uint8_t alpha = 255) {
    const int left = lroundf(x * scale()) + offsetX();
    const int top = lroundf(y * scale()) + offsetY();
    const int right = lroundf((x + width) * scale()) + offsetX();
    const int bottom = lroundf((y + height) * scale()) + offsetY();
    rawRectangle(left, top, right - left, bottom - top, color, alpha);
}
inline void line(int x, int y, int endX, int endY, uint16_t color) {
    const int horizontal = abs(endX - x);
    const int vertical = -abs(endY - y);
    const int stepX = x < endX ? 1 : -1;
    const int stepY = y < endY ? 1 : -1;
    int error = horizontal + vertical;
    while (true) {
        rectangle(x, y, 1, 1, color);
        if (x == endX && y == endY) break;
        const int twice = 2 * error;
        if (twice >= vertical) { error += vertical; x += stepX; }
        if (twice <= horizontal) { error += horizontal; y += stepY; }
    }
}
inline void circle(int x, int y, int radius, uint16_t color) {
    for (int row = -radius; row <= radius; ++row) {
        const int half = int(sqrtf(float(radius * radius - row * row)));
        rectangle(x - half, y + row, half * 2 + 1, 1, color);
    }
}
inline void rounded(int x, int y, int width, int height, int radius, uint16_t color) {
    rectangle(x + radius, y, width - radius * 2, height, color);
    rectangle(x, y + radius, width, height - radius * 2, color);
    circle(x + radius, y + radius, radius, color);
    circle(x + width - radius - 1, y + radius, radius, color);
    circle(x + radius, y + height - radius - 1, radius, color);
    circle(x + width - radius - 1, y + height - radius - 1, radius, color);
}
inline uint32_t nextCodepoint(const char*& text) {
    const uint8_t first = uint8_t(*text++);
    if (first < 0x80) return first;
    const unsigned extra = first < 0xE0 ? 1 : first < 0xF0 ? 2 : 3;
    uint32_t value = first & (0x7F >> extra);
    for (unsigned i = 0; i < extra && *text; ++i) value = (value << 6) | (uint8_t(*text++) & 63);
    return value;
}
inline const FontAssets::Glyph* glyph(const FontAssets::Font& font, uint32_t codepoint) {
    unsigned low = 0, high = font.count;
    while (low < high) {
        const unsigned middle = (low + high) / 2;
        if (font.glyphs[middle].codepoint < codepoint) low = middle + 1;
        else high = middle;
    }
    return low < font.count && font.glyphs[low].codepoint == codepoint ? &font.glyphs[low] : nullptr;
}
inline int textWidth(const char* text, const FontAssets::Font& font = FontAssets::body) {
    int width = 0;
    while (*text) {
        const auto* value = glyph(font, nextCodepoint(text));
        width += value ? value->advance : font.height / 2;
    }
    return width;
}
inline void text(int x, int y, const char* value, uint16_t color = Configuration::ink,
                 const FontAssets::Font& font = FontAssets::body) {
    while (*value) {
        const auto* item = glyph(font, nextCodepoint(value));
        if (!item) { x += font.height / 2; continue; }
        for (int row = 0; row < font.height; ++row) {
            for (int column = 0; column < item->width; ++column) {
                const uint8_t alpha = font.alpha[item->offset + row * item->width + column];
                if (alpha) rectangle(x + column, y + row, 1, 1, color, alpha);
            }
        }
        x += item->advance;
    }
}
inline void centered(int x, int y, int width, const char* value, uint16_t color = Configuration::ink,
                     const FontAssets::Font& font = FontAssets::body) {
    text(x + (width - textWidth(value, font)) / 2, y, value, color, font);
}
inline void present() {
    const int tile = Configuration::refreshTileSize;
    for (unsigned row = 0; row < rows; ++row) {
        for (unsigned column = 0; column < columns; ++column) {
            const int x = column * tile, y = row * tile;
            const int width = x + tile > Configuration::width ? Configuration::width - x : tile;
            const int height = y + tile > Configuration::height ? Configuration::height - y : tile;
            uint32_t hash = 2166136261u;
            for (int offsetY = 0; offsetY < height; ++offsetY) {
                for (int offsetX = 0; offsetX < width; ++offsetX) {
                    hash = (hash ^ pixels[(y + offsetY) * Configuration::width + x + offsetX]) * 16777619u;
                }
            }
            const unsigned index = row * columns + column;
            if (firstFrame || hashes[index] != hash) {
                Display::writeRegion(x, y, width, height, pixels);
                hashes[index] = hash;
            }
        }
    }
    firstFrame = false;
}
}
