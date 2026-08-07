#pragma once

// SGX PS1-CO-100-MOD carbon monoxide module, UART 9600 8N1 (DS-0473).
//
// The module powers up in Q&A mode: we send the 9-byte "read concentration"
// command (0x86) once a second and parse the 9-byte reply. The same frame
// layout arrives unsolicited if the module is ever switched to active-upload
// mode, so the parser accepts both.
//
// Frame (both directions): FF <cmd/retain> ... <checksum>, checksum over
// bytes 1..7: invert the byte sum, add one.
//
// Concentration scale: the module reports in the unit/decimal-places
// configuration of command 0xD7 (unit = ppm, 1 decimal for this part), so
// the 16-bit concentration field in bytes 6..7 is in 0.1 ppm steps and the
// full-range field (bytes 4..5) reads 1000 = 100.0 ppm. The datasheet's
// examples are sloppy about this — verify against the bench (frcat-style
// raw dump on the console) on first power-up.

#include "serial.h"

#include <stdbool.h>
#include <stdint.h>

struct PS1CO {
	// parser state
	uint8_t frame[9];
	int idx;

	// last good reading
	uint16_t ppm_x10; // CO concentration field (bytes 6..7), 0.1 ppm units
	uint16_t mgm3;    // the mass-concentration field (bytes 2..3), unit per
	                  // the module's config — the ppm/mg ratio (CO: 1 ppm =
	                  // 1.145 mg/m3 at 25 C) discriminates the scale
	uint16_t range86; // full-range field (bytes 4..5): 1000 = 100.0 ppm if
	                  // the 0.1 ppm interpretation holds
	uint8_t last[9];  // the raw frame, for eyeballing on the bench
	bool valid;       // at least one good frame seen
	uint32_t age_s;   // seconds since the last good frame (caller-bumped)

	// module identification (0xD7 reply): sensor type 0x23 = CO, range in
	// the module's display unit (1000 = 100.0 ppm confirms the 0.1 ppm
	// scale), unit code (0x02 = ppm & mg/m3), decimal places
	bool ident_seen;
	uint8_t ident_type, ident_unit, ident_decimals;
	uint16_t ident_range;

	// health counters
	uint32_t frames, cksum_errs, resyncs;
};

// The module identifies as the expected PS1-CO-100 (CO, ppm, 1 decimal).
static inline bool ps1co_ident_ok(const struct PS1CO *p) {
	return p->ident_seen && p->ident_type == 0x23 && p->ident_unit == 0x02 &&
	       p->ident_decimals == 1;
}

// The per-frame scale check: full range 1000 = 100.0 ppm in 0.1 ppm units.
static inline bool ps1co_range_ok(const struct PS1CO *p) {
	return p->valid && p->range86 == 1000;
}

// Queue the Q&A poll command on the CO module's TX serial.
void ps1co_poll(struct Serial *tx);

// Queue the 0xD7 module-information request.
void ps1co_ident_poll(struct Serial *tx);

// Feed received bytes to the parser; updates the reading on a good frame.
void ps1co_feed(struct PS1CO *p, const uint8_t *buf, size_t n);
