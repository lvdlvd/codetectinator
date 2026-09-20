// Host-side preview of the UI's screens — build and run with `make fbtest`
// (PANEL=32 or 64). Drives ui.c through its public API exactly as main.c
// does (history, one-second data, button edges, ticks) and dumps the
// framebuffer after each screen as '#'/'.' ASCII, one char per pixel, like
// the console mirror on the target. The SPI layer is a no-op stub
// (hoststub/spi.h), so ssd1306.c's flush goes nowhere and the framebuffer
// is read back with fb_get.

#include "history.h"
#include "ui.h"

#include <stdio.h>

static struct SSD1306 d;
static uint32_t now = 1000;

static void dump(const char *title) {
	printf("--- %s\n", title);
	for (int y = 0; y < SSD1306_H; y++) {
		for (int x = 0; x < SSD1306_W; x++) {
			putchar(fb_get(&d, x, y) ? '#' : '.');
		}
		putchar('\n');
	}
}

static void tick(uint32_t ms) {
	now += ms;
	ui_tick(now);
}

// short press: release before the long-press threshold
static void press(void) {
	ui_button(now, true);
	tick(100);
	ui_button(now, false);
	tick(1);
}

int main(void) {
	// 15 minutes of history: slow triangle waves so the graph has a shape
	for (int i = 0; i < HIST_LEN; i++) {
		int tri = i % 200 < 100 ? i % 200 : 200 - i % 200; // 0..100..0
		history_push(HIST_P, 10132 - 30 + tri * 6 / 10);
		history_push(HIST_T, -1050 + tri * 3);
		history_push(HIST_H, 1000 - tri / 2);
		history_push(HIST_CO, 60 + tri / 3);
	}
	struct UIData v = {10132, -1050, 1000, 95, 812, 8120};

	ui_init(&d);
	ui_second(&v);
	tick(1);
	dump("boot: CO tab");
	press();
	dump("P tab (widest value)");
	press();
	dump("T tab");
	press();
	dump("H tab");

	ui_button(now, true); // long press: graph until release
	tick(500);
	dump("H graph (long press)");
	ui_button(now, false);
	tick(1);

	v.co = 350;
	ui_second(&v);
	tick(300);
	dump("CO alarm");
	v.co = 95;
	ui_second(&v);
	tick(300);

	v.vbat_mv = 6200;
	ui_second(&v);
	tick(300);
	dump("low battery takeover");
	press(); // acknowledge
	dump("H tab with BAT LO badge");
	return 0;
}
