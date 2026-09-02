#pragma once

// U[S]ART init for the v2 (no-FIFO) IP: STM32L4, F7. Identical to
// lib/usart_v3.h minus USART_CR1_FIFOEN, which that IP does not have; the
// receive-timeout (RTOR/RTOF) machinery exists on both, so the serial.h
// DMA/IRQ handlers and the "drain on RXNE" flush loop work unchanged (the
// v2 "FIFO" is just the single holding register).

#include "serial.h"

// Full-duplex DMA RX+TX, 8N1, with receive-timeout flush of partial DMA bursts.
static inline void usart_init(struct USART_Type *u, uint32_t kernel_hz, uint32_t baud) {
	u->CR1 = USART_CR1_RTOIE;
	u->CR2 = USART_CR2_RTOEN;
	usart_rtor_rto_set(u, 160); // ~16 byte times at 8N1
	u->BRR = usart_brr(kernel_hz, baud);
	u->CR3 = USART_CR3_DMAT | USART_CR3_DMAR;
	u->CR1 |= USART_CR1_UE | USART_CR1_RE | USART_CR1_TE;
}

static inline void usart_init_tx(struct USART_Type *u, uint32_t kernel_hz, uint32_t baud) {
	u->CR1 = 0;
	u->CR2 = 0;
	u->BRR = usart_brr(kernel_hz, baud);
	u->CR3 = USART_CR3_DMAT;
	u->CR1 |= USART_CR1_UE | USART_CR1_TE;
}

// LPUART (L4): shares the USART base layout; only the BRR math differs and
// there is no receive-timeout — see the v3 header's note.
static inline void lpuart_init_tx(struct USART_Type *u, uint32_t kernel_hz, uint32_t baud) {
	u->CR1 = 0;
	u->CR2 = 0;
	u->BRR = lpuart_brr(kernel_hz, baud);
	u->CR3 = USART_CR3_DMAT;
	u->CR1 |= USART_CR1_UE | USART_CR1_TE;
}

static inline void usart_init_rx(struct USART_Type *u, uint32_t kernel_hz, uint32_t baud) {
	u->CR1 = USART_CR1_RTOIE;
	u->CR2 = USART_CR2_RTOEN;
	usart_rtor_rto_set(u, 160);
	u->BRR = usart_brr(kernel_hz, baud);
	u->CR3 = USART_CR3_DMAR;
	u->CR1 |= USART_CR1_UE | USART_CR1_RE;
}

// RS-485 half-duplex with the Driver-Enable pin (active high). Same fields
// as v3 (RM0394 38.5.20).
static inline void usart_set_rs485(struct USART_Type *u) {
	u->CR1 &= ~USART_CR1_UE;
	usart_cr1_deat_set(u, 31); // 16 = one bit time, 31 = max
	usart_cr1_dedt_set(u, 31);
	u->CR3 |= USART_CR3_DEM | USART_CR3_HDSEL;
	u->CR1 |= USART_CR1_UE;
}
