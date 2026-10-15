# Makefile: Doom for Rach2, the engine of src/ and the platform layer beside
# it, built as one freestanding static program of the architecture
# mk/arch.mk names: ELF32 i386 into build/ports/doom/ or ELF64 x86-64 into
# build/x86_64/ports/doom/.

# The cross toolchain, the header dependencies and the output tree (../../mk/).
include ../../mk/toolchain.mk
include ../../mk/deps.mk
TOP = ../..
COMPONENT = ports/doom
include ../../mk/build.mk
CC := $(CROSS_CC)

# The engine's sources, doomgeneric's.
DOOMGENERIC_DIR = src

# Compiler flags
CFLAGS = $(ARCH_CFLAGS) -nostdlib -static -ffreestanding -Wl,-Ttext=$(ARCH_USER_TEXT) -Wl,--build-id=none $(ARCH_LDFLAGS)
# The headers of include/ come first, in place of a C library's.
CFLAGS += -I./include -I$(DOOMGENERIC_DIR) -I.
CFLAGS += -DNORMALUNIX -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200
CFLAGS += -Os -fomit-frame-pointer -fno-strict-aliasing
CFLAGS += -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-function-declaration
# C99, since C23 makes keywords of false and true, which doomtype.h defines.
CFLAGS += -std=gnu99
# The sound and music modules of i_pcsound.c.
CFLAGS += -DFEATURE_SOUND
# Two macros that no source of the port reads.
CFLAGS += -DSNDSERV=0 -D_DEFAULT_SOURCE
# Automatic header-dependency tracking (see ../../mk/deps.mk).
CFLAGS += $(DEPFLAGS)

# Output binary (under $(BUILD) = build/ports/doom/, from mk/build.mk)
OUTPUT = $(BUILD)/doom

# The engine's sources: every .c file of src/.
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

# The platform layer, the sound modules and the music player.
LOCAL_SRC = doomgeneric_rach2.c i_pcsound.c opl3.c mus2mid.c midifile.c i_oplmusic.c
LOCAL_OBJS = $(addprefix $(BUILD)/, $(LOCAL_SRC:.c=.o))

# All objects
OBJS = $(BUILD)/crt0.o $(LOCAL_OBJS) $(DOOM_OBJS)

.PHONY: all clean

all: $(OUTPUT)

# Link
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

# Compile the engine's sources
$(BUILD)/%.o: $(DOOMGENERIC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Header-dependency files GCC wrote (-MMD); a header edit rebuilds its
# includers.
-include $(OBJS:.o=.d)

clean:
	rm -rf $(BUILD)
