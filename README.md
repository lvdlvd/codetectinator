# The Codedectinator is a portable Carbonmonoxide sensor

The Codetectinator combines a PS1-CO-100-MOD calibrated CO sensor,
a BME280 Pressure/temperature/humidity sensor, an STM32 and an
SDD 128x64 pixel OLED display.

The prototype is made from breakout boards for each of these components,
around a Nucleo-32 (Arduino Nano form factor) board: originally the
NUCLEO-G431KB, currently the NUCLEO-L432KC (same breadboard wiring; see
the Firmware section for the board-internal deltas and the board switch).
Firmware is built on the n-array platform.

## Pinout (Nucleo-32 headers; STM32 pin names as on the G431KB)

The OLED (write-only) and the BME280 share SPI1 on the pins the Nucleo
already labels for it; each has its own chip select. The CO module is a
3.3 V UART device (9600 8N1) on USART1, leaving USART2 on the ST-LINK
VCP as the debug console.

| Signal        | STM32 pin | Nucleo | Mode         | Connects to                    |
|---------------|-----------|--------|--------------|--------------------------------|
| SPI1_SCK      | PB3       | D13    | AF5          | OLED D0/SCL, BME280 SCK        |
| SPI1_MOSI     | PB5       | D11    | AF5          | OLED D1/SDA, BME280 SDI        |
| SPI1_MISO     | PB4       | D12    | AF5          | BME280 SDO                     |
| OLED_CS       | PA11      | D10    | out          | OLED CS                        |
| OLED_DC       | PA12      | D2     | out          | OLED D/C                       |
| OLED_RST      | PB0       | D3     | out          | OLED RES                       |
| BME_CS        | PA8       | D9     | out          | BME280 CS                      |
| USART1_TX     | PA9       | D1     | AF7          | CO module RX (yellow)          |
| USART1_RX     | PA10      | D0     | AF7          | CO module TX (green)           |
| BUTTON        | PA4       | A3     | in, pull-up  | pushbutton to GND              |
| VBAT_SENSE    | PA1       | A1     | ADC1_IN2     | battery voltage divider        |
| USART2_TX/RX  | PA2/PA3   | —      | AF7          | ST-LINK VCP (debug console)    |
| LED           | PB8       | —      | out          | LD2 on-board user LED          |

Power: a 9 V block battery through a 5-16 V -> 3V3 buck converter into
the 3V3 pin (CN3-14). BME280, OLED and CO module all run from the 3V3
rail (CO module < 5 mA, OLED ~20 mA); nothing needs 5 V. On battery the
ST-LINK is unpowered — develop on USB with the battery/buck disconnected;
don't feed the 3V3 pin and USB at the same time (the buck would fight
the on-board LDO U9 via SB15).

VBAT_SENSE divider: 3:1 from the 9 V side (R_top = 2 x R_bottom, e.g.
200k/100k), 100 nF from the ADC pin to GND, long ADC sample time.
Full scale ~9.9 V; reference is the regulated 3V3 so no VREFINT
correction needed.

Spare pins: PA4/PA5/PA6/PA7 (analog-capable), PB6 (TIM PWM, buzzer
candidate), PB7, PA15.

## Firmware (src/)

n-array app, ROM run model, 16 MHz HSI16 (no PLL — lowest-power preset,
ample for a 1 Hz sample cadence). `make` generates the device
header/linker scripts with narray and builds; `make flash` programs over
the ST-LINK with OpenOCD.

Two boards, selected with `make BOARD=g431|l432` (default l432; run
`make clean` when switching). board_g431.h / board_l432.h carry the
per-board facts (family lib includes, pins, DMA channel plan, ADC
channel/clock); the Makefile copies the chosen one to board.h. The
NUCLEO-L432KC became the working target 2026-08-07 when the G431KB's
ST-LINK wedged (ancient J2 firmware + a resident app that zeroes
FLASH_ACR.DBG_SWEN); the n-array L4 port was built for it — see
n-array/doc/l4-extraction-notes.md. The BREADBOARD IS IDENTICAL for
both boards (same Nucleo-32 header positions); board-internal deltas:
VCP console RX is PA15/AF3 (not PA3), PA1 = ADC1_IN6 (not IN2), no
PB8 so LD3-on-PB3 doubles as an SPI-activity light and the LED role
moves to PA4 (external, optional), the L4's DMA requests live on fixed
channels (all fit on DMA1), and the console runs 460800 (not 921600:
the L432's OVER8 receiver proved deaf on the bench; 460800 is the
fastest in-tolerance 16x rate at a 16 MHz kernel).

