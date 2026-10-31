# term/sources.mk: the one list of term's sources, read by mk/term.mk and,
# on Rach2, by the guest build, so that the two builds cannot drift apart.
TERM_SRC = main.c pty.c screen.c draw.c font.c unifont.c keys.c keymap.c select.c host.c
