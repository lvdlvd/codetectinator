#include "ui.h"

#include "history.h"

// ---- display power policy ---------------------------------------------------

enum { // ms of inactivity
	DIM_AFTER = 30 * 1000,
	OFF_AFTER = 150 * 1000,
	RENDER_MS = 250,
	BLINK_MS = 500,
};
enum PowerState { PWR_BRIGHT, PWR_DIM, PWR_OFF };

// CO alarm thresholds in 0.1 ppm, with release hysteresis of 0.5 ppm so a
// reading hovering on a threshold does not flap the alarm screen.
static const int16_t co_thresh[3] = {100, 300, 700};
enum { CO_HYST = 5 };

static struct SSD1306 *disp;
static struct UIData cur = {HIST_NONE, HIST_NONE, HIST_NONE, HIST_NONE, 0};
static int tab;   // 0 P, 1 T, 2 H, 3 CO
static int alarm; // 0..3
static enum PowerState pwr;
static uint32_t last_activity_ms;
static uint32_t last_render_ms;
static bool inverted; // alarm blink phase

// per-tab presentation: history channel and display divider (history units
// per displayed 0.1-unit; T is stored in 0.01 degC but shown with 1 decimal)
static const struct {
	const char *name;
	int ch;
	int div;
} tabs[4] = {
    {"hPa", HIST_P, 1},
    {"\'C", HIST_T, 10}, // 5x7 has no degree sign; ' reads well enough
    {"%RH", HIST_H, 1},
    {"ppmCO", HIST_CO, 1},
};

int ui_alarm_level(void) { return alarm; }

// ---- small formatting helpers (value in 0.1 units, one decimal) ------------

char *ui_fmt1(char *buf, int v) {
	char *s = buf;
	if (v < 0) {
		*s++ = '-';
		v = -v;
	}
	char tmp[6];
	int n = 0;
	for (int w = v / 10; n == 0 || w; w /= 10) {
		tmp[n++] = '0' + w % 10;
	}
	while (n) {
		*s++ = tmp[--n];
	}
	*s++ = '.';
	*s++ = '0' + v % 10;
	*s = 0;
	return buf;
}

// ---- screens ---------------------------------------------------------------

// Nonlinear time scale: column x (0 = oldest, 127 = now) covers sample ages
// [edge(x+1), edge(x)) seconds where edge(k) = 900 * ((128-k)/128)^2 — the
// right half of the graph spans the last ~3.7 minutes, the left half the
// remaining ~11. edge(0) = 900, edge(128) = 0.
static int age_edge(int k) { return (HIST_LEN * (128 - k) * (128 - k)) >> 14; }

enum { G_TOP = 34, G_BOT = 53 }; // graph pixel rows (below the 21px big value)

static void render_tab(void) {
	char buf[8];

	// tab bar: four 32px cells, current one inverted
	for (int i = 0; i < 4; i++) {
		int w = fb_text_width(1, tabs[i].name);
		fb_text(disp, i * 32 + (32 - w) / 2, 1, 1, tabs[i].name);
	}
	fb_invert_rect(disp, tab * 32, 0, tab * 32 + 31, 8);

	int16_t v[4] = {cur.p, cur.t, cur.h, cur.co};
	int16_t val = v[tab];
	int dv = tabs[tab].div;

	// current value, large (Scale3x-smoothed)
	if (val == HIST_NONE) {
		fb_text_big(disp, (SSD1306_W - fb_text_big_width("--")) / 2, 11, "--");
	} else {
		ui_fmt1(buf, val / dv);
		fb_text_big(disp, (SSD1306_W - fb_text_big_width(buf)) / 2, 11, buf);
	}

	// graph: y scale from the full-window min/max, padded; flat lines centered
	int16_t wlo, whi;
	if (history_minmax(tabs[tab].ch, 0, HIST_LEN, &wlo, &whi)) {
		int span = whi - wlo;
		if (span < 8) { // under 0.8 display units: pad to keep noise flat
			wlo -= (8 - span) / 2;
			whi = wlo + 8;
			span = 8;
		}
		for (int x = 0; x < SSD1306_W; x++) {
			int a_hi = age_edge(x);     // older edge
			int a_lo = age_edge(x + 1); // newer edge
			if (a_hi == a_lo) {
				a_hi = a_lo + 1; // rightmost columns: at least one sample wide
			}
			int16_t lo, hi;
			if (!history_minmax(tabs[tab].ch, a_lo, a_hi, &lo, &hi)) {
				continue;
			}
			int y0 = G_BOT - (hi - wlo) * (G_BOT - G_TOP) / span;
			int y1 = G_BOT - (lo - wlo) * (G_BOT - G_TOP) / span;
			fb_vline(disp, x, y0, y1);
		}
		// time ticks at 15/5/1 minutes: x = 128 - 128*sqrt(age/900)
		fb_vline(disp, 0, G_BOT + 1, G_BOT + 2);
		fb_vline(disp, 54, G_BOT + 1, G_BOT + 2);
		fb_vline(disp, 95, G_BOT + 1, G_BOT + 2);

		// min/max annotation line
		int x = fb_text(disp, 0, 57, 1, "^");
		x = fb_text(disp, x, 57, 1, ui_fmt1(buf, whi / dv));
		x = fb_text(disp, x + 4, 57, 1, "v");
		fb_text(disp, x, 57, 1, ui_fmt1(buf, wlo / dv));
	}

	// battery, right-aligned on the bottom line (in volts, one decimal)
	if (cur.vbat_mv) {
		ui_fmt1(buf, (int)(cur.vbat_mv / 100));
		int w = fb_text_width(1, buf) + 6;
		int x = fb_text(disp, SSD1306_W - w, 57, 1, buf);
		fb_text(disp, x, 57, 1, "V");
	}
}

