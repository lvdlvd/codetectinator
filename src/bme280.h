#pragma once

// Bosch BME280 pressure/temperature/humidity sensor over the n-array SPI
// transaction queue (4-wire SPI mode, mode 0). Board-independent: addressed
// through one SPIQ slave-select address. All transfers are synchronous
// spiq_xmit.
//
// Usage model: bme280_init once (ID check + calibration read + config), then
// once per second bme280_read (result of the previous forced conversion)
// followed by bme280_trigger (start the next one). Forced mode with 1x
// oversampling and no IIR filter — the "weather monitoring" preset of the
// datasheet; a conversion takes ~8 ms, so a 1 s cadence never reads a busy
// sensor.

#include "bme280_comp.h"
#include "spi.h"

#include <stdbool.h>
#include <stdint.h>

struct BME280 {
	struct SPIQ *q;
	uint16_t addr;
	struct BME280Calib cal; // read once at init (datasheet 4.2.2)
	bool ok;                // ID matched at init
};

// Probe (chip ID 0x60), read calibration, configure. Returns false if the
// sensor does not answer — reads then return false too, the device just
// shows no P/T/H.
bool bme280_init(struct BME280 *b, struct SPIQ *q, uint16_t addr);

// Start a forced-mode conversion.
void bme280_trigger(struct BME280 *b);

// Read and compensate the last conversion. t_centi in 0.01 degC, p_pa in Pa,
// rh_deci in 0.1 %RH. Returns false if the sensor is absent or the sample is
// not plausible (all-ones ADC values = no conversion yet).
bool bme280_read(struct BME280 *b, int32_t *t_centi, uint32_t *p_pa, uint32_t *rh_deci);
