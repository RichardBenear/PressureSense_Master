#include "Display.h"
#include <cmath>   // fabsf, roundf

// ── LovyanGFX hardware config ─────────────────────────────────────────────────

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9488 _panel;
    lgfx::Bus_SPI       _bus;

public:
    LGFX(void) {
        {
            auto cfg = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 8000000;
            cfg.freq_read   = 4000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = 19;   // D8
            cfg.pin_mosi    = 18;   // D10
            cfg.pin_miso    = 20;   // D9  (readable = false)
            cfg.pin_dc      = 16;   // D6
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs       = 17;   // D7
            cfg.pin_rst      = 21;   // D3
            cfg.pin_busy     = -1;
            cfg.panel_width  = 320;
            cfg.panel_height = 480;
            cfg.readable     = false;
            cfg.invert       = false;
            cfg.rgb_order    = false;
            cfg.dlen_16bit   = false;
            cfg.bus_shared   = true;
            _panel.config(cfg);
        }
        setPanel(&_panel);
    }
};

static LGFX tft;

// ── Color palette (RGB565) — mirrors the web UI CSS variables ─────────────────
//
//  Web hex    →  R5  G6  B5  → RGB565
//  #060a10   →   0   2   2  → 0x0042   body background
//  #0d1117   →   1   4   2  → 0x0882   header / card background
//  #0a1420   →   1   5   4  → 0x0924   gauge panel background  (NEW)
//  #061420   →   0   5   4  → 0x0124   zone card background    (NEW)
//  #0d1a2a   →   1   6   5  → 0x08C5   stat card background    (NEW)
//  #3f658d   →   8  25  17  → 0x4331   borders, grid lines
//  #546c81   →  10  27  16  → 0x5370   dim labels
//  #60a5fa   →  12  41  31  → 0x653F   mid values / meta text
//  #87bef2   →  16  47  30  → 0x85FE   bright blue text
//  #e8f4fd   →  29  61  31  → 0xEFBF   primary value text
//  #0d6efd   →   1  27  31  → 0x0B7F   accent blue
//  #c7c419   →  24  48   3  → 0xC603   section headings (yellow-green)
//  #f59e0b   →  30  39   1  → 0xF4E1   amber / warning
//  #0d5829   →   2  22   5  → 0x12c5   green / OK
//  #ef4444   →  29  17   8  → 0xEA28   red / error
//  #1a1200   →   3   9   0  → 0x1900   amber pill background   (NEW)
//  #0a2a1a   →   1  10   3  → 0x0943   status OK pill bg       (NEW)
//  #2a0a0a   →   5   2   1  → 0x2801   status ERR pill bg      (NEW)
//  #1a1500   →   3  10   0  → 0x1A00   status WARN pill bg     (NEW)

static const uint16_t C_BG_BODY   = 0x0042;
static const uint16_t C_BG_HEADER = 0x0882;
static const uint16_t C_BG_GAUGE  = 0x0924;   // gauge panel — blue-tinted dark
static const uint16_t C_BG_ZONE   = 0x0124;   // zone card  — deeper blue-green
static const uint16_t C_BG_STAT   = 0x08C5;   // mini stat cards
static const uint16_t C_BORDER    = 0x4331;
static const uint16_t C_DIM       = 0x5370;
static const uint16_t C_BLUE_HI   = 0x85FE;
static const uint16_t C_HI        = 0xEFBF;
static const uint16_t C_HEAD      = 0xC603;
static const uint16_t C_ACCENT    = 0x0B7F;
static const uint16_t C_AMBER     = 0xF4E1;
static const uint16_t C_RED       = 0xEA28;
static const uint16_t C_GREEN     = 0x12c5;
static const uint16_t C_AMBER_BG  = 0x1900;   // pill bg for amber alerts
static const uint16_t C_OK_BG     = 0x0943;   // pill bg for OK status
static const uint16_t C_ERR_BG    = 0x2801;   // pill bg for ERR status
static const uint16_t C_WARN_BG   = 0x1A00;   // pill bg for WARN status

