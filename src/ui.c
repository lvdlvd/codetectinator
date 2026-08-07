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

// Battery alarm: below 6.5 V the 9V block is near the buck's dropout; only
// meaningful with a real battery attached — under 4.5 V the divider is
// unwired (USB bench power) and the pin reads noise. Acknowledgeable by
// button, unlike the CO alarm; CO always outranks it.
enum { BAT_LOW_MV = 6500, BAT_RELEASE_MV = 6800, BAT_SENSE_MIN_MV = 4500 };

static struct SSD1306 *disp;
static struct UIData cur = {HIST_NONE, HIST_NONE, HIST_NONE, HIST_NONE, HIST_NONE, 0};
static int tab;   // BAT P T H CO; ui_init boots it on CO, the raison d'etre
static int alarm; // CO alarm, 0..3
static bool bat_alarm, bat_ack;
static enum PowerState pwr;
static uint32_t last_activity_ms;
static uint32_t last_render_ms;
static bool inverted; // alarm blink phase

// per-tab presentation: history channel, display divider (history units per
// displayed 0.1-unit; T and BAT store two decimals but show one), and the
// unit label drawn small beside the big value
static const struct {
	const char *name;
	const char *unit;
	int ch;
	int div;
} tabs[] = {
    {"BAT", "V", HIST_BAT, 10},
    {"P", "hPa", HIST_P, 1},
    {"T", "\'C", HIST_T, 10}, // 5x7 has no degree sign; ' reads well enough
    {"H", "%RH", HIST_H, 1},
    {"CO", "ppm", HIST_CO, 1}, // rightmost: the startup tab
};
enum { NTABS = sizeof tabs / sizeof tabs[0] };

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

// Graph geometry: the min/max labels sit LEFT of the plot, max in the top
// half-height, min in the bottom. Nonlinear time scale over the plot width:
// column k (0 = oldest, G_W-1 = now) covers ages [edge(k+1), edge(k)) with
// edge(k) = HIST_LEN * ((G_W-k)/G_W)^2 — the right half spans the last ~3.7
// minutes, the left half the remaining ~11.
enum { G_X0 = 32, G_W = SSD1306_W - G_X0, G_TOP = 34, G_BOT = 60 };

static int age_edge(int k) { return HIST_LEN * (G_W - k) * (G_W - k) / (G_W * G_W); }

