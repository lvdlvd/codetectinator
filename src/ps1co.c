#include "ps1co.h"

// checksum over bytes 1..7: invert the sum, add one (datasheet page 9)
static uint8_t cksum(const uint8_t *f) {
	uint8_t s = 0;
	for (int i = 1; i <= 7; i++) {
		s += f[i];
	}
	return (uint8_t)(~s + 1);
}

void ps1co_poll(struct Serial *tx) {
	static const uint8_t poll[9] = {0xFF, 0x01, 0x86, 0, 0, 0, 0, 0, 0x79};
	fifo_write(&tx->buf, poll, sizeof poll);
	serial_dma_tx_start(tx);
}

void ps1co_ident_poll(struct Serial *tx) {
	// 0x28 = ~(0x01 + 0xD7) + 1
	static const uint8_t poll[9] = {0xFF, 0x01, 0xD7, 0, 0, 0, 0, 0, 0x28};
	fifo_write(&tx->buf, poll, sizeof poll);
	serial_dma_tx_start(tx);
}

void ps1co_feed(struct PS1CO *p, const uint8_t *buf, size_t n) {
	for (size_t i = 0; i < n; i++) {
		uint8_t c = buf[i];
		if (p->idx == 0 && c != 0xFF) {
			continue; // hunt for the start byte
		}
		p->frame[p->idx++] = c;
		if (p->idx < 9) {
			continue;
		}
		p->idx = 0;

		if (p->frame[1] != 0x86 && p->frame[1] != 0xD7) {
			// some other reply (mode switch ack, sleep ack, ...) — the second
			// byte might itself be a start byte, so resync from it
			p->resyncs++;
			if (p->frame[1] == 0xFF) {
				p->frame[0] = 0xFF;
				p->idx = 1;
			}
			continue;
		}
		if (p->frame[8] != cksum(p->frame)) {
			p->cksum_errs++;
			continue;
		}
		if (p->frame[1] == 0xD7) {
			// module information: type, range (big-endian), unit, decimals
			// in bits 7..4 of byte 6 (data sign in bits 3..0)
			p->ident_type = p->frame[2];
			p->ident_range = (uint16_t)((p->frame[3] << 8) | p->frame[4]);
			p->ident_unit = p->frame[5];
			p->ident_decimals = p->frame[6] >> 4;
			p->ident_seen = true;
			continue;
		}
		// 0x86: bytes 6..7: concentration in the module's configured display
		// unit, big-endian, 0.1 ppm steps; bytes 4..5: the full-range field
		// in the same unit (1000 = 100.0 ppm — the live scale cross-check);
		// bytes 2..3: mass concentration (mg/m3-ish, see header)
		p->ppm_x10 = (uint16_t)((p->frame[6] << 8) | p->frame[7]);
		p->mgm3 = (uint16_t)((p->frame[2] << 8) | p->frame[3]);
		p->range86 = (uint16_t)((p->frame[4] << 8) | p->frame[5]);
		for (int i = 0; i < 9; i++) {
			p->last[i] = p->frame[i];
		}
		p->valid = true;
		p->age_s = 0;
		p->frames++;
	}
}
