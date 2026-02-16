#include <stdint.h>
#include "panic.h"
#include "serial.h"
#include "console.h"

__attribute__((noreturn))
void panic(const char *msg) {
    serial_write("\n--- FiFi OS PANIC ---\n");
    serial_write(msg);
    serial_write("\nSystem halted.\n");

    if (console_ready()) {
        console_set_colors(0x00FFFFFF, 0x00300000); /* white on dark red */
        console_clear();
        console_write("FiFi OS PANIC\n\n");
        console_set_colors(0x00FFFFFF, 0x00101010);
        console_write(msg);
        console_write("\n\nSystem halted.\n");
    }

    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}
