CC     = gcc
CFLAGS = -O2 -static -Wall -Wextra -Wno-unused-parameter -s

all: fifi-wifi

fifi-wifi: wifi.c ../../shared/ipc.h ../../shared/app_ipc.h ../../shared/app_ui.h ../../shared/wifi_scan.h
	$(CC) $(CFLAGS) -o fifi-wifi wifi.c

clean:
	rm -f fifi-wifi
