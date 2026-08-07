// Codetectinator — portable CO detector on a Nucleo-32 board (see
// ../README.md; board_g431.h / board_l432.h via the Makefile BOARD switch).
//
// PS1-CO-100-MOD CO module on USART1 (PA9/PA10, 9600 8N1, polled Q&A),
// BME280 + SSD1306 128x64 OLED sharing SPI1 (PB3/PB5/PB4, mode 0, 8 MHz)
// with per-device chip selects, battery divider on PA1/ADC1, pushbutton on
// PA0, debug console on USART2 -> ST-LINK VCP at 460800.
//
// Console keys: 'd' toggles a live ASCII mirror of the display framebuffer
// (1 char = 1 pixel, needs a 128+ column terminal; \e[H-style in-place
// refresh), 'b' acts as the pushbutton — both so the whole device can be
// exercised over the VCP before the OLED (or the button) is even wired.
//
// Runs at 16 MHz straight from HSI16 (no PLL): lowest-power preset, ample
// for a 1 Hz sample cadence and a 4 fps display.

#include "device.h" // generated for the selected board's part
#include "pinmux.h"

#include "console.h" // pulls serial.h + tprintf.h
#include "fault.h"
#include "gpio.h"
#include "nvic.h"
#include "spi.h"
#include "startup.h"

// board.h is a Makefile-generated copy of board_g431.h / board_l432.h: the
// family lib includes (clock/dma/usart), role pins, the pin table, the DMA
// channel plan, and the board_clocks()/board_adc_clock() helpers.
#include "board.h"

#include <string.h>

#include "battery.h"
#include "bme280.h"
#include "history.h"
#include "ps1co.h"
#include "ssd1306.h"
#include "ui.h"

#ifndef __REVISION__
#define __REVISION__ 0
#endif

extern const isr_t __vectors[]; // the vector table, defined at the foot of the file

// ---- serial ----------------------------------------------------------------

// 460800: the fastest rate the 16 MHz kernel hits within tolerance at plain
// 16x oversampling (BRR 35 -> -0.79%; the ST-LINK side is exact). 921600
// needs OVER8, whose receiver proved silently deaf on the L432 (TX fine,
// RX never samples a frame; config verified register-by-register on the
// bench 2026-08-07 — suspected IP quirk, errata sheet wanted). The mirror
// still streams ~5 frames/s at this rate.
enum { CONSOLE_BAUD = 460800 };

static uint8_t console_buf[1024]; // power-of-two rings
static uint8_t console_rxbuf[64];
static struct Serial console_tx = SERIAL_INITIALIZER(USART2, console_buf);
static struct Serial console_rx = SERIAL_INITIALIZER(USART2, console_rxbuf);

static uint8_t co_txbuf[64];
static uint8_t co_rxbuf[256];
static struct Serial co_tx = SERIAL_INITIALIZER(USART1, co_txbuf);
static struct Serial co_rx = SERIAL_INITIALIZER(USART1, co_rxbuf);
static struct SerialRXCounters co_rxc;

// Polled/blocking console putc for the boot-time crash report (fault.h).
static void cputc(char c) { usart_putc(&USART2, c); }

// ---- SPI bus: OLED (cmd/data) + BME280 -------------------------------------

enum { SPI_OLED_CMD, SPI_OLED_DATA, SPI_BME };

static struct SPIQ spiq;

static void ss_hook(struct SPI_Type *spi, uint16_t addr, int on) {
	(void)spi;
	if (on) {
		switch (addr) {
		case SPI_OLED_CMD:
			digitalLo(OLED_DC);
			digitalLo(OLED_CS);
			break;
		case SPI_OLED_DATA:
			digitalHi(OLED_DC);
			digitalLo(OLED_CS);
			break;
		case SPI_BME:
			digitalLo(BME_CS);
			break;
		}
	} else {
		digitalHi(OLED_CS);
		digitalHi(BME_CS);
	}
}

// ---- devices ---------------------------------------------------------------

