#pragma once

// Clock setup for STM32L4 (RM0394 scope), header-only. The L4 counterpart of
// lib/clock.h (G4): same preset + live-query model, adapted to the L4 tree —
// MSI exists (and is the reset clock at 4 MHz), max SYSCLK is 80 MHz, VOS
// range 1 has no boost, and the flash wait-state table is the L4's
// (RM0394 3.3.3: 0WS to 16 MHz, then one per 16 MHz up to 4WS at 80).
//
// No HSE auto-measurement here: the G4 trick (TIM16 input capture of HSE/32)
// has no L4 equivalent wired to HSE, and the boards this targets (Nucleo-32
// L432KC) fit no HSE crystal. A board that does have one states it with
// clock_set_hse_hz() before calling a preset; HSE is otherwise ignored.
//
// Header-only: each including TU gets its own copy of the cached HSE rate,
// which is fine in the n-array model where only the application's main TU
// does clock setup and rate queries.
//
// Requires the narray device header as "device.h".

#include "device.h"
#include <stdint.h>

enum { HSI16_HZ = 16000000 };

static uint32_t clock_hse_hz; // 0 = no crystal (the default); set by the board

static inline void clock_set_hse_hz(uint32_t hz) { clock_hse_hz = hz; }

// MSI range frequencies, CR.MSIRANGE encoding (RM0394 6.4.1).
static inline uint32_t clock_msi_hz(void) {
	static const uint32_t f[16] = {100000, 200000, 400000, 800000, 1000000, 2000000,
	                               4000000, 8000000, 16000000, 24000000, 32000000, 48000000};
	// MSIRGSEL=0: range from CSR.MSISRANGE (Standby exit); =1: CR.MSIRANGE
	uint32_t range = (RCC.CR & RCC_CR_MSIRGSEL) ? rcc_cr_msirange_get() : rcc_csr_msisrange_get();
	return f[range & 15];
}

// ---- presets ---------------------------------------------------------------

// Run from HSI16 directly, no PLL: 16 MHz, 0 WS at VOS range 1 — the simple
// low-power option and all a 1 Hz + 921600 baud application needs.
static inline uint32_t clock_init_16(void) {
	RCC.CR |= RCC_CR_HSION;
	for (int i = 0; i < 100000 && !(RCC.CR & RCC_CR_HSIRDY); i++) {
		__asm volatile("");
	}
	if (!(RCC.CR & RCC_CR_HSIRDY)) {
		return 0; // still on MSI
	}
	flash_acr_latency_set(0); // <= 16 MHz at range 1
	rcc_cfgr_hpre_set(0);
	rcc_cfgr_ppre1_set(0);
	rcc_cfgr_ppre2_set(0);
	rcc_cfgr_sw_set(1); // HSI16
	for (int i = 0; i < 100000 && rcc_cfgr_sws_get() != 1; i++) {
		__asm volatile("");
	}
	return rcc_cfgr_sws_get() == 1 ? HSI16_HZ : 0;
}

// SYSCLK via the PLL from HSI16 (or HSE if the board declared one): PLLM to
// put the VCO input near 8 MHz, PLLN for the target with PLLR = /2. Flash
// wait states per RM0394 Table 9 (range 1), set before the switch. All waits
// bounded; returns achieved SYSCLK or 0 (core stays on its previous clock).
static inline uint32_t clock_pll_set(uint32_t target_hz) {
	uint32_t src = clock_hse_hz ? clock_hse_hz : HSI16_HZ;
	if (target_hz > 80000000) {
		target_hz = 80000000;
	}

	uint32_t m = (src + 4000000) / 8000000; // VCO input 4..16 MHz, aim 8
	if (m < 1) {
		m = 1;
	}
	if (m > 8) {
		m = 8;
	}
	uint32_t vco_in = src / m;
	uint32_t n = (target_hz * 2 + vco_in / 2) / vco_in; // PLLR = /2
	if (n < 8) {
		n = 8;
	}
	if (n > 86) {
		n = 86;
	}
	uint32_t sysclk = vco_in * n / 2;

	if (clock_hse_hz) {
		RCC.CR |= RCC_CR_HSEON;
		for (int i = 0; i < 200000 && !(RCC.CR & RCC_CR_HSERDY); i++) {
			__asm volatile("");
		}
		if (!(RCC.CR & RCC_CR_HSERDY)) {
			return 0;
		}
	} else {
		RCC.CR |= RCC_CR_HSION;
		for (int i = 0; i < 100000 && !(RCC.CR & RCC_CR_HSIRDY); i++) {
			__asm volatile("");
		}
		if (!(RCC.CR & RCC_CR_HSIRDY)) {
			return 0;
		}
	}

	RCC.CR &= ~RCC_CR_PLLON;
	for (int i = 0; i < 1000000 && (RCC.CR & RCC_CR_PLLRDY); i++) {
		__asm volatile("");
	}
	if (RCC.CR & RCC_CR_PLLRDY) {
		return 0;
	}
	rcc_pllcfgr_pllsrc_set(clock_hse_hz ? 3 : 2); // 3=HSE, 2=HSI16 (RM0394 6.4.4)
	rcc_pllcfgr_pllm_set(m - 1);                  // field = divider-1
	rcc_pllcfgr_plln_set(n);
	rcc_pllcfgr_pllr_set(0); // 0 -> /2
	RCC.PLLCFGR |= RCC_PLLCFGR_PLLREN;
	RCC.CR |= RCC_CR_PLLON;

	// wait states for the target HCLK at VOS range 1 (RM0394 Table 9)
	uint32_t ws = sysclk <= 16000000 ? 0 : sysclk <= 32000000 ? 1 : sysclk <= 48000000 ? 2 : sysclk <= 64000000 ? 3 : 4;
	FLASH.ACR |= FLASH_ACR_PRFTEN;
	flash_acr_latency_set(ws);
	for (int i = 0; i < 100000 && flash_acr_latency_get() != ws; i++) {
		__asm volatile("");
	}
	if (flash_acr_latency_get() != ws) {
		return 0;
	}

	for (int i = 0; i < 1000000 && !(RCC.CR & RCC_CR_PLLRDY); i++) {
		__asm volatile("");
	}
	if (!(RCC.CR & RCC_CR_PLLRDY)) {
		return 0;
	}
	rcc_cfgr_hpre_set(0);
	rcc_cfgr_ppre1_set(0);
	rcc_cfgr_ppre2_set(0);
	rcc_cfgr_sw_set(3); // PLL
	for (int i = 0; i < 100000 && rcc_cfgr_sws_get() != 3; i++) {
		__asm volatile("");
	}
	return rcc_cfgr_sws_get() == 3 ? sysclk : 0;
}

