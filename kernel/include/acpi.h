#pragma once
#include <stdint.h>

void acpi_init(void);
void acpi_dump(void);

void acpi_ec_drain(void);
int acpi_enable_sci_safe(void);
void acpi_ec_keyboard_watch(void);
void acpi_ec_poll(void);

/* SCI interrupt handler — called from ISR on IRQ 9.
 * Reads EC output buffer (keyboard scancodes) and acknowledges SCI events. */
void acpi_sci_handler(void);

/* Main-context EC keyboard poll — called from keyboard_try_getchar(). */
void acpi_ec_kbd_check(void);

/* Full ACPI enable via SMI_CMD — proper firmware handoff. */
int acpi_enable_full_smi(void);

/* Print key FADT fields. */
void acpi_dump_fadt(void);

/* Enable all ACPI GPE bits (for EC event delivery). */
void acpi_gpe_enable_all(void);

/* Print GPE status registers inline. */
void acpi_gpe_dump_status(void);

/* Scan DSDT for _REG methods to find EC "available" flag name/location. */
void acpi_dsdt_scan_ec(void);

/* Try writing 1 to common Lenovo EC "available" flag addresses.
 * This emulates what ACPI _REG(EmbeddedControl, 1) does. */
void acpi_ec_try_keyboard_enable(void);

/* Auto-emulate _REG(EmbeddedControl, 1) by parsing DSDT AML.
 * Finds the EC flag variable, resolves its offset, writes 1.
 * Returns 1 on success, 0 on failure. */
int acpi_dsdt_auto_reg(void);

/* Dump ALL named fields from EC OperationRegion Field/IndexField definitions.
 * Prints each field's 4-char name and byte offset — use to find keyboard fields. */
void acpi_ec_field_dump(void);

/* Compact EC RAM hex dump (one line). */
void acpi_ec_dump_compact(void);

/* EC RAM diff — snapshot before/after 5s, show changes.
 * READ-ONLY. Tells us if EC sees key presses at all. */
void acpi_ec_ram_diff(void);

/* Dump _REG method AML bytecode for manual disassembly.
 * Shows the full method body so we can see what _REG does
 * beyond just Store(Arg1, Flag) — e.g., EC writes, method calls. */
void acpi_dump_reg_aml(void);

