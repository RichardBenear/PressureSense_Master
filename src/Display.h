#ifndef TFT_DISPLAY_H
#define TFT_DISPLAY_H

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <IPAddress.h>

// ── Chart buffer ──────────────────────────────────────────────────────────────
// Rolling chart buffer; display currently renders the newest 295 px with a right gutter.
#define CHART_BUFFER_SIZE 300

// ── Zone info passed from main to the display ─────────────────────────────────
struct ZoneInfo {
    String number;
    String name;
    String controller;
    String days;
    String start;
    String run;
    float  avgPsi;
    bool   allOff;    // true when pressure >= ZONES_ALL_OFF_PSI
};

void initTftDisplay();
void updateTftDisplay(float psi, IPAddress ip, int wifiRssi,
                      const ZoneInfo& zone, const String& timeLabel,
                      const float* history, const uint8_t* zoneHistory,
                      int historyCount,
                      int sampleRateSec,
                      const String& alertZoneName);

#endif // TFT_DISPLAY_H
