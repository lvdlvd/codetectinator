#include "bme280_comp.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

void bme280_calib_decode(struct BME280Calib *c, const uint8_t raw88[26], const uint8_t rawE1[7]) {
	c->dig_T1 = le16(raw88 + 0);
	c->dig_T2 = (int16_t)le16(raw88 + 2);
	c->dig_T3 = (int16_t)le16(raw88 + 4);
	c->dig_P1 = le16(raw88 + 6);
	c->dig_P2 = (int16_t)le16(raw88 + 8);
	c->dig_P3 = (int16_t)le16(raw88 + 10);
	c->dig_P4 = (int16_t)le16(raw88 + 12);
	c->dig_P5 = (int16_t)le16(raw88 + 14);
	c->dig_P6 = (int16_t)le16(raw88 + 16);
	c->dig_P7 = (int16_t)le16(raw88 + 18);
	c->dig_P8 = (int16_t)le16(raw88 + 20);
	c->dig_P9 = (int16_t)le16(raw88 + 22);
	c->dig_H1 = raw88[25]; // 0xA1; raw88[24] (0xA0) is a gap byte

	c->dig_H2 = (int16_t)le16(rawE1 + 0);
	c->dig_H3 = rawE1[2];
	// H4/H5 share the nibble register 0xE5: H4 = E4[7:0]:E5[3:0],
	// H5 = E6[7:0]:E5[7:4] — 12-bit SIGNED: sign-extend the MSB before
	// scaling (multiply, not shift: shifting a negative is UB).
	c->dig_H4 = (int16_t)((int8_t)rawE1[3] * 16) | (rawE1[4] & 0x0F);
	c->dig_H5 = (int16_t)((int8_t)rawE1[5] * 16) | (rawE1[4] >> 4);
	c->dig_H6 = (int8_t)rawE1[6];
}

// The t_fine clamp: +-2^20 corresponds to about +-200 degC, far outside the
// sensor's -40..85 envelope, and bounds every t_fine-derived term in the P
// and H formulas below.
enum { T_FINE_CLAMP = 1 << 20 };

// Significant-bits accounting in the trailing comments (worst case over the
// full ADC range and the tested calibration envelope, bmp388.c style): the
// datasheet's int32 version of T and H overflows inside that envelope.
int32_t bme280_comp_T(const struct BME280Calib *c, int32_t adc_T, int32_t *t_fine) {
	int64_t var1 = (((adc_T >> 3) - ((int64_t)c->dig_T1 << 1)) * c->dig_T2) >> 11; // 18+15-11 = 22 bits
	int64_t d = (adc_T >> 4) - c->dig_T1;                                          // 17 bits
	int64_t var2 = (((d * d) >> 12) * c->dig_T3) >> 14;                            // 34-12+15-14 = 23 bits
	int64_t tf = var1 + var2;
	if (tf > T_FINE_CLAMP) {
		tf = T_FINE_CLAMP;
	} else if (tf < -T_FINE_CLAMP) {
		tf = -T_FINE_CLAMP;
	}
	*t_fine = (int32_t)tf;
	return (int32_t)((tf * 5 + 128) >> 8); // 0.01 degC
}

uint32_t bme280_comp_P(const struct BME280Calib *c, int32_t adc_P, int32_t t_fine) {
	// negative operands make << UB: scale by multiplication instead (same code)
	int64_t var1 = (int64_t)t_fine - 128000;                                       // 21 bits (t_fine clamped)
	int64_t var2 = var1 * var1 * c->dig_P6;                                        // 42+15 = 57 bits
	var2 += var1 * c->dig_P5 * ((int64_t)1 << 17);                                 // 21+15+17 = 53 bits
	var2 += c->dig_P4 * ((int64_t)1 << 35);                                        // 50 bits; sum < 58
	var1 = ((var1 * var1 * c->dig_P3) >> 8) + var1 * c->dig_P2 * ((int64_t)1 << 12); // 49, 48 -> < 50 bits
	// (S * P1) >> 33 done as an exact split: S is up to 50 bits, so S*P1
	// (+16) overflows int64 when the t_fine clamp is engaged and P2/P3 sit
	// at their envelope corners
	{
		int64_t s = ((int64_t)1 << 47) + var1;
		int64_t hi = s >> 33;                                 // floor, 17 bits
		int64_t lo = s - (hi << 33);                          // in [0, 2^33)
		var1 = hi * c->dig_P1 + ((lo * c->dig_P1) >> 33);     // 33, 16 -> 34 bits
	}
	if (var1 == 0) {
		return 0; // avoid division by zero
	}
	int64_t p = 1048576 - adc_P; // 20 bits, positive
	p = (p << 31) - var2;        // 51 - 58 bits
	// legit |p| tops out near 2.3e15 = 2^51 (adc_P = 0); *3125 (+12 bits)
	// must stay under 2^63, so clamp at 2.9e15 — outside the legit envelope
	if (p > 2900000000000000LL) {
		p = 2900000000000000LL;
	} else if (p < -2900000000000000LL) {
		p = -2900000000000000LL;
	}
	p = p * 3125 / var1; // <= 63 bits before the divide
	// legit p (Q24.8 Pa) stays under 2^31; bound it so the P9 square below
	// cannot overflow when a hostile calibration makes var1 tiny
	if (p > (int64_t)1 << 36) {
		p = (int64_t)1 << 36;
	} else if (p < -((int64_t)1 << 36)) {
		p = -((int64_t)1 << 36);
	}
	var1 = ((int64_t)c->dig_P9 * (p >> 13) * (p >> 13)) >> 25; // 15+23+23-25 = 36 bits
	var2 = ((int64_t)c->dig_P8 * p) >> 19;                     // 15+36-19 = 32 bits
	p = ((p + var1 + var2) >> 8) + c->dig_P7 * (int64_t)16;
	return (uint32_t)(p >> 8); // Q24.8 -> Pa
}

uint32_t bme280_comp_H(const struct BME280Calib *c, int32_t adc_H, int32_t t_fine) {
	// int64: the datasheet's int32 version overflows at dig_H4 << 20 for
	// H4 >= 2048 and at dig_H5 * v for large t_fine.
	int64_t v = t_fine - 76800; // 21 bits (t_fine clamped)
	// first factor: 30, 31, 33 bit terms, >>15 -> 19 bits
	// second factor: ((27-10)*(17)+21 -> 24 bits)*15+13 -> 39, >>14 -> 25 bits
	v = ((((((int64_t)adc_H << 14) - c->dig_H4 * ((int64_t)1 << 20) - c->dig_H5 * v) + 16384) >> 15) *
	     (((((((v * c->dig_H6) >> 10) * (((v * c->dig_H3) >> 11) + 32768)) >> 10) + 2097152) *
	           c->dig_H2 +
	       8192) >>
	      14)); // 19+25 = 44 bits
	v -= ((((v >> 15) * (v >> 15)) >> 7) * c->dig_H1) >> 4; // (29+29-7+8)-4 = 55 bits
	v = v < 0 ? 0 : v;
	v = v > 419430400 ? 419430400 : v;
	return (uint32_t)(v >> 12); // Q22.10 %RH
}
