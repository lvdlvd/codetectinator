#include "battery.h"

#include "device.h"
#include "pinmux.h"

#include "board.h" // BOARD_VBAT_ADC_CHAN + board_adc_clock()

// 9V block battery through a 3:1 divider into PA1 (= ADC1_IN2 on the G4,
// ADC1_IN6 on the L4 — the channel comes from board.h), VDDA = the regulated
// 3.3 V rail. Single software-triggered conversion, 640.5-cycle sample time
// (the divider is high-impedance), polled with bounded waits — a dead ADC
// returns 0 rather than hanging the loop. The ADC IP generation is shared
// between G4 and L4; the clock plumbing differs and lives in board.h.

enum { CHAN = BOARD_VBAT_ADC_CHAN, DIVIDER = 3, VDDA_MV = 3300 };
static_assert(CHAN >= 1 && CHAN <= 9, "SMPR1 write below covers channels 1..9 only");

void battery_init(void) {
	board_adc_clock(); // enable + kernel clock = SYSCLK (family-specific names)

	VBAT_ADC.CR &= ~ADC_CR_DEEPPWD;
	VBAT_ADC.CR |= ADC_CR_ADVREGEN;
	for (int i = 0; i < 4000; i++) { // > 20 us regulator start-up at 16 MHz
		__asm volatile("");
	}

	VBAT_ADC.CR |= ADC_CR_ADCAL; // single-ended calibration (ADCALDIF clear)
	for (int i = 0; i < 100000 && (VBAT_ADC.CR & ADC_CR_ADCAL); i++) {
		__asm volatile("");
	}
	if (VBAT_ADC.CR & ADC_CR_ADCAL) {
		return; // calibration hung; ADEN below will fail too and reads return 0
	}

	VBAT_ADC.ISR = ADC_ISR_ADRDY;
	VBAT_ADC.CR |= ADC_CR_ADEN;
	for (int i = 0; i < 100000 && !(VBAT_ADC.ISR & ADC_ISR_ADRDY); i++) {
		__asm volatile("");
	}

	// sample time 640.5 ADC clocks; SMPR1 holds channels 0..9, 3 bits each
	VBAT_ADC.SMPR1 = (VBAT_ADC.SMPR1 & ~(7u << (3 * CHAN))) | (7u << (3 * CHAN));
	vbat_adc_sqr1(CHAN);
}

uint32_t battery_read_mv(void) {
	if (!(VBAT_ADC.ISR & ADC_ISR_ADRDY)) {
		return 0; // init failed
	}
	VBAT_ADC.ISR = ADC_ISR_EOC;
	VBAT_ADC.CR |= ADC_CR_ADSTART;
	for (int i = 0; i < 100000 && !(VBAT_ADC.ISR & ADC_ISR_EOC); i++) {
		__asm volatile("");
	}
	if (!(VBAT_ADC.ISR & ADC_ISR_EOC)) {
		return 0;
	}
	uint32_t counts = VBAT_ADC.DR & 0xFFF;
	return counts * (VDDA_MV * DIVIDER) / 4095;
}
