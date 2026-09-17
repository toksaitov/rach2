# Doom for Rach2 - Makefile
#
# Builds Doom using doomgeneric for the Rach2 kernel (freestanding ELF32).

# Shared cross toolchain + automatic header-dependency tracking (see ../../mk/).
include ../../mk/toolchain.mk
include ../../mk/deps.mk
TOP = ../..
COMPONENT = ports/doom
include ../../mk/build.mk
CC := $(CROSS_CC)

# Doomgeneric source directory (self-contained in src/)
DOOMGENERIC_DIR = src

# Compiler flags
CFLAGS = -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -Wl,--build-id=none
# Our stub headers MUST come first to override system headers
CFLAGS += -I./include -I$(DOOMGENERIC_DIR) -I.
CFLAGS += -DNORMALUNIX -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200
CFLAGS += -Os -fomit-frame-pointer -fno-strict-aliasing
CFLAGS += -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-function-declaration
# Use C99 to avoid issues with bool/true/false as keywords in newer C standards
CFLAGS += -std=gnu99
# Enable PC speaker sound support
CFLAGS += -DFEATURE_SOUND
# Disable features we don't support
CFLAGS += -DSNDSERV=0 -D_DEFAULT_SOURCE
# Automatic header-dependency tracking (see ../../mk/deps.mk).
CFLAGS += $(DEPFLAGS)

# Output binary (under $(BUILD) = build/ports/doom/, from mk/build.mk)
OUTPUT = $(BUILD)/doom

# Doom source files (from doomgeneric)
DOOM_SRC = \
	dummy.c am_map.c doomdef.c doomstat.c dstrings.c d_event.c d_items.c \
	d_iwad.c d_loop.c d_main.c d_mode.c d_net.c f_finale.c f_wipe.c g_game.c \
	hu_lib.c hu_stuff.c info.c i_cdmus.c i_endoom.c i_joystick.c i_scale.c \
	i_sound.c i_system.c i_timer.c memio.c m_argv.c m_bbox.c m_cheat.c \
	m_config.c m_controls.c m_fixed.c m_menu.c m_misc.c m_random.c \
	p_ceilng.c p_doors.c p_enemy.c p_floor.c p_inter.c p_lights.c p_map.c \
	p_maputl.c p_mobj.c p_plats.c p_pspr.c p_saveg.c p_setup.c p_sight.c \
	p_spec.c p_switch.c p_telept.c p_tick.c p_user.c r_bsp.c r_data.c \
	r_draw.c r_main.c r_plane.c r_segs.c r_sky.c r_things.c sha1.c sounds.c \
	statdump.c st_lib.c st_stuff.c s_sound.c tables.c v_video.c wi_stuff.c \
	w_checksum.c w_file.c w_main.c w_wad.c z_zone.c w_file_stdc.c \
	i_input.c i_video.c doomgeneric.c

# Object files (under $(BUILD), mirroring the source tree)
DOOM_OBJS = $(addprefix $(BUILD)/, $(DOOM_SRC:.c=.o))

# Local source files
LOCAL_SRC = doomgeneric_rach2.c i_pcsound.c opl3.c mus2mid.c midifile.c i_oplmusic.c
LOCAL_OBJS = $(addprefix $(BUILD)/, $(LOCAL_SRC:.c=.o))

# All objects
OBJS = $(BUILD)/crt0.o $(LOCAL_OBJS) $(DOOM_OBJS)

.PHONY: all clean

all: $(OUTPUT)

# Build output
$(OUTPUT): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^

# Compile crt0
$(BUILD)/crt0.o: crt0.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Compile local sources
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Compile doom sources from doomgeneric directory
$(BUILD)/%.o: $(DOOMGENERIC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Header-dependency files GCC wrote (-MMD); a header edit rebuilds its
# includers.
-include $(OBJS:.o=.d)

clean:
	rm -rf $(BUILD)
