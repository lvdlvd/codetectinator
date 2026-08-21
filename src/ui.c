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
	int div;
} tabs[] = {
    {"hPa", 1},
    {"\'C", 10}, // 5x7 has no degree sign; ' reads well enough
    {"%RH", 1},
    {"ppm", 1}, // last: the startup tab
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

static void render_tab(void) {
	char buf[8];
	int16_t v[NTABS] = {cur.p, cur.t, cur.h, cur.co};
	if (v[tab] == HIST_NONE) {
		buf[0] = '-', buf[1] = '-', buf[2] = 0;
	} else {
		ui_fmt1(buf, v[tab] / tabs[tab].div);
	}
	render_value(NULL, buf, tabs[tab].unit);
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
