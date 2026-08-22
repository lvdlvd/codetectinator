#include "ui.h"

#include "history.h"

// ---- display power policy ---------------------------------------------------

enum { // ms of inactivity
	DIM_AFTER = 30 * 1000,
	OFF_AFTER = 150 * 1000,
	RENDER_MS = 250,
	BLINK_MS = 500,
	LONG_PRESS_MS = 400, // held past this: graph until release, no tab change
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
static int tab;   // P T H CO; ui_init boots it on CO, the raison d'etre
static int alarm; // CO alarm, 0..3
static bool bat_alarm, bat_ack;
static enum PowerState pwr;
static uint32_t last_activity_ms;
static uint32_t last_render_ms;
static bool inverted; // alarm blink phase

// per-tab presentation: display divider (history units per displayed
// 0.1-unit; T stores two decimals but shows one) and the unit label drawn
// small at the big value's lower right — on the 128x32 panel the unit IS the
// tab indicator, there is no room (or need) for a tab bar. No BAT tab: the
// battery surfaces only as the low-battery takeover.
static const struct {
	const char *unit;
	int ch;    // history channel, for the long-press graph
	int div;
	int gspan; // minimum graph y-span, in history units (data centered in it)
	bool nonneg; // clip the graph floor at 0 (ppm: a negative axis is nonsense)
} tabs[] = {
    {"hPa", HIST_P, 1, 10, false},
    {"\'C", HIST_T, 10, 100, false}, // 5x7 has no degree sign; ' reads well enough
    {"%RH", HIST_H, 1, 10, false},
    {"ppm", HIST_CO, 1, 10, true}, // last: the startup tab; 0.0..1.0 when flat at 0
};
enum { NTABS = sizeof tabs / sizeof tabs[0] };

// button gesture state: short press (release before LONG_PRESS_MS) cycles the
// tab, holding longer shows the graph until release. rotate_armed remembers
// whether the press landed on a live value screen — a press that merely woke
// a dimmed panel or acked an alarm must do nothing further on release.
static bool btn_down, rotate_armed, graphing;
static uint32_t btn_down_ms;

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

// One value fills the 128x32 panel: Scale3x digits (21 px), the unit at a
// third of that (the 5x7 base font) baseline-aligned at the lower right, the
// group centered. Optional header line above squeezes the digits to the
// bottom (alarm screens); without one the digits center vertically.
static void render_value(const char *header, const char *val, const char *unit) {
	int y = 5;
	if (header != NULL) {
		fb_text(disp, (SSD1306_W - fb_text_width(1, header)) / 2, 0, 1, header);
		y = 9;
	}
	int wv = fb_text_big_width(val), wu = fb_text_width(1, unit);
	int x0 = (SSD1306_W - (wv + 3 + wu)) / 2;
	if (x0 < 0) {
		x0 = 0;
	}
	int xe = fb_text_big(disp, x0, y, val);
	fb_text(disp, xe + 3, y + 21 - 7, 1, unit);
}

// Long-press graph, full screen: per-column min/max band of the last 15
// minutes on a nonlinear time scale — column k (0 = oldest, G_W-1 = now)
// covers ages [edge(k+1), edge(k)) with edge(k) = HIST_LEN*((G_W-k)/G_W)^2,
// so the right half spans the last ~3.7 minutes. Left label column: window
// max on top, the unit (= tab indicator) in the middle, window min at the
// bottom; ticks under the plot at 15/5/1 minutes.
enum { G_X0 = 32, G_W = SSD1306_W - G_X0, G_TOP = 1, G_BOT = SSD1306_H - 4 };

static int age_edge(int k) { return HIST_LEN * (G_W - k) * (G_W - k) / (G_W * G_W); }

static void render_graph(void) {
	char buf[8];
	int dv = tabs[tab].div;

	fb_text(disp, 0, (SSD1306_H - 7) / 2, 1, tabs[tab].unit);

	// y scale from the full-window min/max, padded; flat lines centered
	int16_t wlo, whi;
	if (!history_minmax(tabs[tab].ch, 0, HIST_LEN, &wlo, &whi)) {
		return; // no data at all yet: just the unit label
	}
	int span = whi - wlo;
	if (span < tabs[tab].gspan) { // pad to the tab's minimum scale, data centered
		wlo -= (tabs[tab].gspan - span) / 2;
		span = tabs[tab].gspan;
		whi = wlo + span;
	}
	if (tabs[tab].nonneg && wlo < 0) { // shift the padded window up to a 0 floor
		whi -= wlo;
		wlo = 0;
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
		fb_vline(disp, G_X0 + k, G_BOT - (hi - wlo) * (G_BOT - G_TOP) / span,
		         G_BOT - (lo - wlo) * (G_BOT - G_TOP) / span);
	}
	// time ticks at 15/5/1 minutes: k = G_W - G_W*sqrt(age/900)
	fb_vline(disp, G_X0, G_BOT + 1, G_BOT + 2);
	fb_vline(disp, G_X0 + 41, G_BOT + 1, G_BOT + 2);
	fb_vline(disp, G_X0 + 71, G_BOT + 1, G_BOT + 2);

	fb_text(disp, 0, 0, 1, ui_fmt1(buf, whi / dv));
	fb_text(disp, 0, G_BOT - 6, 1, ui_fmt1(buf, wlo / dv));
}

static void render_tab(void) {
	char buf[8];
	int16_t v[NTABS] = {cur.p, cur.t, cur.h, cur.co};
	if (v[tab] == HIST_NONE) {
		buf[0] = '-', buf[1] = '-', buf[2] = 0;
	} else {
		ui_fmt1(buf, v[tab] / tabs[tab].div);
	}
	// acknowledged-but-still-low battery: small inverted reminder top right;
	// an empty header drops the digits to the header layout to clear it
	bool lo = bat_alarm && bat_ack;
	if (lo) {
		static const char msg[] = "BAT LO";
		int w = fb_text_width(1, msg);
		fb_text(disp, SSD1306_W - w - 1, 1, 1, msg);
		fb_invert_rect(disp, SSD1306_W - w - 2, 0, SSD1306_W - 1, 8);
	}
	render_value(lo ? "" : NULL, buf, tabs[tab].unit);
}

static void render_alarm(void) {
	char buf[8];
	static const char *over[3] = {"CO OVER 10 ppm", "CO OVER 30 ppm", "CO OVER 70 ppm"};
	buf[0] = '-', buf[1] = '-', buf[2] = 0;
	if (cur.co != HIST_NONE) {
		ui_fmt1(buf, cur.co);
	}
	render_value(over[alarm - 1], buf, "ppm");
}

// Battery-low takeover: same shape as the CO alarm but calmer — no blink,
// and the button acknowledges it (back to the tabs until the battery
// recovers above the release threshold and dips again).
static void render_alarm_bat(void) {
	char buf[8];
	ui_fmt1(buf, (int)(cur.vbat_mv / 100));
	render_value("LOW BAT - push to ack", buf, "V");
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

void ui_button(uint32_t now_ms, bool down) {
	if (down) {
		btn_down = true;
		btn_down_ms = now_ms;
		rotate_armed = pwr == PWR_BRIGHT && alarm == 0 && !(bat_alarm && !bat_ack);
		if (alarm == 0 && bat_alarm && !bat_ack) {
			bat_ack = true; // acknowledge the battery takeover, back to the tabs
		} // else on a dimmed/off display the press only wakes it
	} else {
		btn_down = false;
		if (graphing) {
			graphing = false; // long press: release just puts the value back
		} else if (rotate_armed && now_ms - btn_down_ms < LONG_PRESS_MS) {
			tab = (tab + 1) % NTABS;
		}
	}
	last_activity_ms = now_ms;
	set_power(PWR_BRIGHT);
	last_render_ms = 0; // render now
}

void ui_tick(uint32_t now_ms) {
	// a held button counts as activity and, past the long-press threshold on
	// a live value screen, brings up the graph
	if (btn_down) {
		last_activity_ms = now_ms;
		if (rotate_armed && !graphing && now_ms - btn_down_ms >= LONG_PRESS_MS) {
			graphing = true;
			last_render_ms = 0;
		}
	}

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
	} else if (graphing) {
		render_graph();
	} else {
		render_tab();
	}
	ssd1306_flush(disp);
}
