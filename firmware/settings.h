#ifndef SMART_WATCH_SETTINGS_H
#define SMART_WATCH_SETTINGS_H

#include "src/Watchy.h"

#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC (3600 * 8)

watchySettings settings{
    .cityID = "",
    .lat = "",
    .lon = "",
    .weatherAPIKey = "",
    .weatherURL = "",
    .weatherUnit = "metric",
    .weatherLang = "zh_cn",
    .weatherUpdateInterval = 30,
    .ntpServer = NTP_SERVER,
    .gmtOffset = GMT_OFFSET_SEC,
    .vibrateOClock = true,
};

#endif

