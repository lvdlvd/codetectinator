# codetectinator — portable CO detector, two Nucleo-32 boards supported.
# n-array app: generates the device header + linker scripts with narray,
# builds against the n-array lib. ROM run model on both.
#
# BOARD=g431 : NUCLEO-G431KB (the original; ST-LINK currently wedged)
# BOARD=l432 : NUCLEO-L432KC (same breadboard wiring, see board_l432.h)
# Run `make clean` when switching boards: board.h must be recopied. The
# narray-generated files no longer need it — they live per board, below.
BOARD ?= l432
BOARDS = g431 l432

# PANEL=64 : 0.96" 128x64 SSD1306 (12-row small font, 42 px digits)
# PANEL=32 : 0.91" 128x32 bar (5x7 font, 21 px digits)
# The driver derives its init from the row count, ui.c its layout from the
# font metrics font.h selects. One gcc invocation builds everything, and a
# PANEL other than the last build's drops the elf right here at parse time
# (a stamp prerequisite would race make's whole-second mtime comparison), so
# switching PANEL needs no `make clean`.
PANEL ?= 64
$(shell test -e .panel-$(PANEL) || { rm -f .panel-* codetectinator.elf; touch .panel-$(PANEL); })

ifeq ($(BOARD),g431)
PART      = STM32G431
PARTFULL  = STM32G431KBU6
MEMTARGET = STM32G431Bxx_ROM
FAMILY    = stm32g4
CLOCKSRC  = $(NLIB)/clock.c
OCDTARGET = target/stm32g4x.cfg
else
PART      = STM32L432
PARTFULL  = STM32L432KCU6
MEMTARGET = STM32L432Cxx_ROM
FAMILY    = stm32l4
CLOCKSRC  =
OCDTARGET = target/stm32l4x.cfg
endif

# SELF-CONTAINED [N]ARRAY BUILD. Everything this firmware compiles lives in
# this repository: the lib sources it uses are copied into nlib/ and the
# narray-generated files into g431/ and l432/, all of them tracked. The rules
# that copy and generate carry NO prerequisite on the n-array checkout, so a
# file already present is never refreshed, whatever upstream does — n-array
# can evolve freely without breaking this build, and these copies are ours to
# edit. A file is fetched only when missing, so the one gesture that reaches
# upstream is
#
#	make n-array-clean && make	# take current n-array, see what breaks
#
# and that breakage lands in a build you asked for. `make clean` deliberately
# leaves them alone, so clean-and-rebuild needs no checkout. NARRAY_DIR names
# that checkout: a peer of this repo by default, overridable in the
# environment or on the command line. RULE OF THREE: a local edit to nlib/
# wanted for the third time belongs upstream in n-array, anonymized.
#
# nlib/ holds the union over both boards — the G4 and L4 halves of the lib
# have distinct names (clock.h vs clock_l4.h, dma_g4.h vs dma_l4.h,
# usart_v3.h vs usart_v2.h), so one directory serves both. The generated
# headers do NOT: device.h and pinmux.h genuinely differ per part, which is
# why each board gets its own directory and both can sit here at once.
NARRAY_DIR ?= ../../n-array

NLIB   = nlib
PERIPH = $(NARRAY_DIR)/periph/$(FAMILY)
NARRAY = $(NARRAY_DIR)/gen/narray

REVISION := $(shell git log -1 --format="%h" 2>/dev/null || echo 0)

PREFIX  = arm-none-eabi-
CC      = $(PREFIX)gcc
OBJCOPY = $(PREFIX)objcopy
SIZE    = $(PREFIX)size

ARCH    = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
CFLAGS  = -std=gnu23 -O2 -g3 -ffreestanding -ffunction-sections -fdata-sections \
          $(ARCH) -Wall -Wextra -Wno-pedantic -I. -I$(BOARD) -I$(NLIB) \
          -DSPI_XMIT_BUF=64 -DOLED_ROWS=$(PANEL) -D__REVISION__='0x$(REVISION)'
LDFLAGS = -nostartfiles -lnosys $(ARCH) -T $(BOARD)/$(MEMTARGET).ld -Wl,-L,. -Wl,-L,$(BOARD) \
          -Wl,--gc-sections,-Map,codetectinator.map

CSRC = main.c ssd1306.c bme280.c bme280_comp.c ps1co.c battery.c history.c ui.c \
       $(NLIB)/gpio.c $(CLOCKSRC) $(NLIB)/spi.c \
       $(NLIB)/console.c $(NLIB)/fault.c $(NLIB)/tprintf.c

# the lib files this project uses, as the union over both boards
NLIB_C  = clock.c console.c fault.c gpio.c spi.c tprintf.c
NLIB_H  = clock.h clock_l4.h console.h dma.h dma_g4.h dma_l4.h fault.h fifo.h \
          gpio.h nvic.h serial.h spi.h startup.h tprintf.h usart_v2.h usart_v3.h
NLIB_FILES = $(addprefix $(NLIB)/, $(NLIB_C) $(NLIB_H))

# copied from the lib as well, but linker input, so it sits with the linker
# scripts rather than among the C sources; the same layout serves both boards
LIBLD = sections.ld

