CC     = gcc
CFLAGS = -O2 -static -Wall -Wextra -Wno-unused-parameter -s

all: fifi-wifi

fifi-wifi: wifi.c
	$(CC) $(CFLAGS) -o fifi-wifi wifi.c

clean:
	rm -f fifi-wifi
