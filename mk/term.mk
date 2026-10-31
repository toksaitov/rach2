# mk/term.mk: term, the terminal that runs on the whole framebuffer, built
# against libtsm for the architecture ARCH names and for the host.
#
# The Rach2 build compiles with the musl wrapper and links libtsm.a before
# musl's libc.a, into build/term/term or build/x86_64/term/term; `all` builds
# it. The host build compiles with HOSTCC and HOSTCFLAGS (mk/host.mk) into
# build/host/term/term, for term -H and the tests that drive it; `make
# term-host` builds it. Both read the sources from term/sources.mk, and each
# object depends on the headers its .d file names and on its libtsm's stamp,
# which stages the patched libtsm.h.

include term/sources.mk

# The dialect is libtsm.h's, C99. rach2/include has <rach2/fb.h>, which the
# images stage under /include for the guest build.
TERM_CFLAGS = -std=c99 -O2 -Wall -Wextra -Werror -MMD -MP -Irach2/include
TERM_HOST_CFLAGS = $(HOSTCFLAGS) -std=c99 -MMD -MP -Irach2/include

TERM_BIN = $(ARCH_BUILD)/term/term
TERM_OBJS = $(addprefix $(ARCH_BUILD)/term/,$(TERM_SRC:.c=.o))
TERM_HOST_BIN = build/host/term/term
TERM_HOST_OBJS = $(addprefix build/host/term/,$(TERM_SRC:.c=.o))

.PHONY: term term-host
term: $(TERM_BIN)
term-host: $(TERM_HOST_BIN)
all: $(TERM_BIN)

$(TERM_OBJS): $(ARCH_BUILD)/term/%.o: term/%.c mk/term.mk $(LIBTSM_STAMP) $(VENDOR_COMMON)
	@mkdir -p $(@D)
	RACH2_ARCH=$(ARCH) vendor/musl/muslgcc $(TERM_CFLAGS) -I$(LIBTSM_STAGE)/include -c $< -o $@

$(TERM_BIN): $(TERM_OBJS) $(LIBTSM_A) $(LIBC_STAMP) | $(ELFCHECK)
	RACH2_ARCH=$(ARCH) vendor/musl/muslgcc -static -o $@ $(TERM_OBJS) $(LIBTSM_A)
	@$(ELFCHECK) --arch $(ARCH) $@ || { rm -f $@; exit 1; }

$(TERM_HOST_OBJS): build/host/term/%.o: term/%.c mk/term.mk $(LIBTSM_HOST_STAMP)
	@mkdir -p $(@D)
	$(HOSTCC) $(TERM_HOST_CFLAGS) -I$(LIBTSM_HOST_STAGE)/include -c $< -o $@

$(TERM_HOST_BIN): $(TERM_HOST_OBJS) $(LIBTSM_HOST_A)
	$(HOSTCC) -o $@ $(TERM_HOST_OBJS) $(LIBTSM_HOST_A)

-include $(TERM_OBJS:.o=.d) $(TERM_HOST_OBJS:.o=.d)