// ── Layout (480 × 320, landscape rotation) ────────────────────────────────────
//
//  y=0..31    : header bar
//  y=32       : horizontal separator
//  y=33..157  : gauge panel      (C_BG_GAUGE)
//  y=158..196 : stat card row    (two mini cards, C_BG_STAT)
//  y=197..286 : zone info card   (C_BG_ZONE)
//  y=287..319 : today strip      (C_BG_HEADER)
//
//  x=0..151   : left panel
//  x=152      : vertical separator
//  x=153..178 : Y-axis label column  (26 px)
//  x=179      : Y-axis line
//  x=180..474 : chart data area      (295 px, 5 px right gutter)
//  y=285      : X-axis line
//  y=286..319 : time label / today strip row

static const int HDR_H     = 32;
static const int LPANEL_W  = 152;
static const int SEP_X     = 152;
static const int YAXIS_X   = 179;
static const int PLOT_X    = 180;
static const int PLOT_Y    = 33;
static const int PLOT_BOT  = 285;
static const int PLOT_H    = PLOT_BOT - PLOT_Y;   // 252 px
static const int PLOT_RIGHT_GUTTER = 5;
static const int PLOT_W    = 480 - PLOT_X - PLOT_RIGHT_GUTTER; // 295 px

// Left panel row boundaries
static const int GAUGE_TOP  = PLOT_Y;      // 33
static const int GAUGE_BOT  = 157;
static const int STAT_TOP   = 158;
static const int STAT_BOT   = 196;
static const int ZONE_TOP   = 197;
static const int ZONE_BOT   = 286;
static const int TODAY_TOP  = 287;
static const int TODAY_BOT  = 319;
static const int TIME_LABEL_Y = 311;

// ── Gauge geometry ─────────────────────────────────────────────────────────────
static const int   GAUGE_CX    = 71;
static const int   GAUGE_CY    = 88;    // absolute y from top of screen
static const int   GAUGE_R_OUT = 50;
static const int   GAUGE_R_IN  = 42;   // 8 px thick ring
static const float GAUGE_START = 225.0f;
static const float GAUGE_SWEEP = 270.0f;

static const float PSI_MAX     = 70.0f;
static const float PSI_ALL_OFF = 59.0f;
static const int   PLOT_DOT_R  = 2;

// ── Helpers ────────────────────────────────────────────────────────────────────

static int psiToY(float psi) {
    if (psi < 0.0f)    psi = 0.0f;
    if (psi > PSI_MAX) psi = PSI_MAX;
    return PLOT_BOT - (int)(psi / PSI_MAX * (float)PLOT_H);
}

static uint16_t psiColor(float psi, bool allOff) {
    if (allOff || psi >= PSI_ALL_OFF) return C_GREEN;
    if (psi >= 35.0f) return C_ACCENT;
    if (psi >= 25.0f) return C_AMBER;
    return C_RED;
}

// Mirrors the -60/-75dBm good/fair break points typical of Wi-Fi signal
// meters; -87dBm has been observed in the field as a barely-usable link
// (see the TCP keepalive comment in main.cpp).
static uint16_t rssiColor(int rssi) {
    if (rssi >= -60) return C_GREEN;
    if (rssi >= -75) return C_AMBER;
    return C_RED;
}

static int parseClockSeconds(const String& timeLabel) {
    if (timeLabel.length() < 5) return -1;
    int h = timeLabel.substring(0, 2).toInt();
    int m = timeLabel.substring(3, 5).toInt();
    int s = timeLabel.length() >= 8 ? timeLabel.substring(6, 8).toInt() : 0;
    if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59) return -1;
    return h * 3600 + m * 60 + s;
}

static String fmtClockLabel(int totalSeconds) {
    while (totalSeconds < 0) totalSeconds += 24 * 3600;
    totalSeconds %= 24 * 3600;
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", totalSeconds / 3600, (totalSeconds % 3600) / 60);
    return String(buf);
}

