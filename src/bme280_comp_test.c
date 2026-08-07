// Host-side unit test for bme280_comp.c — build and run with `make test`.
//
// Three layers:
//  1. Pinned vectors: the BMP280 datasheet 3.12 worked example (same T/P
//     formulas as the BME280) and hand-built calibration blocks pinning the
//     H4/H5 nibble unpacking incl. sign extension.
//  2. Oracle cross-check: the double-precision compensation (adapted from
//     the datasheet via the old stm32f103_bme280 project) over LCG-driven
//     calibration sets in generous real-world windows x full ADC ranges;
//     fixed-point must track it within tight tolerances wherever the oracle
//     output is physically meaningful.
//  3. Overflow hunt: the whole sweep runs under UBSan (see the Makefile:
//     -fsanitize=undefined -fno-sanitize-recover), so any signed overflow
//     or bad shift anywhere in bme280_comp.c aborts the test. The published
//     datasheet fixed-point code does NOT survive this sweep.

#include "bme280_comp.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond, ...)                                    \
	do {                                                    \
		if (!(cond)) {                                      \
			failures++;                                     \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);     \
			printf(__VA_ARGS__);                            \
			printf("\n");                                   \
		}                                                   \
	} while (0)

// ---- double-precision oracle (datasheet 4.2.1 / stm32f103_bme280) ----------

static double oracle_T(const struct BME280Calib *c, int32_t adc_T, double *t_fine) {
	double var1 = ((double)adc_T / 16384.0 - (double)c->dig_T1 / 1024.0) * (double)c->dig_T2;
	double var2 = ((double)adc_T / 131072.0 - (double)c->dig_T1 / 8192.0) *
	              ((double)adc_T / 131072.0 - (double)c->dig_T1 / 8192.0) * (double)c->dig_T3;
	*t_fine = var1 + var2;
	return (var1 + var2) / 5120.0; // degC
}

static double oracle_P(const struct BME280Calib *c, int32_t adc_P, double t_fine) {
	double var1 = t_fine / 2.0 - 64000.0;
	double var2 = var1 * var1 * (double)c->dig_P6 / 32768.0;
	var2 = var2 + var1 * (double)c->dig_P5 * 2.0;
	var2 = var2 / 4.0 + (double)c->dig_P4 * 65536.0;
	var1 = ((double)c->dig_P3 * var1 * var1 / 524288.0 + (double)c->dig_P2 * var1) / 524288.0;
	var1 = (1.0 + var1 / 32768.0) * (double)c->dig_P1;
	if (var1 == 0.0) {
		return 0.0;
	}
	double p = 1048576.0 - (double)adc_P;
	p = (p - var2 / 4096.0) * 6250.0 / var1;
	var1 = (double)c->dig_P9 * p * p / 2147483648.0;
	var2 = p * (double)c->dig_P8 / 32768.0;
	return p + (var1 + var2 + (double)c->dig_P7) / 16.0; // Pa
}

static double oracle_H(const struct BME280Calib *c, int32_t adc_H, double t_fine) {
	double var_H = t_fine - 76800.0;
	var_H = ((double)adc_H - ((double)c->dig_H4 * 64.0 + (double)c->dig_H5 / 16384.0 * var_H)) *
	        ((double)c->dig_H2 / 65536.0 *
	         (1.0 + (double)c->dig_H6 / 67108864.0 * var_H *
	                    (1.0 + (double)c->dig_H3 / 67108864.0 * var_H)));
	var_H = var_H * (1.0 - (double)c->dig_H1 * var_H / 524288.0);
	if (var_H > 100.0) {
		var_H = 100.0;
	} else if (var_H < 0.0) {
		var_H = 0.0;
	}
	return var_H; // %RH
}

// ---- 1. pinned vectors ------------------------------------------------------

// BMP280 datasheet 3.12: adc_T = 519888 -> 25.08 degC, adc_P = 415148 ->
// 100653.27 Pa (double precision; the int64 version lands within a Pascal).
static const struct BME280Calib example = {
    .dig_T1 = 27504,
    .dig_T2 = 26435,
    .dig_T3 = -1000,
    .dig_P1 = 36477,
    .dig_P2 = -10685,
    .dig_P3 = 3024,
    .dig_P4 = 2855,
    .dig_P5 = 140,
    .dig_P6 = -7,
    .dig_P7 = 15500,
    .dig_P8 = -14600,
    .dig_P9 = 6000,
    // no H example in the datasheet; typical values from a real part
    .dig_H1 = 75,
    .dig_H2 = 353,
    .dig_H3 = 0,
    .dig_H4 = 340,
    .dig_H5 = 60,
    .dig_H6 = 30,
};

