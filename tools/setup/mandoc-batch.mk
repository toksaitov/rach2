# Native Windows file conversion; upstream parsers and formatters unchanged.
override MAIN_OBJS = $(MANDOC_HTML_OBJS) $(filter-out term_tag.o,$(MANDOC_TERM_OBJS)) \
    batch_main.o batch_mmap.o batch_strptime.o out.o
override MANDOC_COBJS = compat_err.o compat_getline.o compat_getsubopt.o compat_ohash.o \
    compat_progname.o compat_reallocarray.o compat_recallocarray.o compat_strcasestr.o \
    compat_strlcat.o compat_strlcpy.o compat_strsep.o compat_strtonum.o
include Makefile