- `main.c` — bring-up, vector table, SysTick ms timebase, 1 Hz sampling
  loop, button debounce, console on the VCP at 921600 (OVER8: /16 from
  the 16 MHz kernel would be +2.1% baud error). Stats + P/T/H/CO values
  every 10 s; console keys for benching without hardware: 'b' acts as
  the pushbutton, 'd' toggles a live ASCII mirror of the framebuffer
  (1 char = 1 pixel, 128+ column terminal, \e[H/\e[K in-place refresh,
  streamed cooperatively so it never blocks the loop)
- `ps1co.c` — PS1-CO-100 Q&A protocol: 0x86 poll + checksum-verified
  parser (also accepts active-upload frames), plus the 0xD7 module
  identification (sensor type/range/unit/decimals), retried at init
  until answered and checked against the expected CO/ppm/1-decimal —
  catches e.g. a 1000 ppm variant, and its range field cross-checks the
  concentration scale. NOTE: the concentration scale (bytes 6..7 =
  0.1 ppm) matches the datasheet's unit/decimals config but its
  examples are inconsistent — verify on the bench
- `bme280.c` — transport (forced mode 1x/1x/1x); the calibration decode
  and fixed-point compensation live in `bme280_comp.c`, a pure unit with
  a host-side test (`make test`): pinned vectors (T/P from the BMP280
  datasheet's worked example — deliberately: the BME280 datasheet has no
  numeric example and the T/P formulas are identical; H via pinned
  calib-decode vectors), the double-precision formulas as oracle, and a
  UBSan sweep. The datasheet's
  fixed-point code harbors signed overflows (int32 squares in T, H4<<20
  in H, an int64 product in P); this version is widened/clamped to be
  UB-free over a generous calibration envelope and bit-identical to the
  datasheet inside the physical one. H4/H5 sign-extension reconciled
  against stm32f103_bme280
- `ssd1306.c` — framebuffer + 5x7 font, big numbers via Scale3x
  smoothing of the same font (15x21 glyphs, no extra font data),
  contrast/dim/off, SPI shared with the BME280 through the SPIQ
  ss-hook (CS+DC demux)
- `battery.c` — ADC1_IN2 single conversions, 640.5-cycle sample time
- `history.c` — 4 x 900 s int16 ring (units: 0.1 hPa / 0.01 C / 0.1 %RH
  / 0.1 ppm)
- `ui.c` — tabs, nonlinear-time graph, alarm takeover, dim policy

## UI

Tabbed interface, five tabs: BAT / P / T / H / CO; boots on CO (the
rightmost). Each tab shows the current value in large Scale3x digits
with the unit small at its lower right, plus a sliding min/max graph
on a nonlinear time scale — max and min flank the plot on the left,
each in its half-height. P/T/H/CO graph the last 15 minutes (1 s
samples); BAT samples every 10 minutes, so its ring spans ~6 days of
discharge curve.

The pushbutton rotates through the tabs. If the display is dimmed or
off, the first push only wakes it (no tab change).

CO alarm: above 10 / 30 / 70 ppm a separate alarm display takes over
(full contrast + 2 Hz invert blink, overrides dimming and tabs),
showing the current level and which threshold is exceeded; releases
with 0.5 ppm hysteresis. Battery alarm: below 6.5 V (with a battery
attached, i.e. above the 4.5 V sense floor) a calmer takeover — no
blink — which the button acknowledges; CO always outranks it.

Display power policy: full contrast on button activity or alarm, timed
decay to low contrast, display-off (0xAE) after a longer idle timeout.

Board gotchas, NUCLEO-G431KB (UM2397):
- PF0/PF1 (D7/D8) are NOT connected to the headers unless SB8/SB11
  are soldered — avoid.
- PB8 doubles as BOOT0; usable as LED output after reset, don't add
  external pull-ups/downs.
- PA15 and PB7 also appear on A5/A4 (SB3/SB2 default ON) — the same
  net on two header pins.

Board gotchas, NUCLEO-L432KC (UM1956):
- The VCP console RX is PA15 (USART2_RX, AF3) — PA3 is a plain header
  pin here. TX is PA2 as on the G431KB.
- PA0/A0 can carry the ST-LINK's 8 MHz MCO through SB17 (UM1956
  Table 6; factory default is OFF/LSE, but ST suggests closing it for
  VCP rates above 9600, and our bench unit measurably drives PA0):
  as a button input it "presses itself" at 8 MHz. Hence BUTTON =
  PA4/A3 on both boards (and the L432's optional external LED on
  PA5/A4); PA0 stays analog on the L432.
- LD3 is hardwired to PB3 = SPI1_SCK = D13: it flickers as an
  SPI-activity light; the firmware's LED role moves to PA4 (A3),
  external and optional.
- BOOT0 is the dedicated PH3 pin (header H3 pads); no PB8 exists.
- This board's ST-LINK is a V2-1: its CDC bridge can wedge the
  host->target direction (keys stop arriving while output still
  flows) — replug the USB. Press an unmapped console key and look
  for the "key xx" ack to tell this apart from a firmware problem.

Console viewing: the port MUST be opened raw with echo off, or the
tty layer reflects the firmware's own output back as keypresses (the
'd'/'b' letters in the status text then toggle the mirror and tabs).
The blessed one-liner:

    (stty 460800 raw -echo && cat) < /dev/cu.usbmodemXXXX

and send keys from another terminal: printf d > /dev/cu.usbmodemXXXX
(rides on the viewer's raw settings while the port is held open).
Or use: screen /dev/cu.usbmodemXXXX 460800 (exit: C-a k).