static inline uint32_t clock_init_80(void) { return clock_pll_set(80000000); }

// ---- live rate queries -----------------------------------------------------

static inline uint32_t clock_ahb_div(void) {
	static const uint16_t d[] = {1, 1, 1, 1, 1, 1, 1, 1, 2, 4, 8, 16, 64, 128, 256, 512};
	return d[rcc_cfgr_hpre_get()];
}
static inline uint32_t clock_apb_div(uint32_t ppre) {
	static const uint8_t d[] = {1, 1, 1, 1, 2, 4, 8, 16};
	return d[ppre & 7];
}

static inline uint32_t clock_sysclk_hz(void) {
	switch (rcc_cfgr_sws_get()) {
	case 0:
		return clock_msi_hz();
	case 1:
		return HSI16_HZ;
	case 2:
		return clock_hse_hz;
	default: { // PLL
		uint32_t src;
		switch (rcc_pllcfgr_pllsrc_get()) {
		case 1:
			src = clock_msi_hz();
			break;
		case 2:
			src = HSI16_HZ;
			break;
		case 3:
			src = clock_hse_hz;
			break;
		default:
			return 0; // PLL src "none"
		}
		uint32_t vco = (src / (rcc_pllcfgr_pllm_get() + 1)) * rcc_pllcfgr_plln_get();
		return vco / ((rcc_pllcfgr_pllr_get() + 1) * 2); // 0..3 -> /2../8
	}
	}
}

static inline uint32_t clock_hclk_hz(void) { return clock_sysclk_hz() / clock_ahb_div(); }
static inline uint32_t clock_pclk1_hz(void) { return clock_hclk_hz() / clock_apb_div(rcc_cfgr_ppre1_get()); }
static inline uint32_t clock_pclk2_hz(void) { return clock_hclk_hz() / clock_apb_div(rcc_cfgr_ppre2_get()); }

static inline uint32_t clock_pclk1_timer_hz(void) {
	return clock_apb_div(rcc_cfgr_ppre1_get()) == 1 ? clock_pclk1_hz() : 2 * clock_pclk1_hz();
}
static inline uint32_t clock_pclk2_timer_hz(void) {
	return clock_apb_div(rcc_cfgr_ppre2_get()) == 1 ? clock_pclk2_hz() : 2 * clock_pclk2_hz();
}

// USART1..3 kernel clock via CCIPR: 0=PCLK, 1=SYSCLK, 2=HSI16, 3=LSE (0).
// USART1 is on APB2, the others on APB1.
static inline uint32_t clock_usart_hz(int usart) {
	uint32_t sel;
	switch (usart) {
	case 1:
		sel = rcc_ccipr_usart1sel_get();
		break;
	case 2:
		sel = rcc_ccipr_usart2sel_get();
		break;
	default:
		sel = rcc_ccipr_usart3sel_get();
		break;
	}
	switch (sel) {
	case 0:
		return usart == 1 ? clock_pclk2_hz() : clock_pclk1_hz();
	case 1:
		return clock_sysclk_hz();
	case 2:
		return HSI16_HZ;
	default:
		return 0; // LSE
	}
}

static inline uint32_t clock_lpuart_hz(void) {
	switch (rcc_ccipr_lpuart1sel_get()) {
	case 0:
		return clock_pclk1_hz();
	case 1:
		return clock_sysclk_hz();
	case 2:
		return HSI16_HZ;
	default:
		return 0;
	}
}

// I2C1/3: 0=PCLK1, 1=SYSCLK, 2=HSI16.
static inline uint32_t clock_i2c_hz(int i2c) {
	uint32_t sel = i2c == 1 ? rcc_ccipr_i2c1sel_get() : rcc_ccipr_i2c3sel_get();
	switch (sel) {
	case 1:
		return clock_sysclk_hz();
	case 2:
		return HSI16_HZ;
	default:
		return clock_pclk1_hz();
	}
}
