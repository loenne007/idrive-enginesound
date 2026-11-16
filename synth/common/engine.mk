# The build of one sound engine. synth/<id>/Makefile sets SRC (its C files in link order, with main.c and
# ../common/out_alsa.c) and HDR (its headers), optionally INC and MONO, and includes this. The engine's id is its
# folder's name.
#   make host      build/es-synth-host, for this computer (tests, renders)
#   make car       the head unit's binary, ../../tool/lfb-enginesound/es-synth-<id>
#   make samples   samples/{i6,v8,v10,v12}.wav: the 40 s demo drive of every preset, mono
#   make test      test_core.c, the samples, check.py on them, a benchmark
CC ?= cc
ZIG ?= $(HOME)/.local/zig/zig-aarch64-macos-0.17.1/zig
FLAGS = -std=c99 -O2 -Wall -Wextra -Wpedantic $(INC) -I../common
# samples: the left channel, or "mix" = (L + R) / 2
MONO ?= left
DEPS = $(SRC) $(HDR) ../common/out_alsa.h ../common/protect.h Makefile ../common/engine.mk
ENGINE := $(notdir $(CURDIR))
OUT := ../../tool/lfb-enginesound/es-synth-$(ENGINE)
.PHONY: host car samples test clean
host: build/es-synth-host
build/es-synth-host: $(DEPS)
	mkdir -p build
	$(CC) $(FLAGS) $(SRC) -o $@ -lm
car: build/es-synth
	cp build/es-synth $(OUT)
	file $(OUT)
	python3 ../../tools/check_glibc.py $(OUT)
# x86-64, glibc <= 2.27; the head unit has no /lib64, so the ELF interpreter is the one in /lib
build/es-synth: $(DEPS)
	mkdir -p build
	$(ZIG) cc -target x86_64-linux-gnu.2.27 $(FLAGS) -s $(SRC) -o $@ -ldl -lm -Wl,--dynamic-linker=/lib/ld-linux-x86-64.so.2
samples: host
	mkdir -p samples
	for p in i6 v8 v10 v12; do \
		./build/es-synth-host --out wav:build/$$p-stereo.wav --demo 40 --preset $$p && \
		python3 ../common/mono.py $(MONO) build/$$p-stereo.wav samples/$$p.wav || exit 1; done
test: host
	$(CC) $(FLAGS) test_core.c $(filter-out main.c ../common/out_alsa.c,$(SRC)) -lm -o build/test-core
	./build/test-core
	$(MAKE) samples
	python3 check.py
	./build/es-synth-host --bench 60
clean:
	rm -rf build samples
