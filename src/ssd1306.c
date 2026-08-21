#include "ssd1306.h"

#include "font5x7.h"

#include <string.h>

// Send a command byte string via the cmd address (DC low).
static void cmd(struct SSD1306 *d, const uint8_t *c, size_t n) {
	uint8_t buf[8];
	memcpy(buf, c, n);
	spiq_xmit(d->q, d->addr_cmd, n, buf);
}

static void cmd1(struct SSD1306 *d, uint8_t c) { cmd(d, &c, 1); }
static void cmd2(struct SSD1306 *d, uint8_t c, uint8_t a) {
	uint8_t b[2] = {c, a};
	cmd(d, b, 2);
}

void ssd1306_init(struct SSD1306 *d) {
	static const uint8_t seq[] = {
	    0xAE,       // display off
	    0xD5, 0x80, // clock divide: reset default
	    0xA8, SSD1306_H - 1, // multiplex ratio = panel rows
	    0xD3, 0x00, // display offset 0
	    0x40,       // start line 0
	    0x8D, 0x14, // charge pump on
	    0x20, 0x00, // horizontal addressing
	    0xA1,       // segment remap (col 127 -> SEG0 mirrored)
	    0xC8,       // COM scan reversed
	    // COM pins: 64-row panels wire COMs alternating, 32-row sequential
	    0xDA, SSD1306_H == 64 ? 0x12 : 0x02,
	    0x81, 0xCF, // contrast
	    0xD9, 0xF1, // precharge 15/1
	    0xDB, 0x40, // VCOMH deselect
	    0xA4,       // resume from RAM
	    0xA6,       // normal (not inverted)
	};
	for (size_t i = 0; i < sizeof seq;) {
		// two-byte commands are exactly those with an argument in the list above
		uint8_t c = seq[i];
		int two = (c == 0xD5 || c == 0xA8 || c == 0xD3 || c == 0x8D || c == 0x20 ||
		           c == 0xDA || c == 0x81 || c == 0xD9 || c == 0xDB);
		cmd(d, &seq[i], two ? 2 : 1);
		i += two ? 2 : 1;
	}
	fb_clear(d);
	ssd1306_flush(d);
	cmd1(d, 0xAF); // display on
}

void ssd1306_flush(struct SSD1306 *d) {
	static const uint8_t window[] = {
	    0x21, 0, SSD1306_W - 1, // column window
	    0x22, 0, SSD1306_H / 8 - 1, // page window
	};
	cmd(d, window, 3);
	cmd(d, window + 3, 3);
	for (size_t off = 0; off < sizeof d->fb; off += SPI_XMIT_BUF) {
		uint8_t buf[SPI_XMIT_BUF];
		memcpy(buf, d->fb + off, SPI_XMIT_BUF);
		spiq_xmit(d->q, d->addr_data, SPI_XMIT_BUF, buf);
	}
}

void ssd1306_on(struct SSD1306 *d, bool on) { cmd1(d, on ? 0xAF : 0xAE); }
void ssd1306_test(struct SSD1306 *d, bool all) { cmd1(d, all ? 0xA5 : 0xA4); }
void ssd1306_invert(struct SSD1306 *d, bool inv) { cmd1(d, inv ? 0xA7 : 0xA6); }
void ssd1306_contrast(struct SSD1306 *d, uint8_t v) { cmd2(d, 0x81, v); }

void ssd1306_dim(struct SSD1306 *d, bool dim) {
	cmd2(d, 0xD9, dim ? 0x11 : 0xF1); // precharge
	cmd2(d, 0xDB, dim ? 0x00 : 0x40); // VCOMH deselect level
	cmd2(d, 0x81, dim ? 0x00 : 0xCF); // contrast
}

// ---- framebuffer ------------------------------------------------------------

void fb_clear(struct SSD1306 *d) { memset(d->fb, 0, sizeof d->fb); }

void fb_pixel(struct SSD1306 *d, int x, int y) {
	if (x < 0 || x >= SSD1306_W || y < 0 || y >= SSD1306_H) {
		return;
	}
	d->fb[(y >> 3) * SSD1306_W + x] |= 1u << (y & 7);
}