static void render_alarm(void) {
	char buf[8];

	fb_rect_fill(disp, 0, 0, SSD1306_W - 1, 2);
	fb_rect_fill(disp, 0, SSD1306_H - 3, SSD1306_W - 1, SSD1306_H - 1);

	int w = fb_text_big_width("CO");
	fb_text_big(disp, (SSD1306_W - w) / 2, 4, "CO");

	if (cur.co != HIST_NONE) {
		ui_fmt1(buf, cur.co);
		w = fb_text_big_width(buf) + fb_text_width(1, "ppm") + 2;
		int x = fb_text_big(disp, (SSD1306_W - w) / 2, 27, buf);
		fb_text(disp, x + 2, 41, 1, "ppm");
	}

	static const char *over[3] = {"OVER 10 ppm", "OVER 30 ppm", "OVER 70 ppm"};
	const char *msg = over[alarm - 1];
	w = fb_text_width(1, msg);
	fb_text(disp, (SSD1306_W - w) / 2, 52, 1, msg);
}

// ---- state machine ---------------------------------------------------------

static void set_power(enum PowerState p) {
	if (p == pwr) {
		return;
	}
	pwr = p;
	switch (p) {
	case PWR_BRIGHT:
		ssd1306_on(disp, true);
		ssd1306_dim(disp, false);
		break;
	case PWR_DIM:
		ssd1306_dim(disp, true);
		break;
	case PWR_OFF:
		ssd1306_on(disp, false);
		break;
	}
}

void ui_init(struct SSD1306 *d) {
	disp = d;
	pwr = PWR_BRIGHT;
	last_activity_ms = 0;
}

void ui_second(const struct UIData *v) { cur = *v; }

void ui_button(uint32_t now_ms) {
	// a press on a dimmed/off display only wakes it
	if (pwr == PWR_BRIGHT && alarm == 0) {
		tab = (tab + 1) % 4;
	}
	last_activity_ms = now_ms;
	set_power(PWR_BRIGHT);
	last_render_ms = 0; // render now
}

void ui_tick(uint32_t now_ms) {
	// alarm level with hysteresis on release
	int lvl = 0;
	if (cur.co != HIST_NONE) {
		for (int i = 0; i < 3; i++) {
			if (cur.co >= co_thresh[i] || (alarm > i && cur.co >= co_thresh[i] - CO_HYST)) {
				lvl = i + 1;
			}
		}
	}
	if (lvl > 0 && alarm == 0) {
		set_power(PWR_BRIGHT); // alarm overrides dimming, not a button: no tab change
	}
	if (lvl == 0 && alarm > 0) {
		ssd1306_invert(disp, inverted = false);
		last_activity_ms = now_ms; // linger bright after an alarm clears
	}
	alarm = lvl;

	if (alarm > 0) {
		// blink for attention
		bool phase = (now_ms / BLINK_MS) & 1;
		if (phase != inverted) {
			ssd1306_invert(disp, inverted = phase);
		}
	} else {
		uint32_t idle = now_ms - last_activity_ms;
		set_power(idle > OFF_AFTER ? PWR_OFF : idle > DIM_AFTER ? PWR_DIM : PWR_BRIGHT);
	}

	if (pwr == PWR_OFF && alarm == 0) {
		return; // nothing to draw into a sleeping panel
	}
	if (now_ms - last_render_ms < RENDER_MS && last_render_ms != 0) {
		return;
	}
	last_render_ms = now_ms;

	fb_clear(disp);
	if (alarm > 0) {
		render_alarm();
	} else {
		render_tab();
	}
	ssd1306_flush(disp);
}
