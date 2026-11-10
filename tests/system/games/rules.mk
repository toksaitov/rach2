# The Wine probes' payload, compiled in Debian and read from a second disk.
GAMES_DIR = build/tests/system/games
$(GAMES_DIR)/probes.tar: tests/system/games/build.sh tests/system/games/probe.h \
                         tests/system/games/glprobe.c tests/system/games/d3d9probe.c \
                         tests/system/games/holdprobe.c $(wildcard tests/graphics/debian-ref/*)
	@mkdir -p $(@D)
	sh tests/system/games/build.sh $@

$(GAMES_DIR)/bundle.tar: $(GAMES_DIR)/probes.tar tests/system/games/run.py \
                         tests/system/games/ref/linux.json vendor/gnu/games/rach2-game.sh
	@set -e; work=; \
	trap '[ -z "$$work" ] || rm -rf "$$work"; rm -f "$@.tmp"' EXIT; \
	trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; \
	mkdir -p build/tmp; work=$$(mktemp -d build/tmp/rach2-games-payload.XXXXXX); \
	mkdir "$$work/ref"; tar -xf $< -C "$$work"; \
	cp tests/system/games/run.py vendor/gnu/games/rach2-game.sh "$$work/"; \
	cp tests/system/games/ref/linux.json "$$work/ref/"; \
	tar -cf $@.tmp -C "$$work" .; mv $@.tmp $@

$(GAMES_DIR)/data.ext2: $(GAMES_DIR)/bundle.tar
	@set -e; work=; \
	trap '[ -z "$$work" ] || rm -rf "$$work"; rm -f "$@.tmp"' EXIT; \
	trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; \
	mkdir -p build/tmp; work=$$(mktemp -d build/tmp/rach2-games-image.XXXXXX); \
	tar -xf $< -C "$$work"; \
	mke2fs -q -t ext2 -F -b 4096 -O none,filetype,sparse_super,large_file \
	    -d "$$work" $@.tmp 4096; \
	mv $@.tmp $@
