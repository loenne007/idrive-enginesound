# Every sound engine in synth/<id>/ (each with its own Makefile: host, car): make car builds all their car binaries
# into tool/lfb-enginesound/es-synth-<id>.
ENGINES := $(filter-out common,$(patsubst %/Makefile,%,$(wildcard */Makefile)))
.PHONY: host car test $(ENGINES)
host car:
	@for e in $(ENGINES); do $(MAKE) -C $$e $@ || exit 1; done
test:
	@mkdir -p common/build && $(CC) -std=c99 -O2 -Wall -Wextra -D_DEFAULT_SOURCE common/test_protect.c -lm -o common/build/test_protect && common/build/test_protect && LIVE=1 common/build/test_protect
	@for e in $(ENGINES); do sh $$e/test.sh || exit 1; done
