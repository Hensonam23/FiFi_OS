CC = gcc
CFLAGS = -O2 -static -Wall -Wextra -Wno-unused-parameter -s

all: fifi-security

fifi-security: security.c ../../shared/ipc.h ../../shared/app_ipc.h ../../shared/app_ui.h
	$(CC) $(CFLAGS) -o fifi-security security.c

clean:
	rm -f fifi-security