static void render_tab(void) {
	char buf[8];

	// tab bar: NTABS equal cells, current one inverted
	for (int i = 0; i < NTABS; i++) {
		int x0 = i * SSD1306_W / NTABS, x1 = (i + 1) * SSD1306_W / NTABS - 1;
		int w = fb_text_width(1, tabs[i].name);
		fb_text(disp, x0 + (x1 - x0 + 1 - w) / 2, 1, 1, tabs[i].name);
	}
	fb_invert_rect(disp, tab * SSD1306_W / NTABS, 0, (tab + 1) * SSD1306_W / NTABS - 1, 8);

	int16_t v[NTABS] = {cur.bat, cur.p, cur.t, cur.h, cur.co};
	int16_t val = v[tab];
	int dv = tabs[tab].div;

	// current value large (Scale3x), unit small at its lower right — the
	// value+unit group centered as a whole
	if (val == HIST_NONE) {
		ui_fmt1(buf, 0);
		buf[0] = '-', buf[1] = '-', buf[2] = 0;
	} else {
		ui_fmt1(buf, val / dv);
	}
	{
		int wv = fb_text_big_width(buf), wu = fb_text_width(1, tabs[tab].unit);
		int x0 = (SSD1306_W - (wv + 3 + wu)) / 2;
		if (x0 < 0) {
			x0 = 0;
		}
		int xe = fb_text_big(disp, x0, 11, buf);
		fb_text(disp, xe + 3, 11 + 21 - 7, 1, tabs[tab].unit); // baseline-aligned
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
		for (int k = 0; k < G_W; k++) {
			int a_hi = age_edge(k);     // older edge
			int a_lo = age_edge(k + 1); // newer edge
			if (a_hi == a_lo) {
				a_hi = a_lo + 1; // rightmost columns: at least one sample wide
			}
			int16_t lo, hi;
			if (!history_minmax(tabs[tab].ch, a_lo, a_hi, &lo, &hi)) {
				continue;
			}
			int y0 = G_BOT - (hi - wlo) * (G_BOT - G_TOP) / span;
			int y1 = G_BOT - (lo - wlo) * (G_BOT - G_TOP) / span;
			fb_vline(disp, G_X0 + k, y0, y1);
		}
		// time ticks at 15/5/1 minutes: k = G_W - G_W*sqrt(age/900)
		fb_vline(disp, G_X0, G_BOT + 1, G_BOT + 2);
		fb_vline(disp, G_X0 + 41, G_BOT + 1, G_BOT + 2);
		fb_vline(disp, G_X0 + 71, G_BOT + 1, G_BOT + 2);

		// min/max labels left of the plot: max in the top half, min in the
		// bottom half of the graph's height
		fb_text(disp, 0, G_TOP + (G_BOT - G_TOP) / 4 - 3, 1, ui_fmt1(buf, whi / dv));
		fb_text(disp, 0, G_TOP + 3 * (G_BOT - G_TOP) / 4 - 3, 1, ui_fmt1(buf, wlo / dv));
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

// Battery-low takeover: same shape as the CO alarm but calmer — no blink,
// and the button acknowledges it (back to the tabs until the battery
// recovers above the release threshold and dips again).
static void render_alarm_bat(void) {
	char buf[8];

	fb_hline(disp, 0, SSD1306_W - 1, 0);
	fb_hline(disp, 0, SSD1306_W - 1, SSD1306_H - 1);

	int w = fb_text_big_width("BAT");
	fb_text_big(disp, (SSD1306_W - w) / 2, 4, "BAT");

	ui_fmt1(buf, (int)(cur.vbat_mv / 100));
	w = fb_text_big_width(buf) + fb_text_width(1, "V") + 2;
	int x = fb_text_big(disp, (SSD1306_W - w) / 2, 27, buf);
	fb_text(disp, x + 2, 41, 1, "V");

	static const char msg[] = "LOW BATTERY - push to ack";
	fb_text(disp, (SSD1306_W - fb_text_width(1, msg)) / 2, 52, 1, msg);
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
	tab = NTABS - 1; // start on CO, the rightmost tab
	pwr = PWR_BRIGHT;
	last_activity_ms = 0;
}

void ui_second(const struct UIData *v) { cur = *v; }

void ui_button(uint32_t now_ms) {
	if (alarm == 0 && bat_alarm && !bat_ack) {
		bat_ack = true; // acknowledge the battery takeover, back to the tabs
	} else if (pwr == PWR_BRIGHT && alarm == 0) {
		tab = (tab + 1) % NTABS;
	} // else: a press on a dimmed/off display only wakes it
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

	// battery alarm, lower priority than CO: latches while the voltage sits
	// in the [sense-connected, low) window, releases with hysteresis
	bool bat_low = cur.vbat_mv >= BAT_SENSE_MIN_MV &&
	               cur.vbat_mv < (bat_alarm ? BAT_RELEASE_MV : BAT_LOW_MV);
	if (!bat_low) {
		bat_ack = false; // recovered: the next dip alarms afresh
	}
	if (bat_low && !bat_alarm && alarm == 0) {
		set_power(PWR_BRIGHT);
	}
	bat_alarm = bat_low;
	bool bat_takeover = bat_alarm && !bat_ack && alarm == 0;

	if (alarm > 0) {
		// blink for attention (CO only — the battery takeover stays calm)
		bool phase = (now_ms / BLINK_MS) & 1;
		if (phase != inverted) {
			ssd1306_invert(disp, inverted = phase);
		}
	} else {
		if (inverted) {
			ssd1306_invert(disp, inverted = false);
		}
		if (bat_takeover) {
			set_power(PWR_BRIGHT); // hold the panel awake while unacknowledged
		} else {
			uint32_t idle = now_ms - last_activity_ms;
			set_power(idle > OFF_AFTER ? PWR_OFF : idle > DIM_AFTER ? PWR_DIM : PWR_BRIGHT);
		}
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
	} else if (bat_takeover) {
		render_alarm_bat();
	} else {
		render_tab();
	}
	ssd1306_flush(disp);
}
