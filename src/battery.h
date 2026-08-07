#pragma once

// Battery voltage via the 3:1 divider on PA1 (ADC1_IN2). See battery.c.

#include <stdint.h>

void battery_init(void);
uint32_t battery_read_mv(void); // battery millivolts; 0 = ADC not working