static void clearTftDynamicRegions(bool fullLayout) {
    if (fullLayout) {
        tft.fillScreen(C_BG_BODY);
        tft.fillRect(0, 0, 480, HDR_H, C_BG_HEADER);
        tft.fillRect(0, GAUGE_TOP, LPANEL_W, GAUGE_BOT - GAUGE_TOP, C_BG_GAUGE);
        tft.fillRect(0, STAT_TOP,  LPANEL_W, STAT_BOT  - STAT_TOP,  C_BG_BODY);
        tft.fillRect(0, ZONE_TOP,  LPANEL_W, ZONE_BOT  - ZONE_TOP,  C_BG_ZONE);
        tft.fillRect(0, TODAY_TOP, LPANEL_W, TODAY_BOT - TODAY_TOP + 1, C_BG_HEADER);
        return;
    }

    // Keep clears tight: most shapes below overwrite themselves, so broad panel
    // clears just add visible blanking.
    tft.fillRect(0, 0, 480, HDR_H + 1, C_BG_HEADER); // header + bottom border
    tft.fillRect(GAUGE_CX - 42, GAUGE_CY - 25, 84, 52, C_BG_GAUGE);
    tft.fillRect(GAUGE_CX - 30, GAUGE_CY + GAUGE_R_OUT + 3, 60, 22, C_BG_GAUGE);
    tft.fillRect(4, ZONE_TOP + 4, LPANEL_W - 4, ZONE_BOT - ZONE_TOP - 5, C_BG_ZONE);
    tft.fillRect(0, TODAY_TOP, LPANEL_W, TODAY_BOT - TODAY_TOP + 1, C_BG_HEADER);
    tft.fillRect(SEP_X + 1, PLOT_Y, 480 - (SEP_X + 1), 320 - PLOT_Y, C_BG_BODY);
}

// Draw a filled rounded-rect pill with centered text.
// LovyanGFX fillRoundRect(x, y, w, h, r, color)
static void drawPill(int cx, int cy, int w, int h,
                     uint16_t bgCol, uint16_t fgCol,
                     const char* label, uint8_t textSize = 1) {
    int x = cx - w / 2;
    int y = cy - h / 2;
    tft.fillRoundRect(x, y, w, h, h / 2, bgCol);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(fgCol, bgCol);
    tft.setTextSize(textSize);
    tft.drawString(label, cx, cy);
}

// Draw a mini stat card: label on top, value below, bordered rect.
static void drawStatCard(int x, int y, int w, int h,
                         const char* label, const String& value,
                         uint16_t valCol) {
    tft.fillRoundRect(x, y, w, h, 3, C_BG_STAT);
    tft.drawRoundRect(x, y, w, h, 3, C_BORDER);

    int cx = x + w / 2;
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(C_BLUE_HI, C_BG_STAT);
    tft.setTextSize(1);
    tft.drawString(label, cx, y + 3);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(valCol, C_BG_STAT);
    tft.setTextSize(2);
    tft.drawString(value, cx, y + h - 11);
}

// ── Public: splash screen ──────────────────────────────────────────────────────

void initTftDisplay() {
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(C_BG_BODY);
    tft.fillRect(0, 0, 480, HDR_H, C_BG_HEADER);
    tft.drawFastHLine(0, HDR_H, 480, C_BORDER);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_ACCENT, C_BG_BODY);
    tft.setTextSize(4);
    tft.drawString("PRESSURE", 180, 130);
    tft.setTextColor(C_HI, C_BG_BODY);
    tft.drawString("SENSE", 340, 130);
    tft.setTextColor(C_DIM, C_BG_BODY);
    tft.setTextSize(1);
    tft.drawString("CONNECTING...", 240, 220);
}

// ── Public: full display update ────────────────────────────────────────────────
//
//  New parameters vs. original:
//    alertZoneName        — name of first low-PSI zone, empty string = no alert

