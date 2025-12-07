CFLAGS += -m32 -mno-sse -mno-sse2 -Wall -Wextra -O2

UTILITIES = true  \
            false \
            echo  \
            yes   \
            cat   \
            pwd   \
            touch \
            mkdir \
            rm    \
            mv    \
            cp    \
            ls

.PHONY : all
all : $(UTILITIES)

% : %.c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY : clean
clean :
	rm -f $(UTILITIES)
