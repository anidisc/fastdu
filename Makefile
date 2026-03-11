CC = gcc
CFLAGS = -O2 -Wall -Wextra -pedantic -std=c11 $(EXTRA_CFLAGS)
LDFLAGS = -lncursesw -lpthread -luring -larchive
TARGET = fastdu
SRC = fastdu.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

asan:
	$(MAKE) EXTRA_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined" all

clean:
	rm -f $(TARGET)