void updateTftDisplay(float psi, IPAddress ip, int wifiRssi,
                      const ZoneInfo& zone, const String& timeLabel,
                      const float* history, const uint8_t* zoneHistory,
                      int historyCount,
                      int sampleRateSec,
                      const String& alertZoneName) {

    static bool layoutDrawn = false;
    tft.startWrite();

    // zone.number, not zone.allOff: allOff is a pressure-recovery heuristic
    // that can read "not off" for hours into a normal idle decay cycle, or
    // stay "off" briefly after a zone starts until pressure actually falls.
    bool zoneActive = zone.number.length() > 0 && zone.number != "0";

    // ── Section backgrounds ────────────────────────────────────────────────────
    clearTftDynamicRegions(!layoutDrawn);
    layoutDrawn = true;

    // ── Header (y=0..31) ───────────────────────────────────────────────────────

    // Left: IP address + signal strength (top line) + mDNS hostname (bottom line)
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(C_BLUE_HI, C_BG_HEADER);
    tft.setTextSize(1);
    String ipStr = ip.toString();
    tft.drawString(ipStr, 6, 10);
    int ipEndX = 6 + tft.textWidth(ipStr);

    tft.setTextColor(C_DIM, C_BG_HEADER);
    tft.drawString(".", ipEndX + 2, 10);
    int dotEndX = ipEndX + 2 + tft.textWidth(".");

    tft.setTextColor(rssiColor(wifiRssi), C_BG_HEADER);
    tft.drawString(String(wifiRssi) + "dBm", dotEndX + 2, 10);

    tft.setTextColor(C_BLUE_HI, C_BG_HEADER);
    tft.drawString("pressure-sense.local", 6, 22);

    // Centre: zone name pill (amber) or idle pill (dim)
    //if (zone.allOff) {
        //drawPill(240, 16, 130, 18, C_BG_HEADER, C_DIM, "ALL ZONES OFF", 1);
        drawPill(240, 16, 130, 18, C_BG_HEADER, C_AMBER, "MASTER", 1);
    //} else {
        // Left accent bar + pill bg
        // tft.fillRect(170, 7, 3, 18, C_AMBER);
        // tft.fillRect(173, 7, 137, 18, C_AMBER_BG);
        // String hdrName = zone.name;
        // hdrName.toUpperCase();
         tft.setTextDatum(ML_DATUM);
         tft.setTextColor(C_AMBER, C_AMBER_BG);
         tft.setTextSize(2);
         //tft.drawString("MASTER", 178, 16);
         tft.drawString("MASTER", 200, 16);

        // Low-PSI warning badge, immediately right of the zone-name pill above.
        // if (alertZoneName.length() > 0) {
        //     const int warnX = 313;
        //     const int warnY = 7;
        //     const int warnW = 40;
        //     const int warnH = 18;
        //     tft.fillRect(warnX, warnY, 2, warnH, C_AMBER);
        //     tft.fillRect(warnX + 2, warnY, warnW - 2, warnH, C_AMBER_BG);
        //     tft.setTextDatum(MC_DATUM);
        //     tft.setTextColor(C_AMBER, C_AMBER_BG);
        //     tft.setTextSize(1);
        //     tft.drawString("LOW", warnX + warnW / 2, warnY + warnH / 2);
       // }
   // }

    // Right: live PSI reading
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(C_HI, C_BG_HEADER);
    tft.setTextSize(2);
    tft.drawString(String(psi, 1) + " PSI", 474, 16);

    // Header bottom border
    tft.drawFastHLine(0, HDR_H, 480, C_BORDER);

    // ── Gauge arc (y=33..157) ──────────────────────────────────────────────────

    // Full track
    tft.fillArc(GAUGE_CX, GAUGE_CY, GAUGE_R_OUT, GAUGE_R_IN,
                GAUGE_START, GAUGE_START + GAUGE_SWEEP, C_BORDER);

    // Active portion
    uint16_t arcCol = psiColor(psi, zone.allOff);
    float psiClamped = constrain(psi, 0.0f, PSI_MAX);
    float arcEnd = GAUGE_START + (psiClamped / PSI_MAX) * GAUGE_SWEEP;
    if (arcEnd > GAUGE_START + 1.5f) {
        tft.fillArc(GAUGE_CX, GAUGE_CY, GAUGE_R_OUT, GAUGE_R_IN,
                    GAUGE_START, arcEnd, arcCol);
    }

    // PSI value centred inside arc
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_HI, C_BG_GAUGE);
    tft.setTextSize(3);
    tft.drawString(String((int)roundf(psi)), GAUGE_CX, GAUGE_CY - 2);

    // "PSI" unit label
    tft.setTextColor(C_BLUE_HI, C_BG_GAUGE);
    tft.setTextSize(1);
    tft.drawString("PSI", GAUGE_CX, GAUGE_CY + 18);

    // ── Status pill (replaces dot + text) ─────────────────────────────────────
    //  Centred below the gauge arc, above the stat cards
    int pillY = GAUGE_CY + GAUGE_R_OUT + 12;   // y ≈ 150, within gauge panel
    const char* statusStr;
    uint16_t    pillBg, pillFg;
    if (!zoneActive) {
        statusStr = "IDLE";  pillBg = C_BG_HEADER; pillFg = C_DIM;
    } else if (psi >= 35.0f) {
        statusStr = "OK";    pillBg = C_OK_BG;     pillFg = C_GREEN;
    } else if (psi >= 25.0f) {
        statusStr = "WARN";  pillBg = C_WARN_BG;   pillFg = C_AMBER;
    } else if (zone.avgPsi > 0.0f && psi > zone.avgPsi) {
        statusStr = "HIGH";  pillBg = C_ERR_BG;    pillFg = C_RED;
    } else {
        statusStr = "LOW";   pillBg = C_ERR_BG;    pillFg = C_RED;
    }
    drawPill(GAUGE_CX, pillY, 48, 14, pillBg, pillFg, statusStr, 1);

    // ── Mini stat cards (y=158..207) ──────────────────────────────────────────
    //  Two equal cards side-by-side with a 4px gap and 2px margin each side.
    const int cardY = STAT_TOP + 2;
    const int cardH = (STAT_BOT - STAT_TOP) - 4;
    const int cardW = (LPANEL_W - 6) / 2;   // ~73 px each

    drawStatCard(2,          cardY, cardW, cardH,
                 "AVG PSI",  String((int)roundf(zone.avgPsi)), C_BLUE_HI);
    drawStatCard(3 + cardW,  cardY, cardW, cardH,
                 "RUN",      zone.run + "m",                   C_BLUE_HI);

    // ── Zone card (y=208..286) ─────────────────────────────────────────────────

    // Top border + left accent bar
    tft.drawFastHLine(0, ZONE_TOP, LPANEL_W, C_BORDER);
    tft.fillRect(0, ZONE_TOP + 1, 3, ZONE_BOT - ZONE_TOP - 1, C_ACCENT);

    const int cx = 8;   // content left x

    // Section label
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_HEAD, C_BG_ZONE);
    tft.setTextSize(1);
    tft.drawString("ACTIVE ZONE", cx, ZONE_TOP + 5);

    // Zone name 
    String zoneName = zone.name;
    zoneName.toUpperCase();
    tft.setTextColor(C_HI, C_BG_ZONE);
    tft.setTextSize(1);
    tft.drawString(zoneName.length() > 0 ? zoneName : "--", cx, ZONE_TOP + 18);

    // Meta rows
    tft.setTextColor(C_BLUE_HI, C_BG_ZONE);
    tft.setTextSize(1);
    const int rowH = 14;
    int rowY = ZONE_TOP + 38;

    tft.drawString("ZONE   " + zone.number,     cx, rowY);
    tft.drawString("CTRL   " + zone.controller,  cx, rowY + rowH);
    tft.drawString("START  " + zone.start,       cx, rowY + rowH * 2);

    tft.setTextColor(C_BLUE_HI, C_BG_ZONE);
    tft.drawString("DAYS   " + zone.days, cx, rowY + rowH * 3);

    // ── Time remaining strip (y=287..319) ──────────────────────────────────────
    tft.drawFastHLine(0, TODAY_TOP, LPANEL_W, C_BORDER);

    String remainingStr = "--";
    if (zoneActive) {
        int startSec = parseClockSeconds(zone.start + ":00");
        int runMin = zone.run.toInt();
        if (startSec >= 0 && runMin > 0) {
            int remainingSec = (startSec + runMin * 60) - parseClockSeconds(timeLabel);
            if (remainingSec < 0) remainingSec = 0;
            char buf[8];
            snprintf(buf, sizeof(buf), "%d:%02d", remainingSec / 60, remainingSec % 60);
            remainingStr = String(buf);
        }
    }

    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(C_BLUE_HI, C_BG_HEADER);
    tft.setTextSize(1);
    tft.drawString("TIME REMAINING", LPANEL_W / 2, TODAY_TOP + 3);
    tft.setTextColor(C_HI, C_BG_HEADER);
    tft.setTextSize(2);
    tft.drawString(remainingStr, LPANEL_W / 2, TODAY_TOP + 14);

    // ── Panel separator ────────────────────────────────────────────────────────
    tft.drawFastVLine(SEP_X, PLOT_Y, 320 - PLOT_Y, C_BORDER);

    // ── Y-axis labels + horizontal grid ───────────────────────────────────────
    static const int gridPSI[] = {0, 10, 20, 30, 40, 50, 60, 70};
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(C_DIM, C_BG_BODY);
    tft.setTextSize(1);
    for (int p : gridPSI) {
        int y = psiToY((float)p);
        tft.drawFastHLine(YAXIS_X + 1, y, PLOT_W, C_BORDER);
        tft.drawString(String(p), YAXIS_X - 3, y);
    }

    // ── Axes ───────────────────────────────────────────────────────────────────
    tft.drawFastVLine(YAXIS_X, PLOT_Y, PLOT_BOT - PLOT_Y + 1, C_BORDER);
    tft.drawFastHLine(YAXIS_X, PLOT_BOT, PLOT_W + 1, C_BORDER);

    // ── Time labels ────────────────────────────────────────────────────────────
    tft.setTextSize(1);
    tft.setTextColor(C_BLUE_HI, C_BG_BODY);
    int nowSeconds = parseClockSeconds(timeLabel);
    int sampleSec = sampleRateSec > 0 ? sampleRateSec : 30;
    if (nowSeconds >= 0) {
        static const int tickOffsets[] = {0, 75, 150, 225, PLOT_W};
        for (int offset : tickOffsets) {
            int x = PLOT_X + offset;
            int secondsAgo = (PLOT_W - offset) * sampleSec;
            String label = fmtClockLabel(nowSeconds - secondsAgo);
            if (offset == 0) {
                tft.setTextDatum(BL_DATUM);
                tft.drawString(label, x + 2, TIME_LABEL_Y);
            } else if (offset == PLOT_W) {
                tft.setTextDatum(BR_DATUM);
                tft.drawString(label, x, TIME_LABEL_Y);
            } else {
                tft.setTextDatum(BC_DATUM);
                tft.drawString(label, x, TIME_LABEL_Y);
            }
        }
    }

    // ── Chart data line ────────────────────────────────────────────────────────
    if (historyCount < 2) {
        tft.endWrite();
        return;
    }

    // Zone transition markers are drawn behind the pressure series.
    tft.setTextSize(1);
    for (int i = 1; i < historyCount; i++) {
        if (zoneHistory[i] == zoneHistory[i - 1]) continue;

        int x = PLOT_X + PLOT_W - (historyCount - 1 - i);
        uint16_t markerCol = zoneHistory[i] == 0 ? C_BORDER : C_ACCENT;
        tft.drawFastVLine(x, PLOT_Y, PLOT_H, markerCol);

        if (zoneHistory[i] > 0) {
            tft.setTextDatum(TC_DATUM);
            tft.setTextColor(C_BLUE_HI, C_BG_BODY);
            tft.drawString("Z" + String(zoneHistory[i]), x, PLOT_Y + 2);
        }
    }

    for (int i = 1; i < historyCount; i++) {
        if (history[i - 1] <= 0.0f || history[i] <= 0.0f) continue;

        int x0 = PLOT_X + PLOT_W - (historyCount - i);
        int x1 = PLOT_X + PLOT_W - (historyCount - 1 - i);
        int y0 = psiToY(history[i - 1]);
        int y1 = psiToY(history[i]);

        uint16_t lineCol;
        if (history[i] >= PSI_ALL_OFF) {
            lineCol = C_BLUE_HI; // all zones off — brighter blue
        } else if (zone.avgPsi > 0.0f && fabsf(history[i] - zone.avgPsi) > 2.0f) {
            lineCol = C_RED;    // anomaly (> 2 PSI from average) — red
        } else {
            lineCol = C_GREEN;  // normal zone run — green
        }

        tft.drawLine(x0, y0, x1, y1, lineCol);
        tft.fillCircle(x1, y1, PLOT_DOT_R, lineCol);
    }

    tft.endWrite();
}
