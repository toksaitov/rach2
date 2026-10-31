# esctest2.mk: package the pinned upstream driver without staging it on Rach2.
ESCTEST2_COMMIT = 2798f12149a19c3295e9b4853ab2da4b2eff1b2b
ESCTEST2_TARBALL = vendor/esctest2/esctest2-$(ESCTEST2_COMMIT).tar.gz
ESCTEST2_SHA256 = 04b01a8343045a6801ea9a2a849b339e1b41179197102363a43fab13b25a2e1e
ESCTEST2_PATCH = vendor/esctest2/patches/0001-headless-measurements.patch
ESCTEST2_DIR = build/vendor/esctest2/source
ESCTEST2_ZIP = build/tests/host/esctest2-driver.zip
ESCTEST2_PYTHON = $(shell command -v python3)

.PHONY: esctest2
esctest2: $(ESCTEST2_ZIP) build/tests/cases/host/esctest2 build/tests/host/esctest2-term \
    build/tests/host/esctest2-python3 build/tests/host/esctest2-known-failures.txt \
    build/tests/host/esctest2-protected-prefixes.txt

build/tests/host/esctest2-python3: $(ESCTEST2_PYTHON) mk/esctest2.mk
	@mkdir -p $(@D)
	@test -n '$(ESCTEST2_PYTHON)'
	ln -sf '$(ESCTEST2_PYTHON)' $@

build/tests/host/esctest2-%.txt: vendor/esctest2/%.txt
	@mkdir -p $(@D)
	cp $< $@

build/tests/host/esctest2-term: $(TERM_HOST_BIN)
	@mkdir -p $(@D)
	cp $< $@

$(ESCTEST2_ZIP): $(ESCTEST2_TARBALL) $(ESCTEST2_PATCH) mk/esctest2.mk
	@printf '%s  %s\n' '$(ESCTEST2_SHA256)' '$(ESCTEST2_TARBALL)' | shasum -a 256 -c -
	@mkdir -p build/vendor/esctest2 $(@D)
	rm -rf $(ESCTEST2_DIR)
	mkdir -p $(ESCTEST2_DIR)
	tar -xzf $(ESCTEST2_TARBALL) --strip-components=1 -C $(ESCTEST2_DIR)
	patch -d $(ESCTEST2_DIR) -p1 -F 0 --forward < $(ESCTEST2_PATCH)
	PYTHONPYCACHEPREFIX=$(CURDIR)/build/cache/esctest2-python python3 -m compileall -q $(ESCTEST2_DIR)/esctest
	cp $(ESCTEST2_DIR)/esctest/esctest.py $(ESCTEST2_DIR)/esctest/__main__.py
	python3 -m zipapp $(ESCTEST2_DIR)/esctest -o $@.new
	mv $@.new $@

build/tests/cases/host/esctest2: tests/cases/host/esctest2.c \
    tests/cases/host/native.c tests/cases/host/native.h \
    $(filter-out tests/cases/host/host.c tests/cases/host/sanitizer_tool.c tools/etude/json.c utils/sha256.c,$(ETUDE_HOSTCASE_SRC))
	@mkdir -p $(@D)
	$(ETUDE_LINK) -std=c99 -D_DEFAULT_SOURCE -Itools/etude/include -Itools/etude \
	    $(ETUDE_PLATFORM_FLAGS) $(ETUDE_PLATFORM_LIBS)
