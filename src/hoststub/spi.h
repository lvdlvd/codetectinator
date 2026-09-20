#pragma once

// Host stand-in for nlib/spi.h, for `make fbtest`: the Makefile puts this
// directory first on the include path so ssd1306.c builds on the host with
// a transfer that goes nowhere. No product code changes for the host build.

#include <stddef.h>
#include <stdint.h>

struct SPIQ {
	int unused;
};

static inline uint16_t spiq_xmit(struct SPIQ *q, uint16_t addr, size_t len, uint8_t *buf) {
	(void)q, (void)addr, (void)len, (void)buf;
	return 0;
}
