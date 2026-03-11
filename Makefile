CC = gcc
TARGET = fastdu
SRC = fastdu.c

# Detect if we are on a Raspberry Pi
IS_RPI := $(shell grep -q "Raspberry Pi" /proc/device-tree/model 2>/dev/null && echo yes || echo no)

ifeq ($(IS_RPI),yes)
    # Raspberry Pi specific flags
    CFLAGS = -O2 -Wall -Wextra -std=gnu11 $(EXTRA_CFLAGS)
else
    # Default flags for other systems
    CFLAGS = -O2 -Wall -Wextra -pedantic -std=c11 $(EXTRA_CFLAGS)
endif

LDFLAGS = -lncursesw -lpthread -luring -larchive

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

asan:
	$(MAKE) EXTRA_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined" all

clean:
	rm -f $(TARGET)