static struct SSD1306 oled = {.q = &spiq, .addr_cmd = SPI_OLED_CMD, .addr_data = SPI_OLED_DATA};
static struct BME280 bme;
static struct PS1CO ps1;

// ---- timebase --------------------------------------------------------------

static volatile uint32_t ms; // SysTick millisecond counter, wraps in 49 days

static void delay_ms(uint32_t d) {
	for (uint32_t t0 = ms; ms - t0 < d;) {
		__asm volatile("wfi");
	}
}

// ---- 1 Hz sampling ---------------------------------------------------------

static uint32_t vbat_mv;
static uint32_t seconds;
static struct UIData latest = {HIST_NONE, HIST_NONE, HIST_NONE, HIST_NONE, HIST_NONE, 0};

static void one_second(void) {
	struct UIData d = {HIST_NONE, HIST_NONE, HIST_NONE, HIST_NONE, HIST_NONE, 0};

	// read the conversion triggered a second ago, start the next one
	int32_t t_centi;
	uint32_t p_pa, rh_deci;
	if (bme280_read(&bme, &t_centi, &p_pa, &rh_deci)) {
		d.p = (int16_t)(p_pa / 10); // Pa -> 0.1 hPa
		d.t = (int16_t)t_centi;     // 0.01 degC
		d.h = (int16_t)rh_deci;     // 0.1 %RH
	}
	bme280_trigger(&bme);

	// CO: use the last good frame if it is fresh; poll for the next one
	if (ps1.valid && ps1.age_s < 5) {
		d.co = ps1.ppm_x10 > INT16_MAX ? INT16_MAX : (int16_t)ps1.ppm_x10;
	}
	ps1.age_s++;
	ps1co_poll(&co_tx);

	// module identification. Two mechanisms: the 0xD7 info command (some
	// module firmwares ignore it — give up after a minute), and the
	// full-range field that rides in EVERY 0x86 reply (1000 = 100.0 ppm
	// confirms the 0.1 ppm scale; a wrong-range module or unit config
	// would silently rescale every reading — make it loud instead).
	static bool ident_reported, range_reported;
	if (!ps1.ident_seen && seconds < 60 && seconds % 5 == 2) {
		ps1co_ident_poll(&co_tx);
	}
	if (ps1.ident_seen && !ident_reported) {
		ident_reported = true;
		tprintf("CO module: type %x range %u unit %x decimals %u — %s\n", ps1.ident_type,
		        ps1.ident_range, ps1.ident_unit, ps1.ident_decimals,
		        ps1co_ident_ok(&ps1) ? "PS1-CO-100 ok" : "MISMATCH, expected CO/ppm/1dp");
	}
	if (ps1.valid && !range_reported) {
		range_reported = true;
		tprintf("CO 0x86 full-range %u — %s\n", ps1.range86,
		        ps1co_range_ok(&ps1) ? "0.1 ppm scale confirmed"
		                             : "UNEXPECTED, readings may be rescaled");
	}

	if (seconds % 5 == 0) {
		vbat_mv = battery_read_mv();
	}
	d.vbat_mv = vbat_mv;
	if (vbat_mv > 0) {
		d.bat = (int16_t)(vbat_mv / 10); // 0.01 V units for the BAT tab/graph
	}

	history_push(HIST_P, d.p);
	history_push(HIST_T, d.t);
	history_push(HIST_H, d.h);
	history_push(HIST_CO, d.co);
	if (seconds % history_period_s[HIST_BAT] == 0) {
		history_push(HIST_BAT, d.bat); // slow channel: ~6 days of ring
	}
	ui_second(&d);
	latest = d;
	seconds++;
}

// ---- console display mirror ------------------------------------------------

// Live ASCII rendering of the OLED framebuffer: \e[H home, 64 lines of 128
// '#'/' ' chars (1 char = 1 pixel) each ended with \e[K, then two status
// lines. Emitted cooperatively from the main loop as TX-fifo space frees up,
// so a 8.6K frame streams without blocking or overflowing the 1K fifo.
static bool mirror;          // toggled by 'd'
static int mirror_row = -1;  // -1 idle, 0..SSD1306_H+1 = next line to emit
static uint32_t next_mirror;

