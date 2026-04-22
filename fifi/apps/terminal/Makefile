CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS = -static -s
TARGET  = fifi-terminal

$(TARGET): terminal.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "[terminal] built: $@"

clean:
	rm -f $(TARGET)

.PHONY: clean
