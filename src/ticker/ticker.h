/*
 * AnimatedPixelClock - Stock Ticker Module (Yahoo Finance chart endpoint)
 *
 * Same shape as the weather module: a short-lived FreeRTOS task fetches every
 * symbol in settings.tickerSymbols, then publishes a snapshot the render loop
 * reads via getTickerQuotes().
 *
 * The endpoint is unofficial and undocumented (Yahoo closed its public API in
 * 2017), so it can throttle (HTTP 429) or change without notice.
 */

#ifndef TICKER_H
#define TICKER_H

#include <Arduino.h>

#define TICKER_MAX_SYMBOLS 8
#define TICKER_SYMBOL_LEN 16   // incl. NUL, e.g. "BARC.L", "^FTSE"

struct TickerQuote {
  char symbol[TICKER_SYMBOL_LEN];
  bool valid;         // price fields hold data from at least one fetch
  bool notFound;      // server answered 404 for this symbol
  bool marketOpen;    // regularMarketTime inside today's regular session
  float price;        // regularMarketPrice, in `currency` units
  float changePct;    // vs chartPreviousClose, percent
  char currency[4];   // "GBp" (pence), "USD", "EUR", ...
};

struct TickerSnapshot {
  uint8_t count;                  // symbols configured (0..TICKER_MAX_SYMBOLS)
  bool fetchedOnce;               // at least one fetch pass completed
  TickerQuote quotes[TICKER_MAX_SYMBOLS];
};

// Call from loop(). Starts a fetch task when one is due; returns at once
// otherwise, so it is safe to call unconditionally.
void tickerLoop();

// Thread-safe copy of the latest quotes.
TickerSnapshot getTickerQuotes();

// Clean a user-typed list into the stored form: uppercase, no spaces, only
// characters Yahoo symbols use, empties dropped, max TICKER_MAX_SYMBOLS,
// comma-joined. " vod.l, aapl,," -> "VOD.L,AAPL". out needs >= 81 bytes.
void tickerNormalizeSymbols(const char* in, char* out, size_t outLen);

// Fetch on the next tickerLoop() (call after the symbol list changes).
void tickerSettingsChanged();

#endif // TICKER_H
