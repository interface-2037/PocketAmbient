# Ambient Pocket - host build (GCC; tested with WinLibs MinGW-w64 and Linux GCC)
#   make test      unit tests
#   make render    10-minute excerpt per mode into renders/
#   make analyze   run tools/analyze_audio.py on renders/
#   make tables    regenerate lookup tables (needs Python)
#   make fw        composer firmware for the PicoRV32 (clang + lld) -> src/fpga/core/cpu/composer.hex
#   make fpga      Quartus build (src/fpga)   make package  SD-card layout + zip in dist/
# On Windows use mingw32-make, after `source tools/env.sh` (Git Bash) or tools\env.ps1.

ifeq ($(origin CC),default)
CC      := gcc
endif
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Wno-unused-const-variable -Wno-unused-function
PYTHON  ?= python
BUILD   := build
EXE     := $(if $(filter Windows_NT,$(OS)),.exe,)

COMPOSER := fw/composer/composer.c fw/composer/fxp.c fw/composer/prng.c fw/composer/sched.c \
            fw/composer/voices.c fw/composer/harmony.c fw/composer/mode_drift.c \
            fw/composer/mode_tides.c fw/composer/mode_hush.c fw/composer/mode_mother.c
MODEL    := model/synth_model.c fw/hal/hal_model.c
HEADERS  := $(wildcard fw/composer/*.h fw/hal/*.h fw/ui/*.h model/*.h)

RENDER := $(BUILD)/render$(EXE)
TESTS  := $(BUILD)/tests$(EXE)
FUZZ   := $(BUILD)/fuzz$(EXE)
RVSIM  := $(BUILD)/rvsim$(EXE)

.PHONY: all test render analyze tables clean fpga package fw
.DEFAULT_GOAL := all

QUARTUS_BIN ?= /c/altera_lite/25.1std/quartus/bin64
fpga:
	cd src/fpga && PATH="$(QUARTUS_BIN):$$PATH" quartus_sh --flow compile ap_core

package:
	$(PYTHON) tools/package.py

all: $(RENDER) $(TESTS) $(FUZZ) $(RVSIM)

$(BUILD):
	mkdir -p $(BUILD)

$(RENDER): model/render.c $(COMPOSER) $(MODEL) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ model/render.c $(COMPOSER) $(MODEL)

$(TESTS): tests/test_main.c $(wildcard tests/*.c) $(COMPOSER) $(MODEL) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -Itests -o $@ $(wildcard tests/*.c) $(COMPOSER) $(MODEL)

$(RVSIM): model/rvsim.c model/synth_model.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ model/rvsim.c model/synth_model.c

$(FUZZ): model/fuzz.c model/synth_model.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ model/fuzz.c model/synth_model.c

test: $(TESTS)
	./$(TESTS)

RENDER_MIN ?= 10
render: $(RENDER)
	mkdir -p renders
	./$(RENDER) --mode drift --seed 123 --minutes $(RENDER_MIN) --out renders/drift.wav --events renders/drift.csv
	./$(RENDER) --mode tides --seed 123 --minutes $(RENDER_MIN) --out renders/tides.wav --events renders/tides.csv
	./$(RENDER) --mode hush  --seed 123 --minutes $(RENDER_MIN) --out renders/hush.wav  --events renders/hush.csv

analyze:
	$(PYTHON) tools/analyze_audio.py renders/drift.wav renders/tides.wav renders/hush.wav

tables:
	$(PYTHON) tools/gen_tables.py

# ---- composer firmware (RV32IMC, freestanding; DECISIONS D48)
RV_CC     ?= clang
RV_LD     ?= ld.lld
RV_OBJCOPY ?= llvm-objcopy
RV_CFLAGS := --target=riscv32-unknown-elf -march=rv32imc -mabi=ilp32 -Os -ffreestanding -fno-builtin \
             -fno-common -ffunction-sections -std=c11 -Wall -Wextra -Wno-unused-const-variable \
             -Wno-unused-function
FW_SRCS   := fw/rv/crt0.S fw/rv/coro.S fw/rv/main.c fw/rv/rt.c fw/hal/hal_mmio.c fw/ui/ui.c fw/ui/fb.c fw/ui/font5x7.c \
             $(COMPOSER)
FW_OBJS   := $(patsubst %,$(BUILD)/fw/%.o,$(FW_SRCS))
FW_ELF    := $(BUILD)/fw/composer.elf
FW_HEX    := src/fpga/core/cpu/composer.hex

$(BUILD)/fw/%.o: % $(HEADERS) fw/rv/link.ld
	@mkdir -p $(dir $@)
	$(RV_CC) $(RV_CFLAGS) -c $< -o $@

$(FW_ELF): $(FW_OBJS) fw/rv/link.ld
	$(RV_LD) -m elf32lriscv -T fw/rv/link.ld --gc-sections -Map $(BUILD)/fw/composer.map -o $@ $(FW_OBJS)

$(BUILD)/fw/composer.bin: $(FW_ELF)
	$(RV_OBJCOPY) -O binary $< $@

fw: $(BUILD)/fw/composer.bin
	$(PYTHON) tools/fw_hex.py $< $(FW_HEX)

clean:
	rm -rf $(BUILD)
