# vendor/libtsm/sources.mk: the one list of libtsm's C files that are compiled.
# LIBTSM is the top of the extracted tree, which the includer sets: on Rach2,
# term/guest.mk includes this file as /src/term/vendor/libtsm.mk and unpacks
# the tree into vendor/libtsm. build_libtsm.sh reads the list without the
# prefix.
LIBTSM_SRC = $(LIBTSM)/src/tsm/tsm-render.c $(LIBTSM)/src/tsm/tsm-screen.c $(LIBTSM)/src/tsm/tsm-selection.c $(LIBTSM)/src/tsm/tsm-unicode.c $(LIBTSM)/src/tsm/tsm-vte-charsets.c $(LIBTSM)/src/tsm/tsm-vte.c $(LIBTSM)/src/shared/shl-htable.c $(LIBTSM)/external/wcwidth/wcwidth.c
