# mk/unifont.mk: GNU Unifont packed for term (vendor/unifont/Readme.md).
#
# The root Makefile includes this file. The vendored .hex text is checked by
# its SHA-256, the digest tools/font/mkfont.sh checks too, and converted by
# tools/font/mkunifont.c into $(UNIFONT_FONT), in the format term/unifont.h
# describes. The font is the same for both architectures, so it is built
# once, under build/unifont/ whatever ARCH says. The converter checks the
# whole font by reading it back with term's reader before it writes it.
UNIFONT_HEX    = vendor/unifont/unifont_all-18.0.01.hex.gz
UNIFONT_SHA256 = 14c96e497466a82e46cf20032ce510e186bdc8658c9158e2290f452cda9bc498
UNIFONT_FONT   = build/unifont/unifont.font
UNIFONT_STAMP  = build/unifont/.stamp
MKUNIFONT      = build/tools/font/mkunifont

.PHONY: unifont
unifont: $(UNIFONT_STAMP)

$(MKUNIFONT): tools/font/mkunifont.c term/unifont.c term/unifont.h
	@mkdir -p $(dir $@)
	$(HOST_LINK)

# The stamp is removed first and written last, so a failed step leaves none
# and the next make tries again, and the step removes its scratch on either
# outcome. The text is unpacked into a file, not a pipe, so that a damaged
# archive stops the step instead of giving the converter a short text. A
# font removed under a stamp that stays makes the stamp out of date, as the
# vendored projects' outputs do.
$(UNIFONT_STAMP): $(UNIFONT_HEX) $(MKUNIFONT) mk/unifont.mk | $(SHA256TOOL)
	@rm -f $@ $(UNIFONT_FONT)
	@mkdir -p $(dir $@)
	@sum=$$($(SHA256TOOL) $(UNIFONT_HEX)) && [ "$${sum%% *}" = $(UNIFONT_SHA256) ] || \
	    { echo "unifont: $(UNIFONT_HEX): not the pinned Unifont 18.0.01" >&2; exit 1; }
	@gzip -dc $(UNIFONT_HEX) > $(UNIFONT_FONT).hex && \
	    $(MKUNIFONT) $(UNIFONT_FONT).hex $(UNIFONT_FONT).tmp && \
	    mv $(UNIFONT_FONT).tmp $(UNIFONT_FONT); \
	    s=$$?; rm -f $(UNIFONT_FONT).hex $(UNIFONT_FONT).tmp; exit $$s
	@touch $@
	@echo "unifont: result: $(UNIFONT_FONT)"
$(UNIFONT_FONT): $(UNIFONT_STAMP)
ifeq ($(wildcard $(UNIFONT_FONT)),)
_ := $(shell rm -f $(UNIFONT_STAMP))
endif