static const char *fmt_or_dashes(char *buf, int16_t v_x10) {
	return v_x10 == HIST_NONE ? "--" : ui_fmt1(buf, v_x10);
}

static void mirror_pump(uint32_t now) {
	if (mirror_row < 0) {
		if (!mirror || (int32_t)(now - next_mirror) < 0) {
			return;
		}
		next_mirror = now + 250;
		mirror_row = 0;
		fifo_write(&console_tx.buf, "\e[H", 3);
	}
	// one framebuffer line (or status line) per iteration, only when it fits
	while (mirror_row >= 0 && fifo_free(&console_tx.buf) >= SSD1306_W + 8) {
		if (mirror_row < SSD1306_H) {
			uint8_t line[SSD1306_W + 5];
			for (int x = 0; x < SSD1306_W; x++) {
				line[x] = fb_get(&oled, x, mirror_row) ? '#' : ' ';
			}
			memcpy(line + SSD1306_W, "\e[K\r\n", 5);
			fifo_write(&console_tx.buf, line, sizeof line);
			mirror_row++;
		} else if (mirror_row == SSD1306_H) {
			char b1[8], b2[8], b3[8], b4[8], b5[8];
			serial_printf(&console_tx, "P %s hPa  T %s C  H %s %%  CO %s ppm  bat %s V  alarm %d\e[K\r\n",
			              fmt_or_dashes(b1, latest.p),
			              fmt_or_dashes(b2, latest.t == HIST_NONE ? HIST_NONE : latest.t / 10),
			              fmt_or_dashes(b3, latest.h), fmt_or_dashes(b4, latest.co),
			              ui_fmt1(b5, (int)(latest.vbat_mv / 100)), ui_alarm_level());
			mirror_row++;
		} else {
			serial_printf(&console_tx,
			              "co ppmf %u mgf %u range %u [%02x %02x %02x %02x %02x %02x %02x %02x %02x] "
			              "frames %u cksum %u age %u\e[J",
			              ps1.ppm_x10, ps1.mgm3, ps1.range86, ps1.last[0], ps1.last[1],
			              ps1.last[2], ps1.last[3], ps1.last[4], ps1.last[5], ps1.last[6],
			              ps1.last[7], ps1.last[8], (unsigned)ps1.frames,
			              (unsigned)ps1.cksum_errs, (unsigned)ps1.age_s);
			mirror_row = -1;
		}
	}
	serial_dma_tx_start(&console_tx);
}

