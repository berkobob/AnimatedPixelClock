/*
 * AnimatedPixelClock - Stock Ticker Clock (style 18)
 *
 * Layout (128x64):
 *   y=4   date (centred), AM/PM, no-WiFi icon     - usual top row
 *   y=16  HH:MM at text size 3 (24 px tall)
 *   y=46  divider line
 *   y=50  scrolling band: "VOD.L 125.80p ^0.40%   AAPL $..."
 *
 * The scroll position comes from millis(), not a per-frame counter, so the
 * speed stays the same whatever refresh rate main.cpp runs this face at.
 */

#include "clocks.h"
#include "clock_globals.h"
#include "../display/display.h"
#include "../ticker/ticker.h"

#define TK_TIME_Y 16
#define TK_DIVIDER_Y 46
#define TK_BAND_Y 50
#define TK_ITEM_GAP "   "  // 18 px between symbols

// RGB565. Up/down/symbol/digit colours are user slots (COL_TICKER_*).
#define TK_GREY 0x8410
#define TK_DIM 0x4208

// Scroll speeds in px/s for settings.tickerSpeed 0/1/2.
static const uint8_t TK_SPEEDS[3] = {12, 20, 32};

static unsigned long scrollStartMs = 0;

// One run of same-coloured text, or an arrow glyph (arrow != 0).
struct Segment {
  char text[20];
  uint16_t color;
  int8_t arrow;  // +1 up, -1 down, 0 none
};

#define TK_MAX_SEGMENTS (TICKER_MAX_SYMBOLS * 5 + 2)
static Segment segs[TK_MAX_SEGMENTS];
static uint8_t segCount = 0;

static void addText(const char* s, uint16_t color) {
  if (segCount >= TK_MAX_SEGMENTS) return;
  strlcpy(segs[segCount].text, s, sizeof(segs[segCount].text));
  segs[segCount].color = color;
  segs[segCount].arrow = 0;
  segCount++;
}

static void addArrow(int8_t dir, uint16_t color) {
  if (segCount >= TK_MAX_SEGMENTS) return;
  segs[segCount].text[0] = '\0';
  segs[segCount].color = color;
  segs[segCount].arrow = dir;
  segCount++;
}

static int segWidth(const Segment& s) {
  return s.arrow ? 6 : (int)strlen(s.text) * 6;
}

// Yahoo quotes London stocks in pence ("GBp"): show 125.80p, not 125.80.
static void formatPrice(char* buf, size_t len, const TickerQuote& q) {
  if (strcmp(q.currency, "GBp") == 0) snprintf(buf, len, "%.2fp", q.price);
  else if (strcmp(q.currency, "USD") == 0) snprintf(buf, len, "$%.2f", q.price);
  else if (q.currency[0]) snprintf(buf, len, "%.2f %s", q.price, q.currency);
  else snprintf(buf, len, "%.2f", q.price);
}

// Turns the snapshot into coloured segments; returns total width in px,
// including the trailing gap so the loop joins up evenly.
static int buildBand(const TickerSnapshot& snap) {
  segCount = 0;
  if (settings.tickerSymbols[0] == '\0') {
    addText("Set symbols in web portal" TK_ITEM_GAP, DISPLAY_WHITE);
  } else if (!snap.fetchedOnce) {
    addText("Loading..." TK_ITEM_GAP, DISPLAY_WHITE);
  } else {
    char buf[20];
    for (uint8_t i = 0; i < snap.count; ++i) {
      const TickerQuote& q = snap.quotes[i];
      snprintf(buf, sizeof(buf), "%s ", q.symbol);
      addText(buf, SPRITE_COLOR(COL_TICKER_SYMBOL));
      if (q.notFound) {
        addText("?" TK_ITEM_GAP, TK_GREY);
        continue;
      }
      if (!q.valid) {
        addText("--" TK_ITEM_GAP, TK_GREY);
        continue;
      }
      formatPrice(buf, sizeof(buf), q);
      strlcat(buf, " ", sizeof(buf));
      addText(buf, digitColor());

      float pct = q.changePct;
      bool flat = fabsf(pct) < 0.005f;
      uint16_t c = flat ? TK_GREY
                        : SPRITE_COLOR(pct > 0 ? COL_TICKER_UP : COL_TICKER_DOWN);
      if (!flat) addArrow(pct > 0 ? 1 : -1, c);
      snprintf(buf, sizeof(buf), "%.2f%%", fabsf(pct));
      addText(buf, c);
      addText(q.marketOpen ? TK_ITEM_GAP : " (closed)" TK_ITEM_GAP, TK_GREY);
    }
  }
  int w = 0;
  for (uint8_t i = 0; i < segCount; ++i) w += segWidth(segs[i]);
  return w;
}