static void test_datasheet_example(void) {
	int32_t t_fine;
	int32_t t = bme280_comp_T(&example, 519888, &t_fine);
	CHECK(t == 2508, "example T = %d centi, want 2508", t);
	CHECK(t_fine == 128422, "example t_fine = %d, want 128422", t_fine);
	uint32_t p = bme280_comp_P(&example, 415148, t_fine);
	CHECK(p >= 100652 && p <= 100654, "example P = %u Pa, want 100653 +-1", p);
}

static void test_calib_decode(void) {
	uint8_t raw88[26] = {0};
	uint8_t rawE1[7] = {0};
	// T1 = 0x6B70 (27504) little-endian; H1 at offset 25
	raw88[0] = 0x70;
	raw88[1] = 0x6B;
	raw88[25] = 75;
	// H2 = -100 = 0xFF9C; H4 = E4:E5[3:0], H5 = E6:E5[7:4], both signed
	rawE1[0] = 0x9C;
	rawE1[1] = 0xFF;
	rawE1[2] = 3;
	rawE1[3] = 0x81; // H4 msb: sign bit set
	rawE1[4] = 0xFA; // low nibble 0xA -> H4, high nibble 0xF -> H5
	rawE1[5] = 0x15; // H5 msb: positive
	rawE1[6] = 0x85; // H6 = -123

	struct BME280Calib c;
	bme280_calib_decode(&c, raw88, rawE1);
	CHECK(c.dig_T1 == 27504, "T1 = %u", c.dig_T1);
	CHECK(c.dig_H1 == 75, "H1 = %u", c.dig_H1);
	CHECK(c.dig_H2 == -100, "H2 = %d", c.dig_H2);
	CHECK(c.dig_H3 == 3, "H3 = %u", c.dig_H3);
	// H4 = sign_extend12(0x81A) = 0x81A - 0x1000 = -2022
	CHECK(c.dig_H4 == -2022, "H4 = %d, want -2022 (sign extension!)", c.dig_H4);
	// H5 = 0x15F = 351, positive
	CHECK(c.dig_H5 == 351, "H5 = %d, want 351", c.dig_H5);
	CHECK(c.dig_H6 == -123, "H6 = %d", c.dig_H6);
}

// ---- 2 + 3. oracle sweep under UBSan ---------------------------------------

static uint64_t lcg_state = 0x243F6A8885A308D3ull; // deterministic
static uint64_t lcg(void) {
	lcg_state = lcg_state * 6364136223846793005ull + 1442695040888963407ull;
	return lcg_state >> 16;
}
static int32_t rnd_in(int32_t lo, int32_t hi) { return lo + (int32_t)(lcg() % (uint32_t)(hi - lo + 1)); }

// Generous windows around real-part trim values (sign-preserving where the
// sign is structural). This is the calibration envelope the no-UB guarantee
// is tested for — far wider than production trim spread, but not the full
// int16 space, where even 64-bit intermediates cannot absorb the dynamic
// range (and where a part would be broken silicon anyway).
static void rnd_calib(struct BME280Calib *c) {
	c->dig_T1 = (uint16_t)rnd_in(20000, 35000);
	c->dig_T2 = (int16_t)rnd_in(20000, 32000);
	c->dig_T3 = (int16_t)rnd_in(-4000, 4000);
	c->dig_P1 = (uint16_t)rnd_in(25000, 50000);
	c->dig_P2 = (int16_t)rnd_in(-15000, -5000);
	c->dig_P3 = (int16_t)rnd_in(500, 8000);
	c->dig_P4 = (int16_t)rnd_in(-8000, 15000);
	c->dig_P5 = (int16_t)rnd_in(-1000, 1000);
	c->dig_P6 = (int16_t)rnd_in(-100, 100);
	c->dig_P7 = (int16_t)rnd_in(5000, 25000);
	c->dig_P8 = (int16_t)rnd_in(-25000, -5000);
	c->dig_P9 = (int16_t)rnd_in(0, 12000);
	c->dig_H1 = (uint8_t)rnd_in(0, 127);
	c->dig_H2 = (int16_t)rnd_in(250, 450);
	c->dig_H3 = (uint8_t)rnd_in(0, 10);
	c->dig_H4 = (int16_t)rnd_in(150, 600);
	c->dig_H5 = (int16_t)rnd_in(-100, 200);
	c->dig_H6 = (int8_t)rnd_in(10, 50);
}