void Reset_Handler(void) __attribute__((noreturn));
void Reset_Handler(void) {
	narray_init_memory();
	SCB.VTOR = (uint32_t)(uintptr_t)__vectors;
	*(volatile uint32_t *)0xE000ED88 |= 0xfu << 20; // FPU: CP10/CP11 full access
	SCB.SHCSR |= SCB_SHCSR_USGFAULTENA;             // route usage faults to our handler
	SCB.CCR |= SCB_CCR_DIV_0_TRP;                   // div-by-zero -> UsageFault

	clock_init_16(); // HSI16 direct, no PLL

	board_clocks(); // peripheral clock enables (family-specific set)
	gpioConfigAll(board, sizeof board / sizeof board[0]);

	// SysTick at 1 kHz
	STK.LOAD = clock_sysclk_hz() / 1000 - 1;
	STK.VAL = 0;
	STK.CTRL = STK_CTRL_ENABLE | STK_CTRL_TICKINT | STK_CTRL_CLKSOURCE_AHB;

	// console on USART2 -> ST-LINK VCP, full duplex
	dma_set_mux(CH_CONSOLE_TX, DMA_REQ_USART2_TX);
	dma_set_mux(CH_CONSOLE_RX, DMA_REQ_USART2_RX);
	usart_init(&USART2, clock_usart_hz(2), CONSOLE_BAUD);
	// usart_brr truncates; at 460800 the rounded divider is the in-tolerance
	// one (35 -> -0.79%, vs 34 -> +2.1%), so round explicitly
	{
		uint32_t div = (clock_usart_hz(2) + CONSOLE_BAUD / 2) / CONSOLE_BAUD;
		USART2.CR1 &= ~USART_CR1_UE;
		USART2.BRR = div;
		USART2.CR1 |= USART_CR1_UE;
	}
	console = &console_tx;
	serial_dma_rx_start(&console_rx, CH_CONSOLE_RX);
	nvic_enable(DMA_CH_IRQN(CH_CONSOLE_TX));
	nvic_enable(DMA_CH_IRQN(CH_CONSOLE_RX));
	nvic_enable(USART2_IRQn);

	// CO module on USART1, full duplex 9600 8N1
	dma_set_mux(CH_CO_RX, DMA_REQ_USART1_RX);
	dma_set_mux(CH_CO_TX, DMA_REQ_USART1_TX);
	usart_init(&USART1, clock_usart_hz(1), 9600);
	serial_dma_rx_start(&co_rx, CH_CO_RX);
	nvic_enable(DMA_CH_IRQN(CH_CO_RX));
	nvic_enable(DMA_CH_IRQN(CH_CO_TX));
	nvic_enable(USART1_IRQn);

	// SPI1 at 8 MHz, mode 0, software slave select via ss_hook
	dma_set_mux(CH_SPI_RX, DMA_REQ_SPI1_RX);
	dma_set_mux(CH_SPI_TX, DMA_REQ_SPI1_TX);
	spiq_init(&spiq, &SPI1, SPI_CR1_BR_Div2, CH_SPI_RX, CH_SPI_TX, ss_hook);
	nvic_enable(DMA_CH_IRQN(CH_SPI_RX));

	fault_report(cputc); // print a crash from the previous run, if any
	tprintf("\ncodetectinator %x on " BOARD_NAME ", sysclk = %u Hz\n", __REVISION__,
	        (unsigned)clock_sysclk_hz());

	battery_init();

	// OLED reset: PB0 configured low above; release after a settle delay
	delay_ms(10);
	digitalHi(OLED_RST);
	delay_ms(10);
	ssd1306_init(&oled);
	ui_init(&oled);

	if (!bme280_init(&bme, &spiq, SPI_BME)) {
		tprintf("BME280: no answer\n");
	}
	bme280_trigger(&bme);

	uint32_t next_second = ms + 1000;
	uint32_t next_button = 0;
	uint32_t next_report = 10000;
	uint8_t btn_stable = 0, btn_cnt = 0;

	for (;;) {
		__asm volatile("wfi");
		uint32_t now = ms;

		// feed the CO parser whatever arrived
		uint8_t chunk[32];
		for (size_t n; (n = fifo_read(&co_rx.buf, chunk, sizeof chunk)) > 0;) {
			ps1co_feed(&ps1, chunk, n);
		}

		// console keys
		for (uint8_t key; fifo_read(&console_rx.buf, &key, 1) > 0;) {
			switch (key) {
			case 'd': // toggle the display mirror
				mirror = !mirror;
				mirror_row = -1;
				next_mirror = now;
				tprintf("\e[2J\e[H");
				break;
			case 'b': // act as the pushbutton
				ui_button(now);
				break;
			default:
				tprintf("key %02x\n", key); // unknown key: ack it, helps bench debugging
				break;
			}
		}

		// button: sample at 100 Hz, 3 stable samples = edge
		if ((int32_t)(now - next_button) >= 0) {
			next_button = now + 10;
			uint8_t raw = !digitalIn(BUTTON); // active low
			if (raw != btn_stable) {
				if (++btn_cnt >= 3) {
					btn_stable = raw;
					btn_cnt = 0;
					if (raw) {
						ui_button(now);
					}
				}
			} else {
				btn_cnt = 0;
			}
		}

		if ((int32_t)(now - next_second) >= 0) {
			next_second += 1000;
			one_second();
		}

		ui_tick(now);
		mirror_pump(now);

		// LED: fast blink on alarm, short heartbeat blip otherwise
		if (ui_alarm_level() > 0) {
			((now / 250) & 1) ? digitalHi(LED) : digitalLo(LED);
		} else {
			(now % 2000 < 20) ? digitalHi(LED) : digitalLo(LED);
		}

		if (!mirror && (int32_t)(now - next_report) >= 0) {
			next_report += 10000;
			char b1[8], b2[8], b3[8], b4[8];
			tprintf("t %u P %s T %s H %s CO %s (raw %u) age %u frames %u cksum %u "
			        "resync %u rxovfl %u bme %s vbat %u mV\n",
			        (unsigned)seconds, fmt_or_dashes(b1, latest.p),
			        fmt_or_dashes(b2, latest.t == HIST_NONE ? HIST_NONE : latest.t / 10),
			        fmt_or_dashes(b3, latest.h), fmt_or_dashes(b4, latest.co), ps1.ppm_x10,
			        (unsigned)ps1.age_s, (unsigned)ps1.frames, (unsigned)ps1.cksum_errs,
			        (unsigned)ps1.resyncs, (unsigned)co_rx.ovfl_count, bme.ok ? "ok" : "DEAD",
			        (unsigned)vbat_mv);
		}
	}
}

