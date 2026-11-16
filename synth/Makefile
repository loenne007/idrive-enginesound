# Every sound engine in synth/<id>/ (each with its own Makefile: host, car): make car builds all their car binaries
# into tool/lfb-enginesound/es-synth-<id>.
ENGINES := $(patsubst %/Makefile,%,$(wildcard */Makefile))
.PHONY: host car test $(ENGINES)
host car:
	@for e in $(ENGINES); do $(MAKE) -C $$e $@ || exit 1; done
test:
	@for e in $(ENGINES); do sh $$e/test.sh || exit 1; done
