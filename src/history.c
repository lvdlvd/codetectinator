#include "history.h"

static int16_t ring[HIST_NCH][HIST_LEN];
static int head;  // next write slot
static int count; // samples written so far (caps reads into unwritten slots), saturates at HIST_LEN

void history_push(const int16_t v[HIST_NCH]) {
	for (int ch = 0; ch < HIST_NCH; ch++) {
		ring[ch][head] = v[ch];
	}
	head = (head + 1) % HIST_LEN;
	if (count < HIST_LEN) {
		count++;
	}
}

bool history_minmax(int ch, int age_lo, int age_hi, int16_t *lo, int16_t *hi) {
	if (age_hi > count) {
		age_hi = count;
	}
	if (age_lo < 0) {
		age_lo = 0;
	}
	bool any = false;
	for (int age = age_lo; age < age_hi; age++) {
		int16_t s = ring[ch][(head - 1 - age + 2 * HIST_LEN) % HIST_LEN];
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
