#pragma once
#include <Arduino.h>
#include <esp_timer.h>
#include <time.h>
#include <stdint.h>

// 通过 USB 主机提供时间锚点，并用 uptime 推导后续记录时间。
namespace Clock {
inline bool valid = false;
inline int32_t offsetMinutes = 480;
inline int64_t anchorEpochMilliseconds = 0;
inline uint64_t anchorUptimeMilliseconds = 0;
inline char source[] = "usb_host";

inline bool digits(const char* value, unsigned start, unsigned count) {
    for (unsigned index = start; index < start + count; ++index)
        if (value[index] < '0' || value[index] > '9') return false;
    return true;
}
inline int number(const char* value, unsigned start, unsigned count) {
    int result = 0;
    for (unsigned index = start; index < start + count; ++index) result = result * 10 + value[index] - '0';
    return result;
}
inline int64_t daysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = unsigned(year - era * 400);
    const unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + int64_t(dayOfEra) - 719468;
}
inline bool leap(int year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
inline bool setFromIso(const char* value) {
    if (!value || strlen(value) != 25 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || (value[19] != '+' && value[19] != '-') || value[22] != ':') return false;
    if (!digits(value, 0, 4) || !digits(value, 5, 2) || !digits(value, 8, 2) || !digits(value, 11, 2) ||
        !digits(value, 14, 2) || !digits(value, 17, 2) || !digits(value, 20, 2) || !digits(value, 23, 2)) return false;
    const int year = number(value, 0, 4), month = number(value, 5, 2), day = number(value, 8, 2);
    const int hour = number(value, 11, 2), minute = number(value, 14, 2), second = number(value, 17, 2);
    const int zoneHour = number(value, 20, 2), zoneMinute = number(value, 23, 2);
    if (year < 2024 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
        second > 59 || zoneHour > 23 || zoneMinute > 59) return false;
    const unsigned daysInMonth[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    if (day > int(daysInMonth[month]) + (month == 2 && leap(year))) return false;
    offsetMinutes = (value[19] == '-' ? -1 : 1) * (zoneHour * 60 + zoneMinute);
    const int64_t localSeconds = (daysFromCivil(year, month, day) * 86400LL) + hour * 3600 + minute * 60 + second;
    anchorEpochMilliseconds = (localSeconds - int64_t(offsetMinutes) * 60) * 1000;
    anchorUptimeMilliseconds = uint64_t(esp_timer_get_time() / 1000);
    valid = true;
    Serial.printf("{\"event\":\"clock_set\",\"iso\":\"%s\",\"offset_minutes\":%ld}\n", value, long(offsetMinutes));
    return true;
}
inline uint64_t epochMilliseconds() {
    return valid ? uint64_t(anchorEpochMilliseconds + int64_t(uint64_t(esp_timer_get_time() / 1000) - anchorUptimeMilliseconds)) : 0;
}
inline bool isoNow(char* output, size_t capacity) {
    if (!valid || capacity < 26) return false;
    const time_t shiftedSeconds = time_t(epochMilliseconds() / 1000) + offsetMinutes * 60;
    struct tm calendar;
    gmtime_r(&shiftedSeconds, &calendar);
    const char sign = offsetMinutes < 0 ? '-' : '+';
    const int absoluteOffset = abs(offsetMinutes);
    const int written = snprintf(output, capacity, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
        calendar.tm_year + 1900, calendar.tm_mon + 1, calendar.tm_mday, calendar.tm_hour, calendar.tm_min,
        calendar.tm_sec, sign, absoluteOffset / 60, absoluteOffset % 60);
    return written > 0 && size_t(written) < capacity;
}
inline bool minuteTimestamp(uint32_t minute, char* output, size_t capacity) {
    if (!valid || capacity < 26) return false;
    const int64_t milliseconds = anchorEpochMilliseconds + int64_t(anchorUptimeMilliseconds - anchorUptimeMilliseconds) + int64_t(minute) * 60000;
    const time_t shiftedSeconds = time_t(milliseconds / 1000) + offsetMinutes * 60;
    struct tm calendar;
    gmtime_r(&shiftedSeconds, &calendar);
    const char sign = offsetMinutes < 0 ? '-' : '+';
    const int absoluteOffset = abs(offsetMinutes);
    const int written = snprintf(output, capacity, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d",
        calendar.tm_year + 1900, calendar.tm_mon + 1, calendar.tm_mday, calendar.tm_hour, calendar.tm_min,
        calendar.tm_sec, sign, absoluteOffset / 60, absoluteOffset % 60);
    return written > 0 && size_t(written) < capacity;
}
}
