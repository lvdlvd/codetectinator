#pragma once

// Board definition: NUCLEO-G431KB (the original codetectinator target).
// Selected by the Makefile (BOARD=g431), which copies it to board.h —
// run `make clean` when switching boards so the generated device header
// and linker scripts regenerate too.

#include "clock.h"    // G4 clock lib (clock.c must be in CSRC)
#include "dma_g4.h"   // DMAMUX request routing
#include "usart_v3.h" // usart_init* for the G4 FIFO IP

#define BOARD_NAME "NUCLEO-G431KB"

// Role pins. LED = LD2 on PB8 (BOOT0 pin: output-only use).
// BUTTON moved from PA0/A0 to PA4/A3 for breadboard parity with the
// L432KC, whose PA0 carries the ST-LINK MCO (SB4). Costs the WKUP1
// option here; Stop-mode EXTI wake works from PA4 all the same.
static const enum GPIO_Pin LED = PB8;
static const enum GPIO_Pin BUTTON = PA4;   // to GND, internal pull-up
static const enum GPIO_Pin OLED_CS = PA11; // active low
static const enum GPIO_Pin OLED_DC = PA12; // low = command, high = data
static const enum GPIO_Pin OLED_RST = PB0; // active low
static const enum GPIO_Pin BME_CS = PA8;   // active low

// Battery divider input: PA1 = ADC1_IN2 on the G4.
enum { BOARD_VBAT_ADC_CHAN = 2 };

// DMA channels: free choice through the DMAMUX. #defines (not an enum) so
// they keep the enum DMA_CHAN type and remain vector-table constants.
#define CH_CONSOLE_TX DMA1_CH1
#define CH_CO_RX DMA1_CH2
#define CH_CO_TX DMA1_CH3
#define CH_SPI_RX DMA1_CH4
#define CH_SPI_TX DMA1_CH5
#define CH_CONSOLE_RX DMA1_CH6
// DMA1_CHn IRQ numbers are consecutive from DMA1_CH1_IRQn on G4 and L4 alike.
#define DMA_CH_IRQN(ch) ((enum IRQn_Type)(DMA1_CH1_IRQn + (ch)))

// The board pinout (run `make pinfmt` to annotate).
static const pinconf_t board[] = {
	// Unused -> analog (low power); PAMost keeps SWDIO/SWCLK alive. PA1 is
	// the battery divider input and stays analog by this default too.
	PAMost | PIN_ANALOG,
	PBAll | PIN_ANALOG,
	PA4 | PIN_INPUT | PIN_PULLUP, //% pushbutton to GND (A3)
	PA8 | PIN_OUTPUT | PIN_HIGH,  //% BME280 /CS
	PA9_USART1_TX | PIN_HIGH,     //% CO module RX (yellow)
	PA10_USART1_RX | PIN_PULLUP,  //% CO module TX (green)
	PA11 | PIN_OUTPUT | PIN_HIGH, //% OLED /CS
	PA12 | PIN_OUTPUT,            //% OLED D/C
	PA2_USART2_TX | PIN_HIGH,     //% ST-LINK VCP (debug console TX)
	PA3_USART2_RX | PIN_PULLUP,   //% ST-LINK VCP (debug console RX: bench keys)
	PB0 | PIN_OUTPUT,             //% OLED /RST, held low until released in main
	// PULLDOWN: spiq idles with SPE=0, which tri-states SCK — hold it at the
	// mode-0 idle level instead of floating at logic threshold
	PB3_SPI1_SCK | PIN_PULLDOWN,  //% OLED D0 + BME280 SCK
	PB4_SPI1_MISO,                //% BME280 SDO
	PB5_SPI1_MOSI,                //% OLED D1 + BME280 SDI
	PB8 | PIN_OUTPUT,             //% LD2 user LED
};

static inline void board_clocks(void) {
	RCC.AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;
	RCC.AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN;
	RCC.APB1ENR1 |= RCC_APB1ENR1_USART2EN;
	RCC.APB2ENR |= RCC_APB2ENR_USART1EN | RCC_APB2ENR_SPI1EN;
}

// ADC clock + kernel source: G4 names, SYSCLK kernel.
static inline void board_adc_clock(void) {
	RCC.AHB2ENR |= RCC_AHB2ENR_ADC12EN;
	rcc_ccipr_adc12sel_set(2); // kernel clock = SYSCLK
}

// The G4 numbers its ADCs (ADC1..ADC5) and its accessors take the instance;
// the L4's single ADC generates unnumbered with instance-free accessors.
#define VBAT_ADC ADC1
static inline void vbat_adc_sqr1(uint32_t chan) {
	adc_sqr1_l_set(&ADC1, 0); // one conversion
	adc_sqr1_sq1_set(&ADC1, chan);
}
