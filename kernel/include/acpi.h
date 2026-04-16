#pragma once
#include <stdint.h>

void acpi_init(void);

void acpi_ec_drain(void);
void acpi_ec_poll(void);

/* SCI interrupt handler — called from ISR on IRQ 9. */
void acpi_sci_handler(void);
