#pragma once

// SSD1306 128x64 OLED over the n-array SPI transaction queue (4-wire SPI:
// SCK/MOSI/CS/DC, plus a reset GPIO owned by the board code). The driver is
// board-independent: it addresses the display through two SPIQ slave-select
// addresses — one the board's ss hook decodes as "CS low, DC low" (commands),
// one as "CS low, DC high" (data). All transfers are synchronous spiq_xmit;
// keep the queue free of async traffic around calls into this driver.
//
// The framebuffer is in SSD1306 page layout: fb[page*128 + x], bit 0 the top
// row of the page. Drawing primitives clip to the 128x64 canvas.

#include "spi.h"

#include <stdbool.h>
#include <stdint.h>

enum { SSD1306_W = 128, SSD1306_H = 64 };

struct SSD1306 {
	struct SPIQ *q;
	uint16_t addr_cmd;  // ss-hook address: CS asserted, DC low
	uint16_t addr_data; // ss-hook address: CS asserted, DC high
	uint8_t fb[SSD1306_W * SSD1306_H / 8];
};

// Init sequence for a 128x64 panel with the internal charge pump. The caller
// must have released the panel's reset line (high) at least 3 us before.
void ssd1306_init(struct SSD1306 *d);

// Send the whole framebuffer (horizontal addressing, one pass).
void ssd1306_flush(struct SSD1306 *d);

void ssd1306_on(struct SSD1306 *d, bool on);          // 0xAF / 0xAE (sleep, RAM retained)
void ssd1306_invert(struct SSD1306 *d, bool inv);     // 0xA7 / 0xA6
void ssd1306_contrast(struct SSD1306 *d, uint8_t v);  // 0x81 <v>
// Dim beyond what contrast alone allows: shortens precharge and drops VCOMH.
void ssd1306_dim(struct SSD1306 *d, bool dim);

// Framebuffer drawing (no transfer until ssd1306_flush).
void fb_clear(struct SSD1306 *d);
void fb_pixel(struct SSD1306 *d, int x, int y);
static inline bool fb_get(const struct SSD1306 *d, int x, int y) {
	return (d->fb[(y >> 3) * SSD1306_W + x] >> (y & 7)) & 1;
}
void fb_vline(struct SSD1306 *d, int x, int y0, int y1);
void fb_hline(struct SSD1306 *d, int x0, int x1, int y);
void fb_rect_fill(struct SSD1306 *d, int x0, int y0, int x1, int y1);
void fb_invert_rect(struct SSD1306 *d, int x0, int y0, int x1, int y1);
// 5x7 font scaled by an integer factor; returns the x after the last glyph.
int fb_text(struct SSD1306 *d, int x, int y, int scale, const char *s);
static inline int fb_text_width(int scale, const char *s) {
	int n = 0;
	while (s[n]) {
		n++;
	}
	return n * 6 * scale;
}

// Large text: the 5x7 font rendered through Scale3x pixel-art smoothing —
// 15x21 px glyphs (18 px advance) with rounded corners instead of 3x blocks.
// No font data beyond the 5x7 table. Returns the x after the last glyph.
int fb_text_big(struct SSD1306 *d, int x, int y, const char *s);
static inline int fb_text_big_width(const char *s) { return fb_text_width(3, s); }