// 5x5 triangle inside a 6 px wide cell, vertically aligned with size-1 text.
static void drawArrow(int x, int y, int8_t dir, uint16_t color) {
  if (dir > 0) display.fillTriangle(x, y + 5, x + 4, y + 5, x + 2, y + 1, color);
  else display.fillTriangle(x, y + 1, x + 4, y + 1, x + 2, y + 5, color);
}

static void drawBandAt(int x) {
  display.setTextSize(1);
  // GFX wraps a glyph that crosses the right edge onto the next line (y+8)
  // instead of clipping it. Clip instead, and restore the default after.
  display.setTextWrap(false);
  for (uint8_t i = 0; i < segCount && x < SCREEN_WIDTH; ++i) {
    int w = segWidth(segs[i]);
    if (x + w > 0) {  // skip segments fully off the left edge
      if (segs[i].arrow) {
        drawArrow(x, TK_BAND_Y, segs[i].arrow, segs[i].color);
      } else {
        display.setTextColor(segs[i].color);
        display.setCursor(x, TK_BAND_Y);
        display.print(segs[i].text);
      }
    }
    x += w;
  }
  display.setTextColor(DISPLAY_WHITE);
  display.setTextWrap(true);
}

void resetTickerAnimation() {
  scrollStartMs = millis();
}

void displayClockWithTicker() {
  struct tm timeinfo;
  if (!getTimeWithTimeout(&timeinfo)) {
    display.setTextSize(1);
    display.setCursor(20, 28);
    display.print(!ntpSynced ? "Syncing time..." : "Time Error");
    return;
  }

  // --- Top row: date, AM/PM, WiFi ---
  int displayHour, displayMin;
  bool isPM;
  formatTimeForDisplay(timeinfo.tm_hour, timeinfo.tm_min, displayHour,
                       displayMin, isPM);
  char dateStr[16];
  int dateW = formatDateString(dateStr, sizeof(dateStr), timeinfo,
                               settings.showWeekday);
  display.setTextSize(1);
  display.setTextColor(DISPLAY_WHITE);
  display.setCursor((SCREEN_WIDTH - dateW) / 2, 4);
  display.print(dateStr);
  drawMeridiemIndicator(110, 4, isPM);
  if (!wifiConnected) drawNoWiFiIcon(0, 0);

  // --- Time, size 3: 5 chars x 18 px ---
  char timeStr[6];
  snprintf(timeStr, sizeof(timeStr), "%02d%c%02d", displayHour,
           shouldShowColon() ? ':' : ' ', displayMin);
  display.setTextSize(3);
  display.setTextColor(digitColor());
  display.setCursor((SCREEN_WIDTH - 5 * 18) / 2, TK_TIME_Y);
  display.print(timeStr);
  display.setTextColor(DISPLAY_WHITE);

  display.drawFastHLine(0, TK_DIVIDER_Y, SCREEN_WIDTH, TK_DIM);

  // --- Scrolling band ---
  int cycle = buildBand(getTickerQuotes());
  if (cycle <= 0) return;
  uint8_t sp = settings.tickerSpeed <= 2 ? settings.tickerSpeed : 1;
  // 64-bit maths: elapsed ms x speed overflows 32 bits after ~2.5 days.
  uint64_t travelled =
      (uint64_t)(millis() - scrollStartMs) * TK_SPEEDS[sp] / 1000;
  int x;
  if (travelled < (uint64_t)SCREEN_WIDTH) {
    x = SCREEN_WIDTH - (int)travelled;  // first pass slides in from the right
  } else {
    x = -(int)((travelled - SCREEN_WIDTH) % (uint64_t)cycle);
  }
  for (; x < SCREEN_WIDTH; x += cycle) drawBandAt(x);
}
