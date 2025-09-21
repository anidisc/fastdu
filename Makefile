CC = gcc
CFLAGS = -O2 -Wall -Wextra -pedantic -std=c11
LDFLAGS = -lncursesw -lpthread
TARGET = fastdu
SRC = fastdu.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)
