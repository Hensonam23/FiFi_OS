CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS = -static -s
TARGET  = fifi-aichat

$(TARGET): aichat.c ../../shared/ipc.h ../../shared/app_ipc.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "[aichat] built: $@"

clean:
	rm -f $(TARGET)

.PHONY: clean
