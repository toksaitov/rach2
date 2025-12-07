CFLAGS  += -m32 -mno-sse -mno-sse2 -g -O1 -std=gnu99 -Wno-unused-result
LDFLAGS += -m32 -static

TARGET  = ish
OBJECTS = ish.o                   \
          ish_shell_utilities.o   \
          ish_cstring_utilities.o
SPECIAL_OBJECTS = ish_syscalls.o

.PHONY : all
all : $(TARGET)

$(TARGET) : $(OBJECTS) $(SPECIAL_OBJECTS)

$(OBJECTS) : %.o : %.c ish_common.h

$(SPECIAL_OBJECTS) : %.o : %.c ish_syscalls.i386.c ish_common.h

.PHONY : clean
clean :
	rm -rf $(TARGET) $(OBJECTS) $(SPECIAL_OBJECTS)
