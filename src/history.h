#pragma once

// 15-minute sliding history, one int16 sample per second per channel.
// Storage units are chosen so everything fits an int16 with one decimal:
//   HIST_P  0.1 hPa    HIST_T  0.01 degC    HIST_H  0.1 %RH    HIST_CO 0.1 ppm
// HIST_NONE marks missing samples (sensor absent / not yet warmed up).

#include <stdbool.h>
#include <stdint.h>

enum { HIST_P, HIST_T, HIST_H, HIST_CO, HIST_NCH };
enum { HIST_LEN = 900 }; // seconds kept

#define HIST_NONE INT16_MIN

// Append one sample to every channel (call once a second, HIST_NONE for
// channels with nothing to report this second).
void history_push(const int16_t v[HIST_NCH]);

// Min/max over samples with age in [age_lo, age_hi) seconds (age 0 = the
// newest sample). Returns false if the range holds no valid samples.
bool history_minmax(int ch, int age_lo, int age_hi, int16_t *lo, int16_t *hi);
