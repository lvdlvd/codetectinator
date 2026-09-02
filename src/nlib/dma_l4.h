#pragma once

// DMA request routing for STM32L4 — no DMAMUX: each channel has a 4-bit
// request-selection code in DMA_CSELR, and every peripheral request is only
// wired to FIXED channels (RM0394 Tables 41/42; the ground-truth dump lives
// in periph/stm32l4/DMA.periph's header comment). The family-neutral channel
// helpers live in lib/dma.h (pulled in here); the DMAMUX counterpart is
// lib/dma_g4.h.
//
// A DMA_REQ therefore encodes the whole routing fact: the CSELR code for
// each controller (they differ!) plus the bitmask of DMA_CHAN slots the
// request is wired to. dma_set_mux(ch, req) keeps the dma.h signature and
// asserts the channel is one the silicon actually supports — a wrong
// channel is a wiring error, caught at the call, not a silent dead DMA.
//
// The L4 has channels 1..7 per controller (no CH8).

#include "dma.h"

#include <assert.h>

// req = DMA1 CSELR code | DMA2 CSELR code | mask of legal DMA_CHAN slots.
#define DMA_REQ_L4(d1code, d2code, chanmask) (((d1code) << 16) | ((d2code) << 20) | (chanmask))

// The requests n-array uses, from RM0394 Table 41 (DMA1) / Table 42 (DMA2).
// Extend as needed — periph/stm32l4/DMA.periph's comment carries the rows.
enum DMA_REQ {
	DMA_REQ_NONE = DMA_REQ_L4(0, 0, 0x7f7f),
	DMA_REQ_ADC1 = DMA_REQ_L4(0x0, 0x0, (1u << DMA1_CH1) | (1u << DMA2_CH3)),
	DMA_REQ_SPI1_RX = DMA_REQ_L4(0x1, 0x4, (1u << DMA1_CH2) | (1u << DMA2_CH3)),
	DMA_REQ_SPI1_TX = DMA_REQ_L4(0x1, 0x4, (1u << DMA1_CH3) | (1u << DMA2_CH4)),
	DMA_REQ_USART1_TX = DMA_REQ_L4(0x2, 0x2, (1u << DMA1_CH4) | (1u << DMA2_CH6)),
	DMA_REQ_USART1_RX = DMA_REQ_L4(0x2, 0x2, (1u << DMA1_CH5) | (1u << DMA2_CH7)),
	DMA_REQ_USART2_RX = DMA_REQ_L4(0x2, 0, 1u << DMA1_CH6),
	DMA_REQ_USART2_TX = DMA_REQ_L4(0x2, 0, 1u << DMA1_CH7),
	DMA_REQ_USART3_TX = DMA_REQ_L4(0x2, 0, 1u << DMA1_CH2),
	DMA_REQ_USART3_RX = DMA_REQ_L4(0x2, 0, 1u << DMA1_CH3),
	DMA_REQ_SPI2_RX = DMA_REQ_L4(0x1, 0, 1u << DMA1_CH4), // L43x/44x
	DMA_REQ_SPI2_TX = DMA_REQ_L4(0x1, 0, 1u << DMA1_CH5), // L43x/44x
	DMA_REQ_SPI3_RX = DMA_REQ_L4(0, 0x3, 1u << DMA2_CH1),
	DMA_REQ_SPI3_TX = DMA_REQ_L4(0, 0x3, 1u << DMA2_CH2),
	DMA_REQ_I2C1_TX = DMA_REQ_L4(0x3, 0x5, (1u << DMA1_CH6) | (1u << DMA2_CH7)),
	DMA_REQ_I2C1_RX = DMA_REQ_L4(0x3, 0x5, (1u << DMA1_CH7) | (1u << DMA2_CH6)),
};

// select the request code on a channel (the L4 stand-in for the G4 DMAMUX
// route); asserts the request is wired to this channel at all
static inline void dma_set_mux(enum DMA_CHAN ch, enum DMA_REQ req) {
	assert(req & (1u << ch));
	uint32_t code = (ch < 8 ? (uint32_t)req >> 16 : (uint32_t)req >> 20) & 0xF;
	unsigned shift = 4 * (ch % 8);
	dma_unit(ch)->CSELR = (dma_unit(ch)->CSELR & ~(0xFu << shift)) | (code << shift);
}
