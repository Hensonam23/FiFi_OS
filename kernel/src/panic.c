#include "panic.h"
#include "serial.h"

__attribute__((noreturn))
void panic(const char *msg) {
    serial_write("\n--- FiFi OS PANIC ---\n");
    serial_write(msg);
    serial_write("\nSystem halted.\n");
    for (;;) __asm__ __volatile__("hlt");
}
