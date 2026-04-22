CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS = -static -s
TARGET  = fifi-editor

$(TARGET): editor.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "[editor] built: $@"

clean:
	rm -f $(TARGET)

.PHONY: clean
