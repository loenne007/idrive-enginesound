CC ?= cc
ZIG ?= $(HOME)/.local/zig/zig-aarch64-macos-0.17.1/zig
SRC = main.c synth.c engine.c presets.c out_alsa.c
FLAGS = -std=c99 -O2 -Wall -Wextra -Wpedantic -I../common
.PHONY: host car clean
host: build/es-synth-host
build/es-synth-host: $(SRC) synth.h dsp.h out_alsa.h ../common/protect.h
	mkdir -p build
	$(CC) $(FLAGS) $(SRC) -o $@ -lm
# the engine's id: this folder's name; the car binary is tool/lfb-enginesound/es-synth-<id>
ENGINE := $(notdir $(CURDIR))
OUT := ../../tool/lfb-enginesound/es-synth-$(ENGINE)
car: build/es-synth
	cp build/es-synth $(OUT)
	file $(OUT)
	python3 check_glibc.py $(OUT)
# the head unit has no /lib64: point the ELF interpreter at /lib
build/es-synth: $(SRC) synth.h dsp.h out_alsa.h Makefile
	mkdir -p build
	$(ZIG) cc -target x86_64-linux-gnu.2.27 $(FLAGS) -s $(SRC) -o $@ -ldl -lm -Wl,--dynamic-linker=/lib/ld-linux-x86-64.so.2
clean:
	rm -rf build
