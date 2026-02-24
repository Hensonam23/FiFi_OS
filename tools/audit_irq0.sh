#!/usr/bin/env bash
set -euo pipefail

echo "== Where pit_on_irq0 is referenced =="
grep -RIn --line-number "pit_on_irq0" kernel | sed -n '1,120p' || true
echo

echo "== Where g_pit_ticks is referenced =="
grep -RIn --line-number "g_pit_ticks" kernel | sed -n '1,200p' || true
echo

echo "== Where pit_get_ticks is referenced =="
grep -RIn --line-number "pit_get_ticks" kernel | sed -n '1,200p' || true
echo

echo "== IRQ dispatch hotspots in isr.c (vector 32 / IRQ0) =="
grep -nE "case[[:space:]]+(32|0x20)\b|IRQ0|irq[[:space:]]*==[[:space:]]*0|-[[:space:]]*32" kernel/arch/x86_64/idt/isr.c || true
echo

echo "== Safety: isr.c must NOT read 0x60 =="
grep -n "inb(0x60" kernel/arch/x86_64/idt/isr.c || true