// Serial and SPI interrupts (the app owns the vectors, lib handlers do the
// work). Channel roles come from board.h.
static void dma_console_tx(void) { serial_dma_tx_handler(&console_tx, CH_CONSOLE_TX); }
static void dma_console_rx(void) { serial_dma_rx_handler(&console_rx, CH_CONSOLE_RX); }
static void dma_co_rx(void) { serial_dma_rx_handler(&co_rx, CH_CO_RX); }
static void dma_co_tx(void) { serial_dma_tx_handler(&co_tx, CH_CO_TX); }
static void dma_spi_rx(void) { spi_rx_dma_handler(&spiq); }
static void usart1(void) {
	serial_irq_tx_handler(&co_tx, CH_CO_TX);
	serial_irq_rx_handler(&co_rx, CH_CO_RX, &co_rxc);
}
static void usart2(void) {
	serial_irq_tx_handler(&console_tx, CH_CONSOLE_TX);
	serial_irq_rx_handler(&console_rx, CH_CONSOLE_RX, NULL);
}
static void systick(void) { ++ms; }

extern void _estack(void); // top of stack (linker)

// The vector table IS the program: slot 0 = SP, 1 = Reset (positional), the
// core fault handlers at their fixed slots, device IRQs by VECTOR(IRQn).
__attribute__((section(".isr_vector"))) const isr_t __vectors[NVIC_VECTORS] = {
	(isr_t)&_estack, // [0] SP    — positional
	Reset_Handler,   // [1] Reset — positional
	[VECTOR(HardFault_IRQn)] = HardFault_Handler,
	[VECTOR(MemManage_IRQn)] = MemManage_Handler,
	[VECTOR(BusFault_IRQn)] = BusFault_Handler,
	[VECTOR(UsageFault_IRQn)] = UsageFault_Handler,
	[VECTOR(SysTick_IRQn)] = systick,
	[VECTOR(DMA_CH_IRQN(CH_CONSOLE_TX))] = dma_console_tx,
	[VECTOR(DMA_CH_IRQN(CH_CONSOLE_RX))] = dma_console_rx,
	[VECTOR(DMA_CH_IRQN(CH_CO_RX))] = dma_co_rx,
	[VECTOR(DMA_CH_IRQN(CH_CO_TX))] = dma_co_tx,
	[VECTOR(DMA_CH_IRQN(CH_SPI_RX))] = dma_spi_rx,
	[VECTOR(USART1_IRQn)] = usart1,
	[VECTOR(USART2_IRQn)] = usart2,
};
