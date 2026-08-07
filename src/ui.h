#pragma once

// Codetectinator UI: four tabs (P / T / H / CO), each with the current value
// large and a 15-minute sliding min/max graph on a nonlinear time scale;
// a CO alarm screen that takes over above 10/30/70 ppm; button = tab cycle
// (or wake when dimmed); timed contrast decay and display-off.

#include "ssd1306.h"

#include <stdint.h>

// Latest readings in history units (history.h); HIST_NONE = not available.
struct UIData {
	int16_t p, t, h, co;
	uint32_t vbat_mv;
};

// Format a 0.1-unit value with one decimal into buf (>= 8 bytes); returns buf.
char *ui_fmt1(char *buf, int v);

void ui_init(struct SSD1306 *d);
void ui_second(const struct UIData *v); // call once a second, after history_push
void ui_button(uint32_t now_ms);        // debounced press event
void ui_tick(uint32_t now_ms);          // call from the main loop; renders when due
int ui_alarm_level(void);               // 0 none, 1 >10, 2 >30, 3 >70 ppm
