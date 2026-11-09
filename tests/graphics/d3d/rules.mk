# Direct3D test payload, compiled in Debian and read from a second guest disk.
D3D_DIR = build/tests/graphics/d3d
D3D_COMPILE_INPUTS = $(wildcard tests/graphics/d3d/*.c tests/graphics/d3d/*.h) \
             tests/graphics/d3d/build.sh tests/graphics/d3d/sources.txt tests/graphics/guest-env.sh \
             $(wildcard tests/graphics/debian-ref/*) vendor/gnu/debian/fetch_asset.sh
$(D3D_DIR)/compiled.tar: $(D3D_COMPILE_INPUTS)
	@mkdir -p $(@D)
	sh tests/graphics/d3d/build.sh $@

$(D3D_DIR)/runtime.tar: tests/graphics/d3d/runtime.sh $(wildcard tests/graphics/debian-ref/*)
	@mkdir -p $(@D)
	sh tests/graphics/d3d/runtime.sh $@

$(D3D_DIR)/privatefilemusl: tests/musl/privatefilemusl.c | musl-programs
	cp $(ARCH_BUILD)/tests/musl/privatefilemusl $@

$(D3D_DIR)/bundle.tar: $(D3D_DIR)/compiled.tar $(D3D_DIR)/runtime.tar $(D3D_DIR)/privatefilemusl tests/graphics/d3d/run.py \
                       tests/graphics/guest-env.sh $(wildcard tests/graphics/d3d/ref/*)
	@set -e; work=; \
	trap '[ -z "$$work" ] || rm -rf "$$work"; rm -f "$@.tmp"' EXIT; \
	trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; \
	mkdir -p build/tmp; work=$$(mktemp -d build/tmp/d3d-payload.XXXXXX); \
	tar -xf $< -C "$$work"; tar -xf $(D3D_DIR)/runtime.tar -C "$$work"; \
	cp tests/graphics/d3d/run.py tests/graphics/guest-env.sh $(D3D_DIR)/privatefilemusl "$$work/"; \
	cp -R tests/graphics/d3d/ref "$$work/"; \
	tar -cf $@.tmp -C "$$work" .; mv $@.tmp $@

$(D3D_DIR)/data.ext2: $(D3D_DIR)/bundle.tar
	@set -e; work=; \
	trap '[ -z "$$work" ] || rm -rf "$$work"; rm -f "$@.tmp"' EXIT; \
	trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; \
	mkdir -p build/tmp; work=$$(mktemp -d build/tmp/d3d-image.XXXXXX); \
	tar -xf $< -C "$$work"; \
	mke2fs -q -t ext2 -F -b 4096 -O none,filetype,sparse_super,large_file \
	    -d "$$work" $@.tmp 65536; \
	mv $@.tmp $@
