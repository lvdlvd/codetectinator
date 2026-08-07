#include "history.h"

const uint16_t history_period_s[HIST_NCH] = {1, 1, 1, 1, 600};

static int16_t ring[HIST_NCH][HIST_LEN];
static int head[HIST_NCH];  // next write slot
static int count[HIST_NCH]; // samples written (caps reads into unwritten slots), saturates

void history_push(int ch, int16_t v) {
	ring[ch][head[ch]] = v;
	head[ch] = (head[ch] + 1) % HIST_LEN;
	if (count[ch] < HIST_LEN) {
		count[ch]++;
	}
}

bool history_minmax(int ch, int age_lo, int age_hi, int16_t *lo, int16_t *hi) {
	if (age_hi > count[ch]) {
		age_hi = count[ch];
	}
	if (age_lo < 0) {
		age_lo = 0;
	}
	bool any = false;
	for (int age = age_lo; age < age_hi; age++) {
		int16_t s = ring[ch][(head[ch] - 1 - age + 2 * HIST_LEN) % HIST_LEN];
		if (s == HIST_NONE) {
			continue;
		}
		if (!any || s < *lo) {
			*lo = s;
		}
		if (!any || s > *hi) {
			*hi = s;
		}
		any = true;
	}
	return any;
}
