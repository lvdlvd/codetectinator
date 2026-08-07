#include "bme280.h"

#include <string.h>

// Registers (datasheet chapter 5). SPI: bit 7 clear = write, set = read.
enum {
	REG_ID = 0xD0,        // = 0x60
	REG_RESET = 0xE0,     // write 0xB6
	REG_CTRL_HUM = 0xF2,  // osrs_h
	REG_STATUS = 0xF3,    //
	REG_CTRL_MEAS = 0xF4, // osrs_t | osrs_p | mode
	REG_CONFIG = 0xF5,    // t_sb | filter | spi3w
	REG_DATA = 0xF7,      // 8 bytes: press msb/lsb/xlsb, temp ditto, hum msb/lsb
	REG_CALIB00 = 0x88,   // 24 bytes T1..P9
	REG_CALIB_H1 = 0xA1,  // 1 byte
	REG_CALIB26 = 0xE1,   // 7 bytes H2..H6
};

// Full-duplex register read: clock out the read address, then len dummy
// bytes; the response arrives in the same buffer from offset 1.
static bool rd(struct BME280 *b, uint8_t reg, uint8_t *out, size_t len) {
	uint8_t buf[SPI_XMIT_BUF];
	memset(buf, 0, sizeof buf);
	buf[0] = reg | 0x80;
	// status: CRCERR/MODF/OVR are failures; BSY (0x80) may legitimately still
	// be captured at completion, and 0xffff (queue full) has low bits set too.
	if (len + 1 > sizeof buf || (spiq_xmit(b->q, b->addr, len + 1, buf) & 0x7F) != 0) {
		return false;
	}
	memcpy(out, buf + 1, len);
	return true;
}

static void wr(struct BME280 *b, uint8_t reg, uint8_t val) {
	uint8_t buf[2] = {reg & 0x7F, val};
	spiq_xmit(b->q, b->addr, 2, buf);
}

bool bme280_init(struct BME280 *b, struct SPIQ *q, uint16_t addr) {
	b->q = q;
	b->addr = addr;
	b->ok = false;

	uint8_t id = 0;
	if (!rd(b, REG_ID, &id, 1) || id != 0x60) {
		return false;
	}

	// one burst 0x88..0xA1 (T1..P9, gap, H1) and the 0xE1..0xE7 block;
	// decode (incl. the H4/H5 nibble/sign dance) in bme280_comp.c where
	// the host-side test can pin it
	uint8_t c[26], h[7];
	if (!rd(b, REG_CALIB00, c, 26) || !rd(b, REG_CALIB26, h, 7)) {
		return false;
	}
	bme280_calib_decode(&b->cal, c, h);

	wr(b, REG_CONFIG, 0); // filter off, no standby (forced mode ignores t_sb)
	b->ok = true;
	return true;
}

void bme280_trigger(struct BME280 *b) {
	if (!b->ok) {
		return;
	}
	wr(b, REG_CTRL_HUM, 1);           // osrs_h x1 (latched by the ctrl_meas write)
	wr(b, REG_CTRL_MEAS, (1u << 5) |  // osrs_t x1
	                         (1u << 2) | // osrs_p x1
	                         1);         // mode = forced
}

bool bme280_read(struct BME280 *b, int32_t *t_centi, uint32_t *p_pa, uint32_t *rh_deci) {
	if (!b->ok) {
		return false;
	}
	uint8_t d[8];
	if (!rd(b, REG_DATA, d, 8)) {
		return false;
	}
	int32_t adc_P = (d[0] << 12) | (d[1] << 4) | (d[2] >> 4);
	int32_t adc_T = (d[3] << 12) | (d[4] << 4) | (d[5] >> 4);
	int32_t adc_H = (d[6] << 8) | d[7];
	if (adc_T == 0x80000 || adc_P == 0x80000) {
		return false; // reset values: no conversion yet
	}
	int32_t t_fine;
	*t_centi = bme280_comp_T(&b->cal, adc_T, &t_fine);
	*p_pa = bme280_comp_P(&b->cal, adc_P, t_fine);
	*rh_deci = (bme280_comp_H(&b->cal, adc_H, t_fine) * 10) >> 10; // Q22.10 -> 0.1 %RH
	return true;
}
