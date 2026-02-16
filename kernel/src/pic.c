#include "pic.h"
#include "io.h"

/* PIC ports */
#define PIC1        0x20
#define PIC2        0xA0
#define PIC1_CMD    PIC1
#define PIC1_DATA   (PIC1 + 1)
#define PIC2_CMD    PIC2
#define PIC2_DATA   (PIC2 + 1)

#define ICW1_INIT   0x10
#define ICW1_ICW4   0x01
#define ICW4_8086   0x01

static uint16_t pic_get_mask(void) {
    uint8_t m1 = inb(PIC1_DATA);
    uint8_t m2 = inb(PIC2_DATA);
    return (uint16_t)m1 | ((uint16_t)m2 << 8);
}

static void pic_set_mask_all(uint16_t mask) {
    outb(PIC1_DATA, (uint8_t)(mask & 0xFF));
    outb(PIC2_DATA, (uint8_t)((mask >> 8) & 0xFF));
}

void pic_send_eoi(uint8_t irq) {
    /* If IRQ came from slave PIC, we must ACK slave then master */
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}

void pic_set_mask(uint8_t irq) {
    uint16_t mask = pic_get_mask();
    mask |= (1u << irq);
    pic_set_mask_all(mask);
}

void pic_clear_mask(uint8_t irq) {
    uint16_t mask = pic_get_mask();
    mask &= ~(1u << irq);
    pic_set_mask_all(mask);
}

void pic_disable(void) {
    /* Mask all IRQs */
    pic_set_mask_all(0xFFFF);
}

void pic_remap(uint8_t offset1, uint8_t offset2) {
    /* Save current masks */
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);

    /* Start init sequence */
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);

    /* Set vector offsets */
    outb(PIC1_DATA, offset1);
    outb(PIC2_DATA, offset2);

    /* Tell Master about Slave at IRQ2, tell Slave its cascade identity */
    outb(PIC1_DATA, 0x04);
    outb(PIC2_DATA, 0x02);

    /* 8086 mode */
    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);

    /* Restore masks */
    outb(PIC1_DATA, a1);
    outb(PIC2_DATA, a2);
}