static void test_sweep(void) {
	enum { NCALIB = 32, STEP_T = 8191, STEP_P = 16381, STEP_H = 1021 };
	long compared_t = 0, compared_p = 0, compared_h = 0;

	for (int k = 0; k < NCALIB; k++) {
		struct BME280Calib c;
		if (k == 0) {
			c = example;
		} else {
			rnd_calib(&c);
		}

		for (int32_t adc_T = 0; adc_T < (1 << 20); adc_T += STEP_T) {
			int32_t t_fine;
			int32_t t = bme280_comp_T(&c, adc_T, &t_fine);
			double tfd;
			double td = oracle_T(&c, adc_T, &tfd);

			// compare only where the oracle is physically meaningful (the
			// fixed-point t_fine clamp engages far outside this window)
			if (td >= -45.0 && td <= 90.0) {
				compared_t++;
				CHECK(fabs(t / 100.0 - td) < 0.02, "calib %d adc_T %d: T %d vs oracle %.3f", k,
				      adc_T, t, td);
			}

			for (int32_t adc_P = 0; adc_P < (1 << 20); adc_P += STEP_P) {
				uint32_t p = bme280_comp_P(&c, adc_P, t_fine);
				double pd = oracle_P(&c, adc_P, tfd);
				if (td >= -45.0 && td <= 90.0 && pd >= 30000.0 && pd <= 115000.0) {
					compared_p++;
					CHECK(fabs((double)p - pd) < 12.0, "calib %d adc_T %d adc_P %d: P %u vs %.1f",
					      k, adc_T, adc_P, p, pd);
				}
			}
			for (int32_t adc_H = 0; adc_H < (1 << 16); adc_H += STEP_H) {
				uint32_t h = bme280_comp_H(&c, adc_H, t_fine);
				double hd = oracle_H(&c, adc_H, tfd);
				if (td >= -45.0 && td <= 90.0 && hd > 0.5 && hd < 99.5) {
					compared_h++;
					CHECK(fabs(h / 1024.0 - hd) < 0.25, "calib %d adc_T %d adc_H %d: H %.2f vs %.2f",
					      k, adc_T, adc_H, h / 1024.0, hd);
				}
			}
		}
	}
	printf("sweep: %ld T, %ld P, %ld H oracle comparisons\n", compared_t, compared_p, compared_h);
	CHECK(compared_t > 1000 && compared_p > 10000 && compared_h > 10000,
	      "sweep compared too little — windows or steps wrong");
}

// The clamp paths themselves must be exercised (and be UB-free): drive the
// compensation with the calibration envelope's corners and hostile ADC
// values; no accuracy claim, only defined behavior and the H clamp range.
static void test_hostile(void) {
	static const int32_t adcs[] = {0, 1, 0x7FFFF, 0x80000, 0xFFFFF};
	for (int k = 0; k < 200; k++) {
		struct BME280Calib c;
		rnd_calib(&c);
		// push the trim words to their window corners half of the time
		if (k & 1) {
			c.dig_T2 = (k & 2) ? 32000 : 20000;
			c.dig_T3 = (k & 4) ? 4000 : -4000;
			c.dig_P6 = (k & 8) ? 100 : -100;
			c.dig_H4 = (k & 16) ? 600 : 150;
		}
		for (size_t i = 0; i < sizeof adcs / sizeof adcs[0]; i++) {
			int32_t t_fine;
			(void)bme280_comp_T(&c, adcs[i], &t_fine);
			for (size_t j = 0; j < sizeof adcs / sizeof adcs[0]; j++) {
				(void)bme280_comp_P(&c, adcs[j], t_fine);
				uint32_t h = bme280_comp_H(&c, adcs[j] & 0xFFFF, t_fine);
				CHECK(h <= 102400, "H out of clamp: %u", h);
			}
		}
	}
}

int main(void) {
	test_datasheet_example();
	test_calib_decode();
	test_sweep();
	test_hostile();
	if (failures) {
		printf("%d FAILURES\n", failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
