#pragma once

// Font selection per panel height — the one preprocessor switch in the
// firmware. Everything else (ssd1306.c's renderers, ui.c's layout) is
// written against these metrics and tables:
//
//   small font  FONT_*  the 5x7-style text: units, headers, graph labels
//   big base    BIG_*   the glyphs fb_text_big scales 3x for the value
//
// FONT_BASE is the cap/digit height (rows above the baseline), FONT_H the
// full cell including descender rows; they coincide for 5x7, which has
// none. The tables are column-major, bit 0 = top row, FONT_COLS/BIG_COLS
// columns per glyph, and the renderer advances FONT_ADV per small glyph
// (5x7 stores 5 columns and gets a 1-column gap; 6x13 stores its 6-wide
// cell, gap included). Out-of-range characters render as the fallback.
// OLED_ROWS comes from the Makefile (PANEL=64|32), as does SSD1306_H.

#include <stdint.h>

#if OLED_ROWS == 64
#include "font6x13.h" // X.Org misc 6x13 "fixed", public domain, cell rows 2..12
#include "font5x14.h" // tall digits drawn for this panel
enum { FONT_ADV = 6, FONT_COLS = 6, FONT_H = 11, FONT_BASE = 9, FONT_FIRST = 0x20, FONT_N = 95, FONT_FALLBACK = '?' };
enum { BIG_COLS = 5, BIG_ROWS = 14, BIG_FIRST = '-', BIG_N = 13, BIG_FALLBACK = '-' };
typedef uint16_t fontcol_t;
static const fontcol_t (*const font_small)[FONT_COLS] = font6x13;
static const fontcol_t (*const font_bigbase)[BIG_COLS] = font5x14;
#else
#include "font5x7.h"
enum { FONT_ADV = 6, FONT_COLS = 5, FONT_H = 7, FONT_BASE = 7, FONT_FIRST = 0x20, FONT_N = 95, FONT_FALLBACK = '?' };
enum { BIG_COLS = 5, BIG_ROWS = 7, BIG_FIRST = 0x20, BIG_N = 95, BIG_FALLBACK = '?' };
typedef uint8_t fontcol_t;
static const fontcol_t (*const font_small)[FONT_COLS] = font5x7;
static const fontcol_t (*const font_bigbase)[BIG_COLS] = font5x7;
#endif

enum { BIG_H = 3 * BIG_ROWS, BIG_ADV = 3 * FONT_ADV }; // Scale3x of the big base