# narray outputs, per board
GEN = $(addprefix $(BOARD)/, device.h pinmux.h devs.ld $(MEMTARGET).ld)

board.h: board_$(BOARD).h
	cp $< $@

# --- the only rules that reach NARRAY_DIR, and only when a file is gone ---

$(NLIB_FILES) $(LIBLD):
	@mkdir -p $(@D)
	cp $(NARRAY_DIR)/lib/$(@F) $@

# The generator is built in place in the checkout, on demand. It is
# bootstrapped from INSIDE the recipes rather than named as their
# prerequisite: make remakes a prerequisite even when the target is present
# and up to date, order-only included, which would demand the checkout on
# every build and hand back the dependency this arrangement removes.
$(NARRAY): $(wildcard $(NARRAY_DIR)/gen/*.go)
	cd $(NARRAY_DIR)/gen && go build -o narray .

$(BOARD)/device.h:
	@mkdir -p $(@D)
	@$(MAKE) --no-print-directory $(NARRAY)
	$(NARRAY) -part $(PART) -header $(PERIPH) > $@

$(BOARD)/pinmux.h:
	@mkdir -p $(@D)
	@$(MAKE) --no-print-directory $(NARRAY)
	$(NARRAY) -pinmux $(PERIPH) > $@

$(BOARD)/devs.ld:
	@mkdir -p $(@D)
	@$(MAKE) --no-print-directory $(NARRAY)
	$(NARRAY) -part $(PART) -devs $(PERIPH) > $@

$(BOARD)/$(MEMTARGET).ld:
	@mkdir -p $(@D)
	@$(MAKE) --no-print-directory $(NARRAY)
	$(NARRAY) -memory $(MEMTARGET) $(PERIPH) > $@

# The 128x64 panel's small font is the public-domain X.Org 6x13, converted
# from its BDF by bdf2h.go (one file, standard library only, run with `go
# run`). Same policy as the n-array files: the output is tracked and only
# regenerated when missing, which is the one gesture that reaches the network.
FONT6X13_BDF = https://gitlab.freedesktop.org/xorg/font/misc-misc/-/raw/master/6x13.bdf
font6x13.h:
	curl -sfL $(FONT6X13_BDF) | go run bdf2h.go font6x13 0x20 0x7E 2 11 > $@

# --------------------------------------------------------------------------

codetectinator.elf: $(GEN) $(LIBLD) $(NLIB_FILES) board.h $(CSRC)
	$(CC) $(CFLAGS) $(CSRC) $(LDFLAGS) -o $@
	$(SIZE) $@

codetectinator.bin: codetectinator.elf
	$(OBJCOPY) -O binary $< $@

.DEFAULT_GOAL := codetectinator.bin

# regenerate the in-file pinout annotations from the datasheet
pinfmt: $(BOARD)/pinmux.h
	@$(MAKE) --no-print-directory $(NARRAY)
	$(NARRAY) -part $(PARTFULL) -pinfmt main.c $(PERIPH)

# Flash over ST-Link with OpenOCD (G4 flash is driven by the stm32l4x cmd set).
flash: codetectinator.elf
	openocd \
		-f interface/stlink.cfg \
		-f $(OCDTARGET) \
		-c init \
		-c halt \
		-c "flash probe 0" \
		-c "stm32l4x mass_erase 0" \
		-c "sleep 100" \
		-c "program $< verify" \
		-c reset \
		-c shutdown

# Host-side unit test of the pure compensation code, with UBSan as the
# overflow hunter (the datasheet's own fixed-point version does not pass).
HOSTCC ?= cc
test: bme280_comp.c bme280_comp.h bme280_comp_test.c
	$(HOSTCC) -O2 -g -Wall -Wextra -fsanitize=undefined -fno-sanitize-recover=undefined \
		-o bme280_comp_test bme280_comp.c bme280_comp_test.c
	./bme280_comp_test

# Host-side preview of the UI screens (fbtest.c), rendered as ASCII: the
# real ui.c/ssd1306.c/history.c against a stub spi.h (hoststub/ shadows
# nlib/ on the include path, so ssd1306.c compiles unchanged).
fbtest: fbtest.c ui.c ssd1306.c history.c ssd1306.h ui.h history.h hoststub/spi.h
	$(HOSTCC) -std=gnu23 -O2 -g -Wall -Wextra -Ihoststub -I. -DSPI_XMIT_BUF=64 -DOLED_ROWS=$(PANEL) \
		-o fbtest fbtest.c ui.c ssd1306.c history.c
	./fbtest

clean:
	rm -f *.o *.elf *.bin *.hex *.map bme280_comp_test fbtest board.h .panel-*

# the deliberate upgrade gesture: drop everything that came from n-array —
# both boards' generated sets, not just the one currently selected
n-array-clean:
	rm -rf $(BOARDS)
	rm -f $(LIBLD) $(NLIB_FILES)

.PHONY: flash clean n-array-clean pinfmt test fbtest

# a failed "narray ... > $@" must not leave a truncated target behind
.DELETE_ON_ERROR:
