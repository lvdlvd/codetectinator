#pragma once

// Codetectinator UI: one value at a time (P / T / H / CO, boots on CO),
// digits as large as the panel allows (Scale3x of font.h's big base: 21 px
// on the 128x32 bar, 42 px on the 128x64) with the unit small at the lower
// right — the unit is the tab indicator, there is no tab bar; a CO alarm
// screen that takes over above 10/30/70 ppm; button = tab cycle (or wake
// when dimmed), long press = graph; timed contrast decay and display-off.
// The layout is written in font.h's metrics, so one ui.c serves both panels.

#include "ssd1306.h"

#include <stdint.h>

// Latest readings in history units (history.h); HIST_NONE = not available.
struct UIData {
	int16_t p, t, h, co, bat; // bat in 0.01 V
	uint32_t vbat_mv;         // raw millivolts, for the battery alarm logic
};

// Format a 0.1-unit value with one decimal into buf (>= 8 bytes); returns buf.
char *ui_fmt1(char *buf, int v);

void ui_init(struct SSD1306 *d);
void ui_second(const struct UIData *v); // call once a second, after history_push
// Debounced press/release edges. A short press (released before the long-
// press threshold) cycles the tab; holding shows the current value's graph
// until release.
void ui_button(uint32_t now_ms, bool down);
void ui_tick(uint32_t now_ms);          // call from the main loop; renders when due
int ui_alarm_level(void);               // 0 none, 1 >10, 2 >30, 3 >70 ppm
