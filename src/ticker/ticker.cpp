/*
 * AnimatedPixelClock - Stock Ticker Module
 *
 * Mirrors weather.cpp: the fetch runs in a one-shot task on core 0 so TLS
 * handshakes never stall the render loop on core 1. Results are published
 * under a spinlock; getTickerQuotes() hands the render loop a copy.
 *
 * Endpoint (one symbol per request):
 *   https://query1.finance.yahoo.com/v8/finance/chart/<SYM>?range=1d&interval=1d
 * Only chart.result[0].meta is kept, via an ArduinoJson filter, so the ~2 KB
 * response never needs a big document.
 */

#include "ticker.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "../config/config.h"
#include "../clocks/cycle_config.h"
#include "../network/network.h"

#define TICKER_STYLE_ID 18
#define TICKER_RETRY_INTERVAL_MS (60UL * 1000UL)
#define TICKER_IDLE_POLL_MS 5000UL
#define TICKER_GAP_MS 500          // pause between symbols, to be polite
#define TICKER_TASK_STACK 8192

static TickerSnapshot published = {};
static portMUX_TYPE tickerMux = portMUX_INITIALIZER_UNLOCKED;

// Same hand-off rules as weather.cpp: loop() sets fetchBusy before starting
// the task; the task writes waitFromMs/waitMs and clears fetchBusy last.
static volatile bool fetchBusy = false;
static volatile bool fetchKick = false;
static volatile unsigned long waitFromMs = 0;
static volatile unsigned long waitMs = 0;  // 0: check now

static unsigned long refreshIntervalMs() {
  uint8_t m = settings.tickerRefresh;
  if (m != 5 && m != 10 && m != 15) m = 10;
  return (unsigned long)m * 60UL * 1000UL;
}

// Only pay for TLS when a screen can show the result.
static bool tickerOnScreen() {
  if (settings.clockStyle == TICKER_STYLE_ID) return true;
  if (settings.clockStyle != 9) return false;
  CycleEntry entries[CYCLE_COUNT];
  if (!parseCycleConfig(settings.cycleConfig, entries)) return true;
  for (unsigned i = 0; i < CYCLE_COUNT; ++i)
    if (entries[i].style == TICKER_STYLE_ID && entries[i].seconds) return true;
  return false;
}

// "vod.l, aapl,,BARC.L" -> {"VOD.L","AAPL","BARC.L"}. Drops empties and
// anything with characters a Yahoo symbol doesn't use.
static uint8_t parseSymbols(const char* in, char out[][TICKER_SYMBOL_LEN]) {
  uint8_t n = 0;
  char cur[TICKER_SYMBOL_LEN];
  uint8_t len = 0;
  bool bad = false;
  for (const char* p = in;; ++p) {
    char c = *p;
    if (c == ',' || c == '\0') {
      if (len > 0 && !bad && n < TICKER_MAX_SYMBOLS) {
        cur[len] = '\0';
        strcpy(out[n++], cur);
      }
      len = 0;
      bad = false;
      if (c == '\0') break;
      continue;
    }
    if (c == ' ' || c == '\t') continue;
    c = toupper((unsigned char)c);
    bool okChar = isalnum((unsigned char)c) || c == '.' || c == '-' ||
                  c == '^' || c == '=';
    if (!okChar || len >= TICKER_SYMBOL_LEN - 1) bad = true;
    else cur[len++] = c;
  }
  return n;
}

void tickerNormalizeSymbols(const char* in, char* out, size_t outLen) {
  char syms[TICKER_MAX_SYMBOLS][TICKER_SYMBOL_LEN];
  uint8_t n = parseSymbols(in ? in : "", syms);
  if (outLen == 0) return;
  out[0] = '\0';
  for (uint8_t i = 0; i < n; ++i) {
    // Stop rather than store a symbol cut in half.
    if (strlen(out) + strlen(syms[i]) + (i ? 1 : 0) >= outLen) break;
    if (i) strlcat(out, ",", outLen);
    strlcat(out, syms[i], outLen);
  }
}

// Result of one request: OK, symbol unknown, or a transient failure.
enum FetchResult { FETCH_OK, FETCH_NOT_FOUND, FETCH_FAIL, FETCH_THROTTLED };

