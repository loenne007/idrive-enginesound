# Every sound engine in synth/<id>/ (its Makefile includes common/engine.mk).
#   make host | car   every engine for this computer | the head unit (tool/lfb-enginesound/es-synth-<id>)
#   make test         the speaker protection, then every engine's tests
#   make samples      ../samples/: the app's five sounds as recordings (40 s demo drives)
ENGINES := $(patsubst %/Makefile,%,$(wildcard */Makefile))
.PHONY: host car test samples
host car:
	@for e in $(ENGINES); do $(MAKE) -C $$e $@ || exit 1; done
test:
	@mkdir -p common/build && $(CC) -std=c99 -O2 -Wall -Wextra -D_DEFAULT_SOURCE common/test_protect.c -lm -o common/build/test_protect && common/build/test_protect && LIVE=1 common/build/test_protect
	@for e in $(ENGINES); do $(MAKE) -C $$e test || exit 1; done
samples:
	@$(MAKE) -C simulate samples && $(MAKE) -C enhance samples
	mkdir -p ../samples
	cp simulate/samples/i6.wav ../samples/inline-6.wav
	cp simulate/samples/v8.wav ../samples/v8.wav
	cp simulate/samples/v12.wav ../samples/v12.wav
	cp enhance/samples/i6.wav ../samples/inline-6-diesel.wav
	cp enhance/samples/v8.wav ../samples/v8-tdi.wav
