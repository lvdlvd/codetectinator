# codetectinator — portable CO detector, two Nucleo-32 boards supported.
# n-array app: generates the device header + linker scripts with narray,
# builds against the n-array lib. ROM run model on both.
#
# BOARD=g431 : NUCLEO-G431KB (the original; ST-LINK currently wedged)
# BOARD=l432 : NUCLEO-L432KC (same breadboard wiring, see board_l432.h)
# Run `make clean` when switching boards: device.h & friends must regen.
BOARD ?= l432

ifeq ($(BOARD),g431)
PART      = STM32G431
PARTFULL  = STM32G431KBU6
MEMTARGET = STM32G431Bxx_ROM
FAMILY    = stm32g4
CLOCKSRC  = $(LIB)/clock.c
OCDTARGET = target/stm32g4x.cfg
else
PART      = STM32L432
PARTFULL  = STM32L432KCU6
MEMTARGET = STM32L432Cxx_ROM
FAMILY    = stm32l4
CLOCKSRC  =
OCDTARGET = target/stm32l4x.cfg
endif

LIB    = ../../n-array/lib
PERIPH = ../../n-array/periph/$(FAMILY)
NARRAY = ../../n-array/gen/narray

REVISION := $(shell git log -1 --format="%h" 2>/dev/null || echo 0)

PREFIX  = arm-none-eabi-
CC      = $(PREFIX)gcc
OBJCOPY = $(PREFIX)objcopy
SIZE    = $(PREFIX)size

ARCH    = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
CFLAGS  = -std=gnu23 -O2 -g3 -ffreestanding -ffunction-sections -fdata-sections \
          $(ARCH) -Wall -Wextra -Wno-pedantic -I. -I$(LIB) \
          -DSPI_XMIT_BUF=64 -D__REVISION__='0x$(REVISION)'
LDFLAGS = -nostartfiles -lnosys $(ARCH) -T $(MEMTARGET).ld -Wl,-L,. -Wl,-L,$(LIB) \
          -Wl,--gc-sections,-Map,codetectinator.map

CSRC = main.c ssd1306.c bme280.c bme280_comp.c ps1co.c battery.c history.c ui.c \
       $(LIB)/gpio.c $(CLOCKSRC) $(LIB)/spi.c \
       $(LIB)/console.c $(LIB)/fault.c $(LIB)/tprintf.c

# generated: narray outputs + the selected board definition
GEN = device.h pinmux.h devs.ld $(MEMTARGET).ld board.h

board.h: board_$(BOARD).h
	cp $< $@

$(NARRAY): $(wildcard ../../n-array/gen/*.go)
	cd ../../n-array/gen && go build -o narray .

device.h: $(NARRAY)
	$(NARRAY) -part $(PART) -header $(PERIPH) > $@

pinmux.h: $(NARRAY)
	$(NARRAY) -pinmux $(PERIPH) > $@

devs.ld: $(NARRAY)
	$(NARRAY) -part $(PART) -devs $(PERIPH) > $@

$(MEMTARGET).ld: $(NARRAY)
	$(NARRAY) -memory $(MEMTARGET) $(PERIPH) > $@

codetectinator.elf: $(GEN) $(CSRC) $(LIB)/sections.ld
	$(CC) $(CFLAGS) $(CSRC) $(LDFLAGS) -o $@
	$(SIZE) $@

codetectinator.bin: codetectinator.elf
	$(OBJCOPY) -O binary $< $@

.DEFAULT_GOAL := codetectinator.bin

# regenerate the in-file pinout annotations from the datasheet
pinfmt: $(NARRAY) pinmux.h
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

clean:
	rm -f *.o *.elf *.bin *.hex *.map bme280_comp_test $(GEN)

.PHONY: flash clean pinfmt test

# a failed "narray ... > $@" must not leave a truncated target behind
.DELETE_ON_ERROR:
