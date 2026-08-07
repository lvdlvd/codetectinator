#pragma once

// BME280 calibration decode and fixed-point compensation — pure functions,
// no device or transport dependencies, so the exact firmware object also
// compiles host-side for the unit test (make test).
//
// The arithmetic follows the datasheet's fixed-point reference (4.2.3) but
// with the internals widened to int64 and explicitly clamped: the published
// version harbors signed overflows for large-but-representable inputs
// (e.g. the int32 square in the T formula, dig_H4 << 20 for H4 >= 2048).
// Within the physically possible calibration/ADC envelope the results are
// bit-identical to the datasheet code; outside it they degrade to clamped
// garbage instead of UB. The test verifies both claims.

#include <stdint.h>

struct BME280Calib {
	uint16_t dig_T1;
	int16_t dig_T2, dig_T3;
	uint16_t dig_P1;
	int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
	uint8_t dig_H1, dig_H3;
	int16_t dig_H2, dig_H4, dig_H5;
	int8_t dig_H6;
};

// Decode from the raw register images: raw88 = 0x88..0xA1 (26 bytes,
// T1..P9 then a gap byte then H1), rawE1 = 0xE1..0xE7 (7 bytes, H2..H6
// with the shared nibble register 0xE5).
void bme280_calib_decode(struct BME280Calib *c, const uint8_t raw88[26], const uint8_t rawE1[7]);

// Temperature in 0.01 degC from the 20-bit raw sample; *t_fine carries the
// fine temperature into the P and H compensation (clamped to +-2^20).
int32_t bme280_comp_T(const struct BME280Calib *c, int32_t adc_T, int32_t *t_fine);

// Pressure in Pa from the 20-bit raw sample.
uint32_t bme280_comp_P(const struct BME280Calib *c, int32_t adc_P, int32_t t_fine);

// Relative humidity in Q22.10 %RH (value/1024 = %) from the 16-bit raw sample.
uint32_t bme280_comp_H(const struct BME280Calib *c, int32_t adc_H, int32_t t_fine);