void fb_vline(struct SSD1306 *d, int x, int y0, int y1) {
	if (y1 < y0) {
		int t = y0;
		y0 = y1;
		y1 = t;
	}
	for (int y = y0; y <= y1; y++) {
		fb_pixel(d, x, y);
	}
}

void fb_hline(struct SSD1306 *d, int x0, int x1, int y) {
	if (x1 < x0) {
		int t = x0;
		x0 = x1;
		x1 = t;
	}
	for (int x = x0; x <= x1; x++) {
		fb_pixel(d, x, y);
	}
}

void fb_rect_fill(struct SSD1306 *d, int x0, int y0, int x1, int y1) {
	for (int x = x0; x <= x1; x++) {
		fb_vline(d, x, y0, y1);
	}
}

void fb_invert_rect(struct SSD1306 *d, int x0, int y0, int x1, int y1) {
	for (int y = y0; y <= y1; y++) {
		if (y < 0 || y >= SSD1306_H) {
			continue;
		}
		for (int x = x0; x <= x1; x++) {
			if (x < 0 || x >= SSD1306_W) {
				continue;
			}
			d->fb[(y >> 3) * SSD1306_W + x] ^= 1u << (y & 7);
		}
	}
}

// glyph pixel with out-of-bounds = blank, for the Scale3x neighborhood
static bool fpx(const uint8_t *g, int col, int row) {
	return col >= 0 && col < 5 && row >= 0 && row < 7 && ((g[col] >> row) & 1);
}

int fb_text_big(struct SSD1306 *d, int x, int y, const char *s) {
	for (; *s; s++) {
		unsigned ch = (unsigned char)*s;
		if (ch < 0x20 || ch > 0x7E) {
			ch = '?';
		}
		const uint8_t *g = font5x7[ch - 0x20];
		for (int col = 0; col < 5; col++) {
			for (int row = 0; row < 7; row++) {
				// Scale3x: expand E to a 3x3 block, pulling in diagonal
				// neighbors to round corners (the classic pixel-art rule)
				bool A = fpx(g, col - 1, row - 1), B = fpx(g, col, row - 1),
				     C = fpx(g, col + 1, row - 1);
				bool D = fpx(g, col - 1, row), E = fpx(g, col, row), F = fpx(g, col + 1, row);
				bool G = fpx(g, col - 1, row + 1), H = fpx(g, col, row + 1),
				     I = fpx(g, col + 1, row + 1);
				bool e[9];
				if (B != H && D != F) {
					e[0] = D == B ? D : E;
					e[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
					e[2] = B == F ? F : E;
					e[3] = (D == B && E != G) || (D == H && E != A) ? D : E;
					e[4] = E;
					e[5] = (B == F && E != I) || (H == F && E != C) ? F : E;
					e[6] = D == H ? D : E;
					e[7] = (D == H && E != I) || (H == F && E != G) ? H : E;
					e[8] = H == F ? F : E;
				} else {
					for (int k = 0; k < 9; k++) {
						e[k] = E;
					}
				}
				for (int k = 0; k < 9; k++) {
					if (e[k]) {
						fb_pixel(d, x + col * 3 + k % 3, y + row * 3 + k / 3);
					}
				}
			}
		}
		x += 18;
	}
	return x;
}

int fb_text(struct SSD1306 *d, int x, int y, int scale, const char *s) {
	for (; *s; s++) {
		unsigned c = (unsigned char)*s;
		if (c < 0x20 || c > 0x7E) {
			c = '?';
		}
		const uint8_t *glyph = font5x7[c - 0x20];
		for (int col = 0; col < 5; col++) {
			uint8_t bits = glyph[col];
			for (int row = 0; row < 7; row++) {
				if (!(bits & (1u << row))) {
					continue;
				}
				for (int sx = 0; sx < scale; sx++) {
					for (int sy = 0; sy < scale; sy++) {
						fb_pixel(d, x + col * scale + sx, y + row * scale + sy);
					}
				}
			}
		}
		x += 6 * scale;
	}
	return x;
}
