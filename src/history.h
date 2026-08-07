#pragma once

// Sliding history, 900 int16 samples per channel, with a per-channel sample
// period: P/T/H/CO at 1 s (a 15-minute window), BAT at 10 min (a ~6-day
// discharge curve). Storage units, one decimal in an int16:
//   HIST_P  0.1 hPa   HIST_T  0.01 degC   HIST_H  0.1 %RH   HIST_CO 0.1 ppm
//   HIST_BAT 0.01 V
// HIST_NONE marks missing samples (sensor absent / not yet warmed up).

#include <stdbool.h>
#include <stdint.h>

enum { HIST_P, HIST_T, HIST_H, HIST_CO, HIST_BAT, HIST_NCH };
enum { HIST_LEN = 900 }; // samples kept per channel

#define HIST_NONE INT16_MIN

// seconds between samples, per channel — the caller's push cadence
extern const uint16_t history_period_s[HIST_NCH];

// Append one sample to a channel (every history_period_s[ch] seconds;
// HIST_NONE when there is nothing to report).
void history_push(int ch, int16_t v);

// Min/max over samples with age in [age_lo, age_hi) seconds (age 0 = the
// newest sample). Returns false if the range holds no valid samples.
bool history_minmax(int ch, int age_lo, int age_hi, int16_t *lo, int16_t *hi);
