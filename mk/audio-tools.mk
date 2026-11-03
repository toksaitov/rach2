# mk/audio-tools.mk: the resampler's tools under tools/audio/, which `all`
# does not build: mkresample, which writes the committed table, and
# resamplebench, which tools/audio/resamplebench.sh builds for each
# architecture and runs. The root Makefile includes this file and lists both
# among the collection tools, so the compilation database names them.

AUDIO_MKRESAMPLE = build/tools/audio/mkresample
AUDIO_RESAMPLEBENCH = build/tools/audio/resamplebench-i686 build/tools/audio/resamplebench-x86_64

$(AUDIO_MKRESAMPLE): tools/audio/mkresample.c
	@mkdir -p $(dir $@)
	$(HOST_LINK) -ansi -lm

# Each kernel compiler at no optimization with the kernel's code generation
# flags, but for x86-64 the small code model, since the program runs at a
# user address.
build/tools/audio/resamplebench-i686: BENCH_CC = i686-elf-gcc -m32 -mno-sse -mno-sse2
build/tools/audio/resamplebench-x86_64: BENCH_CC = x86_64-elf-gcc -m64 -mno-red-zone -mno-mmx \
    -mno-sse -mno-sse2 -fno-asynchronous-unwind-tables
$(AUDIO_RESAMPLEBENCH): tools/audio/resamplebench.c rach2/sys/audio/resample.c \
    rach2/sys/audio/resample.h rach2/sys/audio/resample_coef.h
	@mkdir -p $(dir $@)
	$(BENCH_CC) -O0 -ansi -Wall -Wextra -ffreestanding -fno-pie -nostdlib -static \
	    -iquote rach2/sys -o $@ tools/audio/resamplebench.c
