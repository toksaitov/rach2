# family.mk: native ownership of prepared GNU product-family controllers.
HOST_FAMILY_CASE_SRC = tests/cases/host/family_scenario.c
build/tests/cases/host/family_scenario: $(HOST_FAMILY_CASE_SRC) \
    tests/cases/host/family_rows.h $(ETUDE_CASE_LIB_SRC) $(ETUDE_CASE_LIB_HDRS)
	@mkdir -p $(@D)
	$(ETUDE_LINK) -std=c99 -D_DEFAULT_SOURCE -Itools/etude/include -Itools/etude \
	    -Itools/etude/posix -Itools/etude/guest -Iutils
