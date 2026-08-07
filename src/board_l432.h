#pragma once

// Board definition: NUCLEO-L432KC (the fallback board while the G431KB's
// ST-LINK is out of action). Selected by the Makefile (BOARD=l432), which
// copies it to board.h — run `make clean` when switching boards so the
// generated device header and linker scripts regenerate too.
//
// The breadboard needs NO rewiring vs the G431KB: every external hookup
// (SPI on D11/D12/D13, CO module on D0/D1, button A0, battery A1, the
// CS/DC/RST pins) sits on the same Nucleo-32 header positions. What
// differs is board-internal: the VCP console RX is PA15/AF3 (UM1956, not
// PA3), PA1 is ADC1_IN6 (not IN2), there is no PB8 — LD3 sits on PB3
// where it doubles as an SPI-activity light — and the L4's DMA requests
// live on fixed channels.

#include "clock_l4.h" // header-only L4 clock lib (no clock.c in CSRC)
#include "dma_l4.h"   // CSELR request routing, fixed channels
#include "usart_v2.h" // usart_init* for the no-FIFO L4 IP

#define BOARD_NAME "NUCLEO-L432KC"

// Role pins. LD3 is hardwired to PB3 = SPI1_SCK: the on-board LED becomes
// an SPI-activity light, and the LED role moves to PA5 (A4 header pin,
// wire an external LED there if wanted — it is otherwise harmless).
// BUTTON is PA4 = A3, NOT PA0/A0: with SB17 closed (UM1956 Table 6 —
// not factory default, but our bench unit measurably drives PA0) the
// ST-LINK's 8 MHz MCO reads as a furiously bouncing button. PA0 stays
// analog; A3 is PA4 on the G431KB too, so the breadboard wiring is
// identical for both boards. (No wakeup-capable pin is lost: the
// L432KC has none free — WKUP1 is the MCO pin, WKUP4 the VCP TX.)
static const enum GPIO_Pin LED = PA5;
static const enum GPIO_Pin BUTTON = PA4;   // to GND, internal pull-up
static const enum GPIO_Pin OLED_CS = PA11; // active low
static const enum GPIO_Pin OLED_DC = PA12; // low = command, high = data
static const enum GPIO_Pin OLED_RST = PB0; // active low
static const enum GPIO_Pin BME_CS = PA8;   // active low

// Battery divider input: PA1 = ADC1_IN6 on the L4.
enum { BOARD_VBAT_ADC_CHAN = 6 };

// DMA channels: FIXED per request on the L4 (RM0394 Table 41) — this is
// the only legal assignment, and it happens to need DMA1 alone. #defines
// (not an enum) so they keep the enum DMA_CHAN type and remain
// vector-table constants.
#define CH_SPI_RX DMA1_CH2
#define CH_SPI_TX DMA1_CH3
#define CH_CO_TX DMA1_CH4      // USART1_TX
#define CH_CO_RX DMA1_CH5      // USART1_RX
#define CH_CONSOLE_RX DMA1_CH6 // USART2_RX
#define CH_CONSOLE_TX DMA1_CH7 // USART2_TX
// DMA1_CHn IRQ numbers are consecutive from DMA1_CH1_IRQn on G4 and L4 alike.
#define DMA_CH_IRQN(ch) ((enum IRQn_Type)(DMA1_CH1_IRQn + (ch)))

// The board pinout (run `make pinfmt` to annotate).
static const pinconf_t board[] = {
	// Unused -> analog (low power); PAMost keeps SWDIO/SWCLK alive. PA1 is
	// the battery divider input and stays analog by this default too.
	PAMost | PIN_ANALOG,
	PBAll | PIN_ANALOG,
	// PA0 stays analog: SB4 feeds it the ST-LINK MCO clock on this board
	PA4 | PIN_INPUT | PIN_PULLUP, //% pushbutton to GND (A3)
	PA5 | PIN_OUTPUT,             //% LED role (external, optional; A4)
	PA8 | PIN_OUTPUT | PIN_HIGH,  //% BME280 /CS
	PA9_USART1_TX | PIN_HIGH,     //% CO module RX (yellow)
	PA10_USART1_RX | PIN_PULLUP,  //% CO module TX (green)
	PA11 | PIN_OUTPUT | PIN_HIGH, //% OLED /CS
	PA12 | PIN_OUTPUT,            //% OLED D/C
	PA2_USART2_TX | PIN_HIGH,     //% ST-LINK VCP (debug console TX)
	PA15_USART2_RX | PIN_PULLUP,  //% ST-LINK VCP RX — PA15/AF3 on this board!
	PB0 | PIN_OUTPUT,             //% OLED /RST, held low until released in main
	PB3_SPI1_SCK,                 //% OLED D0 + BME280 SCK (+ LD3 activity light)
	PB4_SPI1_MISO,                //% BME280 SDO
	PB5_SPI1_MOSI,                //% OLED D1 + BME280 SDI
};

static inline void board_clocks(void) {
	RCC.AHB1ENR |= RCC_AHB1ENR_DMA1EN; // no DMAMUX on L4
	RCC.AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN;
	RCC.APB1ENR1 |= RCC_APB1ENR1_USART2EN;
	RCC.APB2ENR |= RCC_APB2ENR_USART1EN | RCC_APB2ENR_SPI1EN;
}

// ADC clock + kernel source: L4 names, SYSCLK kernel (CCIPR ADCSEL = 11).
static inline void board_adc_clock(void) {
	RCC.AHB2ENR |= RCC_AHB2ENR_ADCEN;
	rcc_ccipr_adcsel_set(3);
}

// The L4's single ADC generates unnumbered (like IWDG) with instance-free
// accessors; the G4 numbers its ADCs and passes the instance.
#define VBAT_ADC ADC
static inline void vbat_adc_sqr1(uint32_t chan) {
	adc_sqr1_l_set(0); // one conversion
	adc_sqr1_sq1_set(chan);
}