static FetchResult fetchOne(const char* sym, TickerQuote& q) {
  // '^' (indices like ^FTSE) is not URL-safe: percent-encode it.
  char encoded[TICKER_SYMBOL_LEN * 3];
  size_t j = 0;
  for (const char* p = sym; *p && j < sizeof(encoded) - 4; ++p) {
    if (*p == '^') { memcpy(encoded + j, "%5E", 3); j += 3; }
    else encoded[j++] = *p;
  }
  encoded[j] = '\0';

  char url[128];
  snprintf(url, sizeof(url),
           "https://query1.finance.yahoo.com/v8/finance/chart/%s"
           "?range=1d&interval=1d", encoded);

  WiFiClientSecure client;
  client.setInsecure();  // public, non-sensitive data; saves a cert bundle
  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);  // no chunked encoding: lets ArduinoJson read the stream
  // Browser-like UA, as used by common Yahoo clients; the default ESP32 UA
  // may be refused by the unofficial endpoint.
  http.setUserAgent("Mozilla/5.0");
  if (!http.begin(client, url)) return FETCH_FAIL;

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Ticker %s: HTTP %d\n", sym, code);
    http.end();
    if (code == 404) return FETCH_NOT_FOUND;
    if (code == 429) return FETCH_THROTTLED;
    return FETCH_FAIL;
  }

  // Keep only the fields we draw.
  JsonDocument filter;
  JsonObject f = filter["chart"]["result"][0]["meta"].to<JsonObject>();
  f["regularMarketPrice"] = true;
  f["chartPreviousClose"] = true;
  f["regularMarketChangePercent"] = true;
  f["currency"] = true;
  f["regularMarketTime"] = true;
  f["currentTradingPeriod"]["regular"]["start"] = true;
  f["currentTradingPeriod"]["regular"]["end"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("Ticker %s JSON error: %s\n", sym, err.c_str());
    return FETCH_FAIL;
  }

  JsonObject meta = doc["chart"]["result"][0]["meta"];
  if (meta.isNull() || meta["regularMarketPrice"].isNull()) return FETCH_FAIL;

  q.price = meta["regularMarketPrice"] | 0.0f;
  float prev = meta["chartPreviousClose"] | 0.0f;
  q.changePct = prev > 0 ? (q.price - prev) / prev * 100.0f
                         : (meta["regularMarketChangePercent"] | 0.0f);
  strlcpy(q.currency, meta["currency"] | "", sizeof(q.currency));

  long start = meta["currentTradingPeriod"]["regular"]["start"] | 0L;
  long end = meta["currentTradingPeriod"]["regular"]["end"] | 0L;
  long now = (long)time(nullptr);
  q.marketOpen = start > 0 && now >= start && now < end;
  q.valid = true;
  q.notFound = false;
  return FETCH_OK;
}

static bool fetchAll() {
  char syms[TICKER_MAX_SYMBOLS][TICKER_SYMBOL_LEN];
  uint8_t n = parseSymbols(settings.tickerSymbols, syms);

  TickerSnapshot old = getTickerQuotes();
  TickerSnapshot fresh = {};
  fresh.count = n;
  bool allOk = true;

  for (uint8_t i = 0; i < n; ++i) {
    TickerQuote& q = fresh.quotes[i];
    // Start from the last known value so a failed request keeps it on screen.
    for (uint8_t k = 0; k < old.count; ++k)
      if (strcmp(old.quotes[k].symbol, syms[i]) == 0) { q = old.quotes[k]; break; }
    strlcpy(q.symbol, syms[i], sizeof(q.symbol));

    FetchResult r = fetchOne(syms[i], q);
    if (r == FETCH_NOT_FOUND) { q.notFound = true; q.valid = false; }
    else if (r == FETCH_OK) netMarkOutboundOk();
    else allOk = false;
    if (r == FETCH_THROTTLED) {
      // Stop hammering; untouched symbols keep their old values.
      for (uint8_t k = i + 1; k < n; ++k) {
        for (uint8_t m = 0; m < old.count; ++m)
          if (strcmp(old.quotes[m].symbol, syms[k]) == 0) { fresh.quotes[k] = old.quotes[m]; break; }
        strlcpy(fresh.quotes[k].symbol, syms[k], sizeof(fresh.quotes[k].symbol));
      }
      break;
    }
    if (i + 1 < n) vTaskDelay(pdMS_TO_TICKS(TICKER_GAP_MS));
  }

  fresh.fetchedOnce = true;
  portENTER_CRITICAL(&tickerMux);
  published = fresh;
  portEXIT_CRITICAL(&tickerMux);
  Serial.printf("Ticker: %u symbols, %s\n", n, allOk ? "ok" : "some failed");
  return allOk;
}

TickerSnapshot getTickerQuotes() {
  portENTER_CRITICAL(&tickerMux);
  TickerSnapshot copy = published;
  portEXIT_CRITICAL(&tickerMux);
  return copy;
}

static void tickerFetchTask(void*) {
  bool ok = fetchAll();
  waitFromMs = millis();
  waitMs = ok ? refreshIntervalMs() : TICKER_RETRY_INTERVAL_MS;
  fetchBusy = false;
  vTaskDelete(nullptr);
}

void tickerLoop() {
  if (fetchBusy) return;
  const unsigned long now = millis();
  if (fetchKick) {
    fetchKick = false;
    waitMs = 0;
  }
  if (now - waitFromMs < waitMs) return;
  if (settings.tickerSymbols[0] == '\0' || !tickerOnScreen() ||
      WiFi.status() != WL_CONNECTED) {
    waitFromMs = now;
    waitMs = TICKER_IDLE_POLL_MS;
    return;
  }
  fetchBusy = true;
  if (xTaskCreatePinnedToCore(tickerFetchTask, "ticker", TICKER_TASK_STACK,
                              nullptr, 1, nullptr, 0) != pdPASS) {
    Serial.println("Ticker fetch task not started, retrying in a minute");
    fetchBusy = false;
    waitFromMs = now;
    waitMs = TICKER_RETRY_INTERVAL_MS;
  }
}

void tickerSettingsChanged() {
  fetchKick = true;
}
