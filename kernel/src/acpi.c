#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "acpi.h"
#include "limine.h"
#include "kprintf.h"
#include "io.h"
#include "pmm.h"
#include "vmm.h"
#include "pit.h"
#include "pic.h"
#include "keyboard.h"
#include "serial.h"
#include "console.h"
#include "xhci.h"

/* Print exactly 4 characters (kprintf doesn't support %.4s) */
static void put4(const char *s) {
    for (int i = 0; i < 4 && s[i]; i++)
        kprintf("%c", s[i]);
}

/* Limine RSDP request */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0
};

typedef struct __attribute__((packed)) {
    char     sig[8];       /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_addr;

    /* ACPI 2.0+ */
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} rsdp_t;

typedef struct __attribute__((packed)) {
    char     sig[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} sdt_hdr_t;

static const rsdp_t *g_rsdp = 0;
static const sdt_hdr_t *g_xsdt = 0;
static const sdt_hdr_t *g_rsdt = 0;

static uint8_t sum_bytes(const void *p, uint32_t n) {
    const uint8_t *b = (const uint8_t*)p;
    uint32_t s = 0;
    for (uint32_t i = 0; i < n; i++) s += b[i];
    return (uint8_t)s;
}

static int sdt_ok(const sdt_hdr_t *h) {
    if (!h) return 0;
    if (h->length < sizeof(sdt_hdr_t)) return 0;
    return (sum_bytes(h, h->length) == 0);
}

void acpi_init(void) {
    struct limine_rsdp_response *resp = rsdp_request.response;
    if (!resp || !resp->address) {
        kprintf("FiFi OS: ACPI: no RSDP from Limine\n");
        return;
    }

    g_rsdp = (const rsdp_t*)resp->address;

    if (!g_rsdp) return;

    if (g_rsdp->sig[0] != 'R' || g_rsdp->sig[1] != 'S' || g_rsdp->sig[2] != 'D') {
        kprintf("FiFi OS: ACPI: bad RSDP signature\n");
        return;
    }

    /* checksum: ACPI 1.0 uses first 20 bytes, ACPI 2.0+ uses length */
    uint32_t len = 20;
    if (g_rsdp->revision >= 2 && g_rsdp->length >= 20) {
        len = g_rsdp->length;
    }
    if (sum_bytes(g_rsdp, len) != 0) {
        kprintf("FiFi OS: ACPI: RSDP checksum failed\n");
        return;
    }

    /* Prefer XSDT if available — addresses in ACPI tables are PHYSICAL,
     * must convert via HHDM to get usable virtual pointers */
    if (g_rsdp->revision >= 2 && g_rsdp->xsdt_addr) {
        g_xsdt = (const sdt_hdr_t*)pmm_phys_to_virt(g_rsdp->xsdt_addr);
        if (!sdt_ok(g_xsdt)) {
            kprintf("FiFi OS: ACPI: XSDT invalid checksum/size\n");
            g_xsdt = 0;
        }
    }

    if (!g_xsdt && g_rsdp->rsdt_addr) {
        g_rsdt = (const sdt_hdr_t*)pmm_phys_to_virt((uint64_t)g_rsdp->rsdt_addr);
        if (!sdt_ok(g_rsdt)) {
            kprintf("FiFi OS: ACPI: RSDT invalid checksum/size\n");
            g_rsdt = 0;
        }
    }

    if (g_xsdt) {
        kprintf("FiFi OS: ACPI: RSDP OK (rev=%p) using XSDT=%p\n",
                (void*)(uintptr_t)g_rsdp->revision, (void*)g_xsdt);
    } else if (g_rsdt) {
        kprintf("FiFi OS: ACPI: RSDP OK (rev=%p) using RSDT=%p\n",
                (void*)(uintptr_t)g_rsdp->revision, (void*)g_rsdt);
    } else {
        kprintf("FiFi OS: ACPI: RSDP OK but no valid XSDT/RSDT\n");
    }

}

void acpi_dump(void) {
    if (!g_rsdp) {
        kprintf("ACPI: not initialized\n");
        return;
    }

    kprintf("ACPI: RSDP=%p rev=%p oem=%.6s\n",
            (void*)g_rsdp, (void*)(uintptr_t)g_rsdp->revision, g_rsdp->oem_id);

    if (g_xsdt) {
        const sdt_hdr_t *h = g_xsdt;
        uint32_t entries = (h->length - sizeof(sdt_hdr_t)) / 8;
        const uint64_t *ptrs = (const uint64_t*)((const uint8_t*)h + sizeof(sdt_hdr_t));

        kprintf("ACPI: XSDT entries=%p\n", (void*)(uintptr_t)entries);

        for (uint32_t i = 0; i < entries; i++) {
            if (!ptrs[i]) continue;
            const sdt_hdr_t *th = (const sdt_hdr_t*)pmm_phys_to_virt(ptrs[i]);
            kprintf("  [%p] ", (void*)(uintptr_t)i);
            put4(th->sig);
            kprintf(" len=%p rev=%p phys=%p\n",
                    (void*)(uintptr_t)th->length,
                    (void*)(uintptr_t)th->revision,
                    (void*)(uintptr_t)ptrs[i]);
        }
        return;
    }

    if (g_rsdt) {
        const sdt_hdr_t *h = g_rsdt;
        uint32_t entries = (h->length - sizeof(sdt_hdr_t)) / 4;
        const uint32_t *ptrs = (const uint32_t*)((const uint8_t*)h + sizeof(sdt_hdr_t));

        kprintf("ACPI: RSDT entries=%p\n", (void*)(uintptr_t)entries);

        for (uint32_t i = 0; i < entries; i++) {
            if (!ptrs[i]) continue;
            const sdt_hdr_t *th = (const sdt_hdr_t*)pmm_phys_to_virt((uint64_t)ptrs[i]);
            kprintf("  [%p] ", (void*)(uintptr_t)i);
            put4(th->sig);
            kprintf(" len=%p rev=%p phys=%p\n",
                    (void*)(uintptr_t)th->length,
                    (void*)(uintptr_t)th->revision,
                    (void*)(uintptr_t)ptrs[i]);
        }
        return;
    }

    kprintf("ACPI: no valid XSDT/RSDT\n");
}

/* ── FADT (Fixed ACPI Description Table) ──────────────────────────────────── */

typedef struct __attribute__((packed)) {
    sdt_hdr_t header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved1;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;        /* SCI interrupt number */
    uint32_t smi_cmd;        /* I/O port for SMI command */
    uint8_t  acpi_enable;    /* value to write to smi_cmd to enable ACPI */
    uint8_t  acpi_disable;   /* value to write to smi_cmd to disable ACPI */
    /* ... many more fields, but we only need the above */
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;   /* PM1a control register I/O port */
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
} fadt_t;

/* Find an ACPI table by 4-byte signature */
static const sdt_hdr_t *acpi_find_table(const char *sig) {
    if (g_xsdt) {
        uint32_t entries = (g_xsdt->length - sizeof(sdt_hdr_t)) / 8;
        const uint64_t *ptrs = (const uint64_t *)((const uint8_t *)g_xsdt + sizeof(sdt_hdr_t));
        for (uint32_t i = 0; i < entries; i++) {
            if (!ptrs[i]) continue;
            const sdt_hdr_t *th = (const sdt_hdr_t *)pmm_phys_to_virt(ptrs[i]);
            if (th->sig[0] == sig[0] && th->sig[1] == sig[1] &&
                th->sig[2] == sig[2] && th->sig[3] == sig[3])
                return th;
        }
    } else if (g_rsdt) {
        uint32_t entries = (g_rsdt->length - sizeof(sdt_hdr_t)) / 4;
        const uint32_t *ptrs = (const uint32_t *)((const uint8_t *)g_rsdt + sizeof(sdt_hdr_t));
        for (uint32_t i = 0; i < entries; i++) {
            if (!ptrs[i]) continue;
            const sdt_hdr_t *th = (const sdt_hdr_t *)pmm_phys_to_virt((uint64_t)ptrs[i]);
            if (th->sig[0] == sig[0] && th->sig[1] == sig[1] &&
                th->sig[2] == sig[2] && th->sig[3] == sig[3])
                return th;
        }
    }
    return 0;
}

/* ── Embedded Controller (EC) interface ──────────────────────────────────── */

#define EC_DATA     0x62
#define EC_CMD_STS  0x66

/* EC status register bits */
#define EC_OBF      (1u << 0)   /* Output Buffer Full — data at EC_DATA */
#define EC_IBF      (1u << 1)   /* Input Buffer Full — EC busy          */
#define EC_SCI_EVT  (1u << 5)   /* SCI event pending                    */

/* EC commands */
#define EC_CMD_READ  0x80
#define EC_CMD_WRITE 0x81
#define EC_CMD_QUERY 0x84

/* Guard flag — prevents PIT handler (acpi_ec_poll) from reading EC ports
 * while we're in the middle of an EC command (RAM read/write/query).
 * Without this, the PIT handler steals OBF responses meant for our commands. */
static volatile bool ec_busy = false;

/* Forward declaration — polls detected EC keyboard RAM addresses */
static void ec_kbd_poll_tick(void);

static bool ec_wait_ibf_clear(void) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(EC_CMD_STS) & EC_IBF)) return true;
    return false;
}

static bool ec_wait_obf_set(void) {
    for (int i = 0; i < 100000; i++)
        if (inb(EC_CMD_STS) & EC_OBF) return true;
    return false;
}

/* Read one byte from EC RAM */
static uint8_t ec_ram_read(uint8_t addr) {
    ec_busy = true;
    ec_wait_ibf_clear();
    outb(EC_CMD_STS, EC_CMD_READ);
    ec_wait_ibf_clear();
    outb(EC_DATA, addr);
    ec_wait_obf_set();
    uint8_t val = inb(EC_DATA);
    ec_busy = false;
    return val;
}

/* Drain all pending EC events + output data. */
static void ec_drain_events(void) {
    ec_busy = true;
    /* Drain pending output data */
    for (int i = 0; i < 32; i++) {
        if (!(inb(EC_CMD_STS) & EC_OBF)) break;
        uint8_t d = inb(EC_DATA);
        kprintf("[ec] drain OBF: 0x%x\n", (unsigned)d);
    }

    /* Acknowledge pending SCI events via EC_QR (query) */
    for (int i = 0; i < 16; i++) {
        uint8_t st = inb(EC_CMD_STS);
        if (!(st & EC_SCI_EVT)) break;

        if (!ec_wait_ibf_clear()) { kprintf("[ec] IBF stuck\n"); break; }
        outb(EC_CMD_STS, EC_CMD_QUERY);
        if (!ec_wait_obf_set()) { kprintf("[ec] QR timeout\n"); break; }
        uint8_t qr = inb(EC_DATA);
        kprintf("[ec] query event=0x%x\n", (unsigned)qr);
    }
    ec_busy = false;
}

static void ec_dump_ram(uint8_t start, uint8_t count) {
    for (uint8_t a = start; a < start + count; a += 16) {
        kprintf("[ec] %02x:", (unsigned)a);
        for (uint8_t i = 0; i < 16 && (a + i) < (start + count); i++)
            kprintf(" %02x", (unsigned)ec_ram_read(a + i));
        kprintf("\n");
    }
}

void acpi_ec_drain(void) {
    kprintf("[ec] status=0x%x\n", (unsigned)inb(EC_CMD_STS));
    ec_drain_events();
    kprintf("[ec] EC RAM 0x00-0x3F:\n");
    ec_dump_ram(0x00, 0x40);
    kprintf("[ec] done, status=0x%x\n", (unsigned)inb(EC_CMD_STS));
}

/* Set SCI_EN directly in PM1_CNT, then drain EC and check PIT health.
 * Returns 0 if PIT died (caller must reinit PIC+PIT). */
int acpi_enable_sci_safe(void) {
    const fadt_t *fadt = (const fadt_t *)acpi_find_table("FACP");
    if (!fadt) {
        kprintf("[acpi] no FADT\n");
        return 1;
    }

    kprintf("[acpi] sci=%u pm1a=0x%x\n",
            (unsigned)fadt->sci_int, (unsigned)fadt->pm1a_cnt_blk);

    if (!fadt->pm1a_cnt_blk) return 1;

    uint16_t pm1 = inw(fadt->pm1a_cnt_blk);
    kprintf("[acpi] PM1_CNT=0x%x SCI_EN=%u\n", (unsigned)pm1, (unsigned)(pm1 & 1));

    if (!(pm1 & 1u)) {
        /* Set SCI_EN directly — NO SMI_CMD */
        outw(fadt->pm1a_cnt_blk, pm1 | 1u);
        /* Bounded delay for chipset stabilization */
        for (volatile int i = 0; i < 2000000; i++) __asm__ volatile("pause");
        pm1 = inw(fadt->pm1a_cnt_blk);
        kprintf("[acpi] PM1_CNT=0x%x SCI_EN=%u (set)\n",
                (unsigned)pm1, (unsigned)(pm1 & 1));
    } else {
        kprintf("[acpi] already ACPI mode\n");
    }

    /* Drain EC events generated by the SCI_EN transition */
    kprintf("[ec] post-SCI status=0x%x\n", (unsigned)inb(EC_CMD_STS));
    ec_drain_events();

    /* Check PIT health: see if ticks advance within a bounded spin */
    uint64_t t0 = pit_ticks();
    for (volatile int i = 0; i < 10000000; i++) __asm__ volatile("pause");
    uint64_t t1 = pit_ticks();

    if (t1 > t0) {
        kprintf("[acpi] PIT alive (%u->%u)\n", (unsigned)t0, (unsigned)t1);
        return 1;
    } else {
        kprintf("[acpi] PIT DEAD — needs reinit\n");
        return 0;
    }
}

/* Re-check 8042 config after SCI_EN — chipset may have altered it.
 * Restore IRQ1 + translation + re-enable scanning if needed. */
static void ps2_reinit_post_sci(void) {
    /* Read config byte */
    uint8_t cfg = 0;
    for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 0x02)) break;
    outb(0x64, 0x20);
    for (int i = 0; i < 100000; i++) if (inb(0x64) & 0x01) { cfg = inb(0x60); break; }
    kprintf("[ps2] post-SCI cfg=0x%x\n", (unsigned)cfg);

    /* Ensure IRQ1 + translation set, keyboard not disabled */
    uint8_t want = (cfg | (1u<<0) | (1u<<6)) & ~(1u<<4);
    if (cfg != want) {
        kprintf("[ps2] fixing cfg 0x%x->0x%x\n", (unsigned)cfg, (unsigned)want);
        for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 0x02)) break;
        outb(0x64, 0x60);
        for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 0x02)) break;
        outb(0x60, want);
    }

    /* Re-enable keyboard interface */
    for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 0x02)) break;
    outb(0x64, 0xAE);

    /* Re-enable scanning */
    for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 0x02)) break;
    outb(0x60, 0xF4);
    for (int i = 0; i < 100000; i++) {
        if (inb(0x64) & 0x01) {
            uint8_t r = inb(0x60);
            if (r == 0xFA) { kprintf("[ps2] scan ACK\n"); break; }
            if (r == 0xFE) { kprintf("[ps2] scan NACK\n"); break; }
        }
    }
}

/* SCI event counter for diagnostics */
static volatile uint32_t g_sci_count = 0;
static volatile uint32_t g_sci_kbd_count = 0;

/* GPE register info — set once during probe, used by SCI handler.
 * Without clearing GPE status bits, the chipset won't deliver further
 * GPE interrupts (level-triggered: stays asserted until acknowledged). */
static volatile uint16_t g_gpe0_sts_base = 0;  /* GPE0 status register base */
static volatile uint16_t g_gpe0_en_base  = 0;  /* GPE0 enable register base */
static volatile uint8_t  g_gpe0_half     = 0;  /* number of status/enable bytes */
/* EC GPE specifics — byte offset within GPE block and bitmask */
static volatile uint8_t  g_ec_gpe_byte   = 0;  /* byte index (e.g. 13 for GPE 0x6E) */
static volatile uint8_t  g_ec_gpe_mask   = 0;  /* bitmask (e.g. 0x40 for bit 6) */

/* ACPI SCI interrupt handler — called from ISR on IRQ 9.
 *
 * SCI is LEVEL-TRIGGERED. The chipset asserts SCI whenever any
 * (GPE status & enable) bit is set. To prevent a GPE storm:
 *   1. DISABLE the GPE enable bit (prevents re-trigger)
 *   2. Clear the GPE status bit (write-1-to-clear)
 *   3. Process the EC event (query to deassert EC's event line)
 *   4. RE-ENABLE the GPE enable bit
 * This is exactly what Linux's acpi_ev_gpe_dispatch() does. */
void acpi_sci_handler(void) {
    g_sci_count++;

    if (!g_gpe0_half) return;  /* not initialized yet */

    uint16_t sts_port = (uint16_t)(g_gpe0_sts_base + g_ec_gpe_byte);
    uint16_t en_port  = (uint16_t)(g_gpe0_en_base  + g_ec_gpe_byte);
    uint8_t  mask     = g_ec_gpe_mask;

    /* ── Step 1: Disable EC GPE + clear status (prevent re-trigger) ── */
    uint8_t en_val = inb(en_port);
    outb(en_port, (uint8_t)(en_val & ~mask));  /* disable EC GPE */
    outb(sts_port, mask);                       /* clear EC GPE status (w1c) */

    /* Clear any other fired GPE status bits too */
    for (uint8_t i = 0; i < g_gpe0_half; i++) {
        if (i == g_ec_gpe_byte) continue;  /* already handled */
        uint8_t sts = inb((uint16_t)(g_gpe0_sts_base + i));
        if (sts)
            outb((uint16_t)(g_gpe0_sts_base + i), sts);
    }

    /* ── Step 2: Process EC events ── */
    for (int round = 0; round < 8; round++) {
        uint8_t st = inb(EC_CMD_STS);

        /* Drain EC output buffer */
        if (st & EC_OBF)
            (void)inb(EC_DATA);

        /* Acknowledge SCI event via EC query command */
        if (st & EC_SCI_EVT) {
            if (!(inb(EC_CMD_STS) & EC_IBF)) {
                outb(EC_CMD_STS, EC_CMD_QUERY);
                for (int i = 0; i < 1000; i++) {
                    if (inb(EC_CMD_STS) & EC_OBF) {
                        (void)inb(EC_DATA);
                        break;
                    }
                }
            }
        }

        st = inb(EC_CMD_STS);
        if (!(st & EC_OBF) && !(st & EC_SCI_EVT))
            break;
    }

    /* ── Step 3: Check i8042 for keyboard data ── */
    for (int i = 0; i < 4; i++) {
        if (inb(0x64) & 0x01) {
            uint8_t sc = inb(0x60);
            if (sc != 0) {
                keyboard_on_scancode(sc);
                g_sci_kbd_count++;
            }
        } else {
            break;
        }
    }

    /* ── Step 4: Re-enable EC GPE ── */
    outb(en_port, (uint8_t)(inb(en_port) | mask));
}

/* Brief diagnostic: count SCI events for 3 seconds while user taps keys. */
void acpi_ec_keyboard_watch(void) {
    ps2_reinit_post_sci();

    kprintf("\n== SCI TEST 3s: TAP KEYS ==\n");

    uint32_t sci_before = g_sci_count;
    uint32_t kbd_before = g_sci_kbd_count;

    uint64_t start = pit_ticks();
    while ((pit_ticks() - start) < 300) {
        __asm__ volatile("hlt");  /* sleep until next interrupt */
    }

    uint32_t sci_delta = g_sci_count - sci_before;
    uint32_t kbd_delta = g_sci_kbd_count - kbd_before;

    kprintf("SCI: %u events, %u keyboard\n", sci_delta, kbd_delta);

    if (kbd_delta > 0)
        kprintf(">> KEYBOARD VIA SCI WORKING <<\n");
    else
        kprintf(">> NO SCI keyboard events <<\n");
}

/* Lightweight EC poll for PIT handler — backup drain only.
 * Primary keyboard path is now the SCI handler (IRQ 9). */
void acpi_ec_poll(void) {
    if (ec_busy) return;

    uint8_t st = inb(EC_CMD_STS);

    /* Drain SCI events FIRST (so OBF data below is keyboard, not query response) */
    if (st & EC_SCI_EVT) {
        if (!(st & EC_IBF)) {
            outb(EC_CMD_STS, EC_CMD_QUERY);
            for (int i = 0; i < 500; i++) {
                if (inb(EC_CMD_STS) & EC_OBF) {
                    (void)inb(EC_DATA);  /* discard query result */
                    break;
                }
            }
        }
        return;  /* don't read OBF this tick — it was a query response */
    }

    /* No SCI pending — check if EC has keyboard data in output buffer.
     * Only read if OBF is set and no command is in progress. */
    if ((st & EC_OBF) && !(st & EC_IBF)) {
        uint8_t data = inb(EC_DATA);
        if (data != 0x00 && data != 0xFF) {
            keyboard_on_scancode(data);
        }
    }
}

/* No-op — keyboard data comes from SCI handler now. */
void acpi_ec_kbd_check(void) {
    (void)0;
}

/* Full ACPI enable via SMI_CMD — proper firmware handoff.
 * This tells the firmware "I am an ACPI-aware OS" and may trigger
 * EC reconfiguration (including keyboard forwarding).
 * Returns: 1=success PIT alive, 0=success PIT dead, -1=error */
int acpi_enable_full_smi(void) {
    const fadt_t *fadt = (const fadt_t *)acpi_find_table("FACP");
    if (!fadt) { kprintf("[smi] no FADT\n"); return -1; }

    uint16_t pm1 = inw(fadt->pm1a_cnt_blk);
    kprintf("[smi] cmd=0x%x val=0x%x PM1=0x%x SCI_EN=%u\n",
            (unsigned)fadt->smi_cmd, (unsigned)fadt->acpi_enable,
            (unsigned)pm1, (unsigned)(pm1 & 1));

    if (pm1 & 1) {
        kprintf("[smi] already ACPI mode\n");
        return 1;
    }

    if (!fadt->smi_cmd) {
        kprintf("[smi] no SMI_CMD port\n");
        return -1;
    }

    kprintf("[smi] writing 0x%x to port 0x%x...\n",
            (unsigned)fadt->acpi_enable, (unsigned)fadt->smi_cmd);
    outb((uint16_t)fadt->smi_cmd, fadt->acpi_enable);

    /* Poll for SCI_EN */
    for (int i = 0; i < 50000000; i++) {
        __asm__ volatile("pause");
        pm1 = inw(fadt->pm1a_cnt_blk);
        if (pm1 & 1) break;
    }

    pm1 = inw(fadt->pm1a_cnt_blk);
    kprintf("[smi] post PM1=0x%x SCI_EN=%u\n", (unsigned)pm1, (unsigned)(pm1 & 1));

    if (!(pm1 & 1)) {
        kprintf("[smi] SCI_EN not set — forcing\n");
        outw(fadt->pm1a_cnt_blk, pm1 | 1u);
        for (volatile int i = 0; i < 2000000; i++) __asm__ volatile("pause");
    }

    /* Check PIT health */
    uint64_t t0 = pit_ticks();
    for (volatile int i = 0; i < 10000000; i++) __asm__ volatile("pause");
    uint64_t t1 = pit_ticks();

    if (t1 > t0) {
        kprintf("[smi] PIT alive (%u->%u)\n", (unsigned)t0, (unsigned)t1);
        return 1;
    }
    kprintf("[smi] PIT dead\n");
    return 0;
}

void acpi_dump_fadt(void) {
    const fadt_t *fadt = (const fadt_t *)acpi_find_table("FACP");
    if (!fadt) { kprintf("[fadt] not found\n"); return; }

    kprintf("[fadt] sci=%u smi=0x%x en=0x%x pm1a=0x%x\n",
            (unsigned)fadt->sci_int, (unsigned)fadt->smi_cmd,
            (unsigned)fadt->acpi_enable, (unsigned)fadt->pm1a_cnt_blk);
    kprintf("[fadt] gpe0=0x%x/%u gpe1=0x%x/%u\n",
            (unsigned)fadt->gpe0_blk, (unsigned)fadt->gpe0_blk_len,
            (unsigned)fadt->gpe1_blk, (unsigned)fadt->gpe1_blk_len);

    /* iapc_boot_arch at FADT offset 109 — tells us if 8042 really exists */
    if (fadt->header.length >= 111) {
        uint16_t ba = *(const uint16_t *)((const uint8_t *)fadt + 109);
        kprintf("[fadt] boot_arch=0x%x 8042=%s LEGACY=%s\n",
                (unsigned)ba,
                (ba & 2) ? "YES" : "NO",
                (ba & 1) ? "YES" : "NO");
    }
    /* FADT flags at offset 112 */
    if (fadt->header.length >= 116) {
        uint32_t fl = *(const uint32_t *)((const uint8_t *)fadt + 112);
        kprintf("[fadt] flags=0x%x HW_REDUCED=%s\n",
                (unsigned)fl, (fl & (1u << 20)) ? "YES" : "NO");
    }
}

void acpi_gpe_enable_all(void) {
    const fadt_t *fadt = (const fadt_t *)acpi_find_table("FACP");
    if (!fadt) return;

    if (fadt->gpe0_blk && fadt->gpe0_blk_len >= 2) {
        uint8_t half = fadt->gpe0_blk_len / 2;
        uint16_t sts_base = (uint16_t)fadt->gpe0_blk;
        uint16_t en_base  = (uint16_t)(fadt->gpe0_blk + half);

        kprintf("[gpe] sts=0x%x en=0x%x half=%u\n",
                (unsigned)sts_base, (unsigned)en_base, (unsigned)half);

        /* Clear pending status then enable all GPEs */
        for (uint8_t i = 0; i < half; i++)
            outb((uint16_t)(sts_base + i), 0xFF);
        for (uint8_t i = 0; i < half; i++)
            outb((uint16_t)(en_base + i), 0xFF);

        kprintf("[gpe] en:");
        for (uint8_t i = 0; i < half; i++)
            kprintf(" %x", (unsigned)inb((uint16_t)(en_base + i)));
        kprintf("\n");
    } else {
        kprintf("[gpe] no GPE0 block\n");
    }

    if (fadt->gpe1_blk && fadt->gpe1_blk_len >= 2) {
        uint8_t half = fadt->gpe1_blk_len / 2;
        for (uint8_t i = 0; i < half; i++)
            outb((uint16_t)(fadt->gpe1_blk + i), 0xFF);
        for (uint8_t i = 0; i < half; i++)
            outb((uint16_t)(fadt->gpe1_blk + half + i), 0xFF);
        kprintf("[gpe] GPE1 enabled\n");
    }
}

void acpi_gpe_dump_status(void) {
    const fadt_t *fadt = (const fadt_t *)acpi_find_table("FACP");
    if (!fadt || !fadt->gpe0_blk || fadt->gpe0_blk_len < 2) {
        kprintf("GPE=N/A");
        return;
    }
    uint8_t half = fadt->gpe0_blk_len / 2;
    kprintf("GPE:");
    for (uint8_t i = 0; i < half; i++)
        kprintf("%x", (unsigned)inb((uint16_t)(fadt->gpe0_blk + i)));
}

/* Scan DSDT for _REG methods — find what variable they store Arg1 to.
 * This tells us the EC "available" flag name. */
void acpi_dsdt_scan_ec(void) {
    const fadt_t *fadt = (const fadt_t *)acpi_find_table("FACP");
    if (!fadt) return;

    /* Get DSDT physical address — try x_dsdt (offset 140) first */
    uint64_t dsdt_phys = 0;
    if (fadt->header.length >= 148)
        dsdt_phys = *(const uint64_t *)((const uint8_t *)fadt + 140);
    if (!dsdt_phys)
        dsdt_phys = (uint64_t)fadt->dsdt;
    if (!dsdt_phys) { kprintf("[dsdt] none\n"); return; }

    const uint8_t *d = (const uint8_t *)pmm_phys_to_virt(dsdt_phys);
    const sdt_hdr_t *hdr = (const sdt_hdr_t *)d;
    uint32_t len = hdr->length;
    kprintf("[dsdt] p=%x len=%u\n", (unsigned)(dsdt_phys & 0xFFFFFFFF), (unsigned)len);

    /* Scan for _REG method pattern: MethodOp(0x14) ... "_REG" */
    int found = 0;
    for (uint32_t i = 4; i + 32 <= len; i++) {
        if (d[i] != '_' || d[i+1] != 'R' || d[i+2] != 'E' || d[i+3] != 'G')
            continue;

        /* Verify preceded by MethodOp within 8 bytes */
        bool is_method = false;
        for (int j = 1; j <= 8 && i >= (uint32_t)j; j++)
            if (d[i - j] == 0x14) { is_method = true; break; }
        if (!is_method) continue;

        found++;
        kprintf("[dsdt] _REG#%d @%x", found, (unsigned)i);

        /* Scan body for Store(Arg1, Name): 0x70 0x69 NAME[4] */
        for (uint32_t j = i + 5; j + 6 <= len && j < i + 128; j++) {
            if (d[j] != 0x70 || d[j+1] != 0x69) continue;
            char name[5] = {0};
            bool ok = true;
            for (int k = 0; k < 4; k++) {
                char c = (char)d[j + 2 + k];
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
                    name[k] = c;
                else { ok = false; break; }
            }
            if (ok) kprintf(" ->%s", name);
        }

        /* Hex dump: 20 bytes after _REG */
        kprintf("\n ");
        for (uint32_t j = i; j < i + 20 && j < len; j++)
            kprintf("%x ", (unsigned)d[j]);
        kprintf("\n");
    }

    if (!found) kprintf("[dsdt] no _REG found\n");

    /* Scan for EC device: EISAID("PNP0C09") = 0x41 0xD0 0x0C 0x09 */
    for (uint32_t i = 0; i + 4 <= len; i++) {
        if (d[i] == 0x41 && d[i+1] == 0xD0 && d[i+2] == 0x0C && d[i+3] == 0x09)
            kprintf("[dsdt] EC(PNP0C09) @%x\n", (unsigned)i);
    }
    /* Also check ASCII form */
    for (uint32_t i = 0; i + 7 <= len; i++) {
        if (d[i]=='P' && d[i+1]=='N' && d[i+2]=='P' && d[i+3]=='0' &&
            d[i+4]=='C' && d[i+5]=='0' && d[i+6]=='9')
            kprintf("[dsdt] EC(ASCII) @%x\n", (unsigned)i);
    }
}

/* Forward declarations for AML helpers and field map (defined later in file) */
static uint32_t aml_pkg_len(const uint8_t *d, uint32_t *pos, uint32_t max);
static bool aml_is_name(const uint8_t *d, uint32_t pos, uint32_t max);
static const char *ec_field_name_at(uint8_t addr);

/* ── _REG method AML dump ──────────────────────────────────────────────── *
 * Find every _REG method in the DSDT, decode its PkgLength to get the     *
 * full method body, and hex-dump it.  Also do inline AML annotation for    *
 * common opcodes so we can understand what _REG does:                      *
 *   0x70 = Store, 0x69 = Arg1, 0x68 = Arg0, 0x14 = Method,               *
 *   0xA0 = If, 0xA1 = Else, 0xA4 = Return, 0x5B80 = OpRegion,            *
 *   0x08 = Name, 0x0A = ByteConst, 0x0B = WordConst, 0x0C = DWordConst   */
void acpi_dump_reg_aml(void) {
    const fadt_t *fadt = (const fadt_t *)acpi_find_table("FACP");
    if (!fadt) { kprintf("[reg] no FADT\n"); return; }

    uint64_t dsdt_phys = 0;
    if (fadt->header.length >= 148)
        dsdt_phys = *(const uint64_t *)((const uint8_t *)fadt + 140);
    if (!dsdt_phys) dsdt_phys = (uint64_t)fadt->dsdt;
    if (!dsdt_phys) { kprintf("[reg] no DSDT\n"); return; }

    const uint8_t *d = (const uint8_t *)pmm_phys_to_virt(dsdt_phys);
    uint32_t len = ((const sdt_hdr_t *)d)->length;

    int found = 0;
    for (uint32_t i = 0; i + 8 <= len; i++) {
        /* Look for MethodOp (0x14) followed by PkgLen then "_REG" */
        if (d[i] != 0x14) continue;

        /* Try to decode PkgLength and check for _REG name */
        uint32_t pos = i + 1;
        uint32_t pkg_start = pos;
        uint32_t pkg_len = aml_pkg_len(d, &pos, len);
        if (pkg_len < 8 || pkg_start + pkg_len > len) continue;
        uint32_t method_end = pkg_start + pkg_len;

        /* Name should be right after PkgLength */
        if (pos + 4 > len) continue;
        if (d[pos] != '_' || d[pos+1] != 'R' || d[pos+2] != 'E' || d[pos+3] != 'G')
            continue;
        pos += 4;

        /* Skip method flags byte (arg count + serialize flags) */
        if (pos >= method_end) continue;
        uint8_t mflags = d[pos++];

        found++;
        kprintf("\n=== _REG #%d ===\n", found);
        kprintf("@0x%x len=%u args=%u\n",
                (unsigned)i, (unsigned)pkg_len, (unsigned)(mflags & 7));

        /* Hex dump of method body (after flags) — 16 bytes per line */
        uint32_t body_start = pos;
        uint32_t body_len = method_end - pos;
        kprintf("body (%u bytes):\n", (unsigned)body_len);

        for (uint32_t off = 0; off < body_len; off++) {
            if (off % 16 == 0) kprintf(" %03x:", (unsigned)off);
            kprintf(" %02x", (unsigned)d[body_start + off]);
            if (off % 16 == 15 || off == body_len - 1) kprintf("\n");
        }

        /* Extract ALL 4-char names from the method body.
         * Also decode key opcodes for context. Print large and clear. */
        kprintf("\nOPS:\n");
        uint32_t p = body_start;
        while (p < method_end) {
            uint8_t op = d[p];

            if (op == 0xA0 && p + 1 < method_end) {
                /* If — decode predicate inline */
                p++;
                uint32_t plen = aml_pkg_len(d, &p, method_end);
                (void)plen;
                /* Check for LEqual as predicate */
                if (p < method_end && d[p] == 0x93) {
                    p++; /* skip LEqual op */
                    /* Operand 1 */
                    kprintf("  IF ");
                    if (p < method_end && d[p] == 0x68) { kprintf("Arg0"); p++; }
                    else if (p < method_end && d[p] == 0x69) { kprintf("Arg1"); p++; }
                    else if (p < method_end && d[p] == 0x0A && p+1 < method_end) {
                        kprintf("%x", (unsigned)d[p+1]); p += 2;
                    } else if (p < method_end && aml_is_name(d, p, method_end)) {
                        put4((const char*)&d[p]); p += 4;
                    } else if (p < method_end) { kprintf("[%x]", (unsigned)d[p]); p++; }
                    kprintf(" == ");
                    /* Operand 2 */
                    if (p < method_end && d[p] == 0x68) { kprintf("Arg0"); p++; }
                    else if (p < method_end && d[p] == 0x69) { kprintf("Arg1"); p++; }
                    else if (p < method_end && d[p] == 0x0A && p+1 < method_end) {
                        kprintf("%x", (unsigned)d[p+1]); p += 2;
                    } else if (p < method_end && d[p] == 0x00) { kprintf("0"); p++; }
                    else if (p < method_end && d[p] == 0x01) { kprintf("1"); p++; }
                    else if (p < method_end && aml_is_name(d, p, method_end)) {
                        put4((const char*)&d[p]); p += 4;
                    } else if (p < method_end) { kprintf("[%x]", (unsigned)d[p]); p++; }
                    kprintf("\n");
                } else {
                    kprintf("  IF [%x]\n", p < method_end ? (unsigned)d[p] : 0u);
                }
            } else if (op == 0xA1 && p + 1 < method_end) {
                kprintf("  ELSE\n");
                p++;
                (void)aml_pkg_len(d, &p, method_end);
            } else if (op == 0xA4) {
                kprintf("  RETURN\n");
                p++;
            } else if (op == 0x70) {
                /* Store(Source, Dest) — THE critical operation */
                p++;
                kprintf("  STORE ");
                /* Source */
                if (p < method_end && d[p] == 0x69) { kprintf("Arg1"); p++; }
                else if (p < method_end && d[p] == 0x68) { kprintf("Arg0"); p++; }
                else if (p < method_end && d[p] == 0x0A && p+1 < method_end) {
                    kprintf("%x", (unsigned)d[p+1]); p += 2;
                } else if (p < method_end && d[p] == 0x00) { kprintf("0"); p++; }
                else if (p < method_end && d[p] == 0x01) { kprintf("1"); p++; }
                else if (p < method_end && d[p] == 0xFF) { kprintf("FF"); p++; }
                else if (p < method_end && aml_is_name(d, p, method_end)) {
                    put4((const char*)&d[p]); p += 4;
                } else if (p < method_end) { kprintf("[%x]", (unsigned)d[p]); p++; }
                kprintf(" -> ");
                /* Dest */
                if (p < method_end && aml_is_name(d, p, method_end)) {
                    put4((const char*)&d[p]);
                    p += 4;
                } else if (p < method_end) { kprintf("[%x]", (unsigned)d[p]); p++; }
                kprintf("\n");
            } else if (aml_is_name(d, p, method_end)) {
                /* Method call */
                kprintf("  CALL "); put4((const char*)&d[p]); kprintf("\n");
                p += 4;
            } else {
                /* Skip unknown opcodes silently */
                p++;
            }
        }

        /* Also list ALL unique names found in raw scan */
        kprintf("\nNAMES:");
        for (uint32_t off = body_start; off + 4 <= method_end; off++) {
            if (aml_is_name(d, off, method_end)) {
                /* Avoid printing sub-names of opcodes we already decoded */
                kprintf(" "); put4((const char*)&d[off]);
            }
        }
        kprintf("\n");
    }

    if (!found) kprintf("[reg] no _REG methods found\n");
    kprintf("[reg] total=%d\n", found);
}

/* Write a byte to EC RAM */
static void ec_ram_write(uint8_t addr, uint8_t val) {
    ec_busy = true;
    ec_wait_ibf_clear();
    outb(EC_CMD_STS, EC_CMD_WRITE);
    ec_wait_ibf_clear();
    outb(EC_DATA, addr);
    ec_wait_ibf_clear();
    outb(EC_DATA, val);
    ec_busy = false;
}

/* DISABLED — blind EC RAM writes caused laptop shutdown/BIOS recovery.
 * DO NOT write to EC RAM without knowing the exact DSDT layout. */
void acpi_ec_try_keyboard_enable(void) {
    kprintf("[ec] EC RAM writes DISABLED (safety)\n");
}

/* ── AML parsing helpers for DSDT auto-reg ─────────────────────────────── */

/* Decode AML PkgLength at d[*pos]. Returns value, advances *pos past it. */
static uint32_t aml_pkg_len(const uint8_t *d, uint32_t *pos, uint32_t max) {
    if (*pos >= max) return 0;
    uint8_t lead = d[(*pos)++];
    uint8_t extra = (lead >> 6) & 3;
    if (extra == 0) return lead & 0x3F;
    uint32_t val = lead & 0x0F;
    for (uint8_t i = 0; i < extra && *pos < max; i++)
        val |= (uint32_t)d[(*pos)++] << (4 + 8 * i);
    return val;
}

/* Check if d[pos..pos+3] is a valid AML NameSeg. */
static bool aml_is_name(const uint8_t *d, uint32_t pos, uint32_t max) {
    if (pos + 4 > max) return false;
    char c = (char)d[pos];
    if (!((c >= 'A' && c <= 'Z') || c == '_')) return false;
    for (int i = 1; i < 4; i++) {
        c = (char)d[pos + i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

/* Skip an AML NameString at d[*pos], extract last NameSeg into out[0..3].
 * Handles root/parent prefixes and dual/multi name paths. */
static bool aml_read_namestring(const uint8_t *d, uint32_t *pos, uint32_t max,
                                char out[4]) {
    while (*pos < max && (d[*pos] == 0x5C || d[*pos] == 0x5E))
        (*pos)++;
    if (*pos >= max) return false;
    if (d[*pos] == 0x00) { (*pos)++; return false; }
    if (d[*pos] == 0x2E) {                /* DualNamePath */
        (*pos)++;
        if (*pos + 8 > max) return false;
        *pos += 4;
        if (!aml_is_name(d, *pos, max)) return false;
        for (int i = 0; i < 4; i++) out[i] = (char)d[*pos + i];
        *pos += 4;
        return true;
    }
    if (d[*pos] == 0x2F) {                /* MultiNamePath */
        (*pos)++;
        if (*pos >= max) return false;
        uint8_t cnt = d[(*pos)++];
        if (!cnt || *pos + (uint32_t)cnt * 4 > max) return false;
        *pos += (cnt - 1) * 4;
        if (!aml_is_name(d, *pos, max)) return false;
        for (int i = 0; i < 4; i++) out[i] = (char)d[*pos + i];
        *pos += 4;
        return true;
    }
    if (aml_is_name(d, *pos, max)) {
        for (int i = 0; i < 4; i++) out[i] = (char)d[*pos + i];
        *pos += 4;
        return true;
    }
    return false;
}

/* ── Auto-emulate _REG(EmbeddedControl, 1) by parsing the DSDT ────────── *
 * 1. Find _REG methods, extract Store(Arg1, Name) targets                  *
 * 2. Find OperationRegions with space=EmbeddedControl, record names         *
 * 3. Walk Field defs for EC regions, locate target's byte offset            *
 * 4. Write 1 to that single EC RAM address                                  *
 * Returns 1 on success, 0 on failure.                                       */
int acpi_dsdt_auto_reg(void) {
    const fadt_t *fadt = (const fadt_t *)acpi_find_table("FACP");
    if (!fadt) { kprintf("[autoreg] no FADT\n"); return 0; }

    uint64_t dsdt_phys = 0;
    if (fadt->header.length >= 148)
        dsdt_phys = *(const uint64_t *)((const uint8_t *)fadt + 140);
    if (!dsdt_phys) dsdt_phys = (uint64_t)fadt->dsdt;
    if (!dsdt_phys) { kprintf("[autoreg] no DSDT\n"); return 0; }

    const uint8_t *d = (const uint8_t *)pmm_phys_to_virt(dsdt_phys);
    uint32_t len = ((const sdt_hdr_t *)d)->length;
    kprintf("[autoreg] DSDT len=%u\n", (unsigned)len);

    /* ── 1. Collect Store(Arg1, Name) targets from _REG methods ── */
    char targets[4][5];
    int ntgt = 0;
    for (int t = 0; t < 4; t++) targets[t][0] = 0;

    for (uint32_t i = 4; i + 32 <= len; i++) {
        if (d[i]!='_'||d[i+1]!='R'||d[i+2]!='E'||d[i+3]!='G') continue;
        bool meth = false;
        for (int j = 1; j <= 8 && i >= (uint32_t)j; j++)
            if (d[i-j] == 0x14) { meth = true; break; }
        if (!meth) continue;

        for (uint32_t j = i+5; j+6 <= len && j < i+128; j++) {
            if (d[j] != 0x70 || d[j+1] != 0x69) continue;
            if (!aml_is_name(d, j+2, len)) continue;
            if (ntgt < 4) {
                for (int k=0; k<4; k++) targets[ntgt][k] = (char)d[j+2+k];
                targets[ntgt][4] = 0;
                kprintf("[autoreg] _REG#%d -> %s\n", ntgt+1, targets[ntgt]);
                ntgt++;
            }
            break;
        }
    }
    if (!ntgt) { kprintf("[autoreg] no _REG targets\n"); return 0; }

    /* ── 2. Find EmbeddedControl OperationRegions ── */
    char ec_rgn[4][5];
    int nrgn = 0;
    for (int r = 0; r < 4; r++) ec_rgn[r][0] = 0;

    for (uint32_t i = 0; i + 10 <= len; i++) {
        if (d[i] != 0x5B || d[i+1] != 0x80) continue;
        uint32_t pos = i + 2;
        char nm[4] = {0};
        if (!aml_read_namestring(d, &pos, len, nm)) continue;
        if (pos >= len || d[pos] != 0x03) continue;
        if (nrgn < 4) {
            for (int k=0; k<4; k++) ec_rgn[nrgn][k] = nm[k];
            ec_rgn[nrgn][4] = 0;
            kprintf("[autoreg] EC region=%s\n", ec_rgn[nrgn]);
            nrgn++;
        }
    }
    if (!nrgn) { kprintf("[autoreg] no EC region\n"); return 0; }

    /* ── 3. Walk Field (0x81), IndexField (0x86), BankField (0x87) ── *
     * Search ALL definitions — target name is unique enough to match.
     * IndexField is common on Lenovo: EC vars accessed via index/data pair. */
    int fields_scanned = 0;
    for (uint32_t i = 0; i + 10 <= len; i++) {
        if (d[i] != 0x5B) continue;
        int ftype = 0;
        if (d[i+1] == 0x81) ftype = 1;       /* Field: 1 NameString */
        else if (d[i+1] == 0x86) ftype = 2;  /* IndexField: 2 NameStrings */
        else if (d[i+1] == 0x87) ftype = 3;  /* BankField: 2 NameStrings + val */
        else continue;

        uint32_t pos = i + 2;
        uint32_t pstart = pos;
        uint32_t plen = aml_pkg_len(d, &pos, len);
        uint32_t fend = pstart + plen;
        if (fend > len || plen < 6) continue;

        /* Skip header NameString(s) */
        char rn[4] = {0};
        if (!aml_read_namestring(d, &pos, fend, rn)) continue;
        if (ftype >= 2) {
            char rn2[4] = {0};
            if (!aml_read_namestring(d, &pos, fend, rn2)) continue;
        }
        if (ftype == 3) {
            /* BankField: skip BankValue integer */
            if (pos >= fend) continue;
            uint8_t op = d[pos];
            if (op == 0x00 || op == 0x01 || op == 0xFF) pos++;
            else if (op == 0x0A && pos+1 < fend) pos += 2;
            else if (op == 0x0B && pos+2 < fend) pos += 3;
            else if (op == 0x0C && pos+4 < fend) pos += 5;
            else continue;
        }

        if (pos >= fend) continue;
        pos++;                              /* skip FieldFlags byte */
        fields_scanned++;

        /* Check if this field references an EC region (for logging) */
        bool is_ec = false;
        for (int r = 0; r < nrgn; r++)
            if (rn[0]==ec_rgn[r][0] && rn[1]==ec_rgn[r][1] &&
                rn[2]==ec_rgn[r][2] && rn[3]==ec_rgn[r][3])
                { is_ec = true; break; }

        uint32_t bitoff = 0;
        while (pos < fend) {
            uint8_t el = d[pos];
            if (el == 0x00) {               /* ReservedField — skip bits */
                pos++;
                bitoff += aml_pkg_len(d, &pos, fend);
            } else if (el == 0x01) {        /* AccessField */
                pos += 3;
            } else if (el == 0x03) {        /* ExtendedAccessField */
                pos += 4;
            } else if (aml_is_name(d, pos, fend)) {
                char fn[5];
                for (int k=0; k<4; k++) fn[k] = (char)d[pos+k];
                fn[4] = 0;
                pos += 4;
                uint32_t bw = aml_pkg_len(d, &pos, fend);

                for (int t = 0; t < ntgt; t++) {
                    if (fn[0]==targets[t][0] && fn[1]==targets[t][1] &&
                        fn[2]==targets[t][2] && fn[3]==targets[t][3]) {
                        uint32_t byteoff = bitoff / 8;
                        kprintf("[autoreg] FOUND %s @ 0x%x (%ub) "
                                "ftype=%d ec=%d rgn=",
                                fn, (unsigned)byteoff, (unsigned)bw,
                                ftype, is_ec);
                        put4(rn); kprintf("\n");
                        ec_ram_write((uint8_t)byteoff, 1);
                        kprintf("[autoreg] EC[0x%x]=1 DONE\n",
                                (unsigned)byteoff);
                        return 1;
                    }
                }
                bitoff += bw;
            } else {
                break;
            }
        }
    }

    kprintf("[autoreg] target not found (%d fields scanned)\n",
            fields_scanned);
    return 0;
}

/* ── Dump ALL named fields from EC-region definitions ─────────────────────
 * Walks every Field (0x81) and IndexField (0x86) that references an
 * EmbeddedControl OperationRegion.  Collects all fields, sends full
 * list to serial, and prints keyboard-related fields prominently
 * on screen with their current EC RAM values.                            */

/* File-scope EC field map — populated by acpi_ec_field_dump(),
 * read by acpi_ec_ram_diff() to cross-reference changing addresses. */
#define ECFLD_MAX 128
static char    g_ecfld_name[ECFLD_MAX][5];
static uint8_t g_ecfld_off[ECFLD_MAX];
static uint8_t g_ecfld_bits[ECFLD_MAX];
static int     g_ecfld_count = 0;

/* Look up DSDT field name for a given EC RAM byte offset.
 * Returns pointer to 4-char name or NULL if not named. */
static const char *ec_field_name_at(uint8_t addr) {
    for (int i = 0; i < g_ecfld_count; i++)
        if (g_ecfld_off[i] == addr) return g_ecfld_name[i];
    return 0;
}

/* Check if a 4-char field name looks keyboard-related */
static bool ec_name_is_kbd(const char n[4]) {
    /* KB anywhere */
    for (int i = 0; i < 3; i++)
        if ((n[i]=='K'||n[i]=='k') && (n[i+1]=='B'||n[i+1]=='b')) return true;
    /* SC (scan) */
    for (int i = 0; i < 3; i++)
        if ((n[i]=='S'||n[i]=='s') && (n[i+1]=='C'||n[i+1]=='c')) return true;
    /* KEY */
    for (int i = 0; i < 2; i++)
        if (n[i]=='K' && n[i+1]=='E' && n[i+2]=='Y') return true;
    /* Specific known Lenovo/common EC keyboard field names */
    if (n[0]=='K' && n[1]=='D') return true;  /* KDxx */
    if (n[0]=='H' && n[1]=='K') return true;  /* HKxx (hotkey) */
    if (n[0]=='K' && n[1]=='S') return true;  /* KSxx (key status) */
    if (n[0]=='K' && n[1]=='C') return true;  /* KCxx */
    return false;
}

void acpi_ec_field_dump(void) {
    const fadt_t *fadt = (const fadt_t *)acpi_find_table("FACP");
    if (!fadt) { kprintf("[ecfld] no FADT\n"); return; }

    uint64_t dsdt_phys = 0;
    if (fadt->header.length >= 148)
        dsdt_phys = *(const uint64_t *)((const uint8_t *)fadt + 140);
    if (!dsdt_phys) dsdt_phys = (uint64_t)fadt->dsdt;
    if (!dsdt_phys) { kprintf("[ecfld] no DSDT\n"); return; }

    const uint8_t *d = (const uint8_t *)pmm_phys_to_virt(dsdt_phys);
    uint32_t len = ((const sdt_hdr_t *)d)->length;

    /* Step 1: collect EmbeddedControl OperationRegion names */
    char ec_rgn[8][5];
    int nrgn = 0;
    for (uint32_t i = 0; i + 10 <= len; i++) {
        if (d[i] != 0x5B || d[i+1] != 0x80) continue;
        uint32_t pos = i + 2;
        char nm[4] = {0};
        if (!aml_read_namestring(d, &pos, len, nm)) continue;
        if (pos >= len || d[pos] != 0x03) continue;  /* 0x03 = EmbeddedControl */
        if (nrgn < 8) {
            for (int k = 0; k < 4; k++) ec_rgn[nrgn][k] = nm[k];
            ec_rgn[nrgn][4] = 0;
            kprintf("[ecfld] EC region: "); put4(ec_rgn[nrgn]); kprintf("\n");
            nrgn++;
        }
    }
    if (!nrgn) { kprintf("[ecfld] no EC regions found\n"); return; }

    /* Step 2: collect all fields into file-scope arrays (used by diff too) */
    g_ecfld_count = 0;

    for (uint32_t i = 0; i + 10 <= len; i++) {
        if (d[i] != 0x5B) continue;
        int ftype = 0;
        if (d[i+1] == 0x81) ftype = 1;
        else if (d[i+1] == 0x86) ftype = 2;
        else if (d[i+1] == 0x87) ftype = 3;
        else continue;

        uint32_t pos = i + 2;
        uint32_t pstart = pos;
        uint32_t plen = aml_pkg_len(d, &pos, len);
        uint32_t fend = pstart + plen;
        if (fend > len || plen < 6) continue;

        char rn[4] = {0};
        if (!aml_read_namestring(d, &pos, fend, rn)) continue;
        if (ftype >= 2) {
            char rn2[4] = {0};
            if (!aml_read_namestring(d, &pos, fend, rn2)) continue;
        }
        if (ftype == 3) {
            if (pos >= fend) continue;
            uint8_t op = d[pos];
            if (op == 0x00 || op == 0x01 || op == 0xFF) pos++;
            else if (op == 0x0A && pos+1 < fend) pos += 2;
            else if (op == 0x0B && pos+2 < fend) pos += 3;
            else if (op == 0x0C && pos+4 < fend) pos += 5;
            else continue;
        }

        bool is_ec = false;
        for (int r = 0; r < nrgn; r++) {
            if (rn[0]==ec_rgn[r][0] && rn[1]==ec_rgn[r][1] &&
                rn[2]==ec_rgn[r][2] && rn[3]==ec_rgn[r][3])
                { is_ec = true; break; }
        }
        if (!is_ec) continue;

        if (pos >= fend) continue;
        pos++;  /* skip FieldFlags */

        uint32_t bitoff = 0;
        while (pos < fend) {
            uint8_t el = d[pos];
            if (el == 0x00) {
                pos++;
                bitoff += aml_pkg_len(d, &pos, fend);
            } else if (el == 0x01) {
                pos += 3;
            } else if (el == 0x03) {
                pos += 4;
            } else if (aml_is_name(d, pos, fend)) {
                char fn[5];
                for (int k = 0; k < 4; k++) fn[k] = (char)d[pos+k];
                fn[4] = 0;
                pos += 4;
                uint32_t bw = aml_pkg_len(d, &pos, fend);

                if (g_ecfld_count < ECFLD_MAX) {
                    for (int k = 0; k < 5; k++) g_ecfld_name[g_ecfld_count][k] = fn[k];
                    g_ecfld_off[g_ecfld_count] = (uint8_t)(bitoff / 8);
                    g_ecfld_bits[g_ecfld_count] = (uint8_t)bw;
                    g_ecfld_count++;
                }
                bitoff += bw;
            } else {
                break;
            }
        }
    }

    /* Pass 1: full list to serial (not screen) */
    serial_write("[ecfld] ALL EC FIELDS:\n");
    for (int i = 0; i < g_ecfld_count; i++) {
        /* Build line manually for serial */
        char buf[32];
        buf[0] = ' '; buf[1] = ' ';
        for (int k = 0; k < 4; k++) buf[2+k] = g_ecfld_name[i][k];
        buf[6] = '@'; buf[7] = '0'; buf[8] = 'x';
        /* hex byte */
        uint8_t hi = g_ecfld_off[i] >> 4, lo = g_ecfld_off[i] & 0xF;
        buf[9]  = (char)(hi < 10 ? '0'+hi : 'A'+hi-10);
        buf[10] = (char)(lo < 10 ? '0'+lo : 'A'+lo-10);
        buf[11] = '\n'; buf[12] = 0;
        serial_write(buf);
    }

    /* Screen output is minimal — just count. Full list went to serial. */
    kprintf("[ecfld] %d fields from %d EC regions\n", g_ecfld_count, nrgn);
}

/* ── Probe v21: Fixed ECMM page offset + GNVS setup (no SMI) ──────── *
 * CRITICAL BUG FOUND: vmm_map_page page-aligns physical addresses,     *
 * stripping the low 12 bits. ECMM base is 0xFE0B0400 (0x400 into page *
 * 0xFE0B0000). ALL ECMM accesses since v13 were 0x400 bytes WRONG!     *
 *                                                                        *
 * Fix: add 0x400 offset to all ECMM accesses.                           *
 * Also: set GNVS (OSYS, ECON) + correct ECMM fields (OSTY, ECRD).     *
 * No SMI this time — keep PIT alive, test if correct ECMM writes alone  *
 * enable keyboard forwarding.                                            */

/* Local i8042 helpers (ports 0x60/0x64) */
static bool p60_wait_write(void) {
    for (int i = 0; i < 100000; i++)
        if (!(inb(0x64) & 0x02)) return true;
    return false;
}
static void p60_flush(void) {
    for (int i = 0; i < 32; i++) {
        if (!(inb(0x64) & 0x01)) break;
        (void)inb(0x60);
    }
}

/* ── TSC helpers ── */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t tsc_calibrate(void) {
    uint64_t t0 = pit_ticks();
    while (pit_ticks() == t0) {}
    uint64_t tsc0 = rdtsc();
    t0 = pit_ticks();
    while (pit_ticks() - t0 < 100) {}
    return rdtsc() - tsc0;
}

static void tsc_delay_ms(uint64_t tsc_hz, uint64_t ms) {
    uint64_t target = rdtsc() + (tsc_hz * ms / 1000);
    while (rdtsc() < target) {}
}

static int tsc_kbd_poll(uint64_t tsc_hz, int secs) {
    uint64_t deadline = rdtsc() + (uint64_t)secs * tsc_hz;
    int keys = 0;
    while (rdtsc() < deadline) {
        if (inb(0x64) & 0x01) {
            kprintf("K:%02x ", (unsigned)inb(0x60));
            keys++;
            if (keys >= 30) break;
        }
    }
    return keys;
}

/* ── SSDT KMON scanner ── */

/* Search a byte range for a 4-byte pattern.
 * Returns offset from start, or -1 if not found. */
static int mem_find4(const uint8_t *buf, int len, const uint8_t pat[4]) {
    for (int i = 0; i <= len - 4; i++) {
        if (buf[i] == pat[0] && buf[i+1] == pat[1] &&
            buf[i+2] == pat[2] && buf[i+3] == pat[3])
            return i;
    }
    return -1;
}

/* Scan all SSDTs in XSDT for KMON method body */
static void scan_ssdts_for_kmon(void) {
    if (!g_xsdt) { kprintf("  no XSDT\n"); return; }

    uint32_t entries = (g_xsdt->length - sizeof(sdt_hdr_t)) / 8;
    const uint64_t *ptrs = (const uint64_t *)((const uint8_t *)g_xsdt + sizeof(sdt_hdr_t));
    const uint8_t kmon_pat[4] = { 'K', 'M', 'O', 'N' };
    const uint8_t kmof_pat[4] = { 'K', 'M', 'O', 'F' };
    int ssdt_count = 0;

    for (uint32_t i = 0; i < entries; i++) {
        if (!ptrs[i]) continue;
        const sdt_hdr_t *th = (const sdt_hdr_t *)pmm_phys_to_virt(ptrs[i]);
        if (th->sig[0] != 'S' || th->sig[1] != 'S' ||
            th->sig[2] != 'D' || th->sig[3] != 'T') continue;

        ssdt_count++;
        const uint8_t *body = (const uint8_t *)th;
        int len = (int)th->length;

        kprintf("  SSDT#%d len=%d oem=%.6s tbl=%.8s\n",
                ssdt_count, len, th->oem_id, th->oem_table_id);

        /* Search for KMON */
        int off = mem_find4(body, len, kmon_pat);
        if (off >= 0) {
            kprintf("  >>> KMON at offset %d <<<\n  ", off);
            /* Dump 64 bytes before and 96 after for context */
            int start = off - 32;
            if (start < 0) start = 0;
            int end = off + 96;
            if (end > len) end = len;
            for (int j = start; j < end; j++) {
                kprintf("%02x", (unsigned)body[j]);
                if ((j - start + 1) % 32 == 0) kprintf("\n  ");
                else kprintf(" ");
            }
            kprintf("\n");
        }

        /* Search for KMOF */
        off = mem_find4(body, len, kmof_pat);
        if (off >= 0) {
            kprintf("  >>> KMOF at offset %d <<<\n  ", off);
            int start = off - 16;
            if (start < 0) start = 0;
            int end = off + 64;
            if (end > len) end = len;
            for (int j = start; j < end; j++) {
                kprintf("%02x", (unsigned)body[j]);
                if ((j - start + 1) % 32 == 0) kprintf("\n  ");
                else kprintf(" ");
            }
            kprintf("\n");
        }
    }
    kprintf("  scanned %d SSDTs\n", ssdt_count);
}

/* ── ECCD command interface (ECMM+0x220) ── */
/* This is how DSDT AML talks to the EC via memory-mapped commands.
 * ECTB (bit 0 at +0x222): task busy flag
 * ECTE (bit 1 at +0x222): task execute flag
 * ECMD (+0x22B): command byte
 * EDT1-EDT5 (+0x22C-0x230): command data
 * ERN1-ERN8 (+0x223-0x22A): return data */

static int eccd_cmd(volatile uint8_t *ecmm, uint8_t cmd,
                     uint8_t d1, uint8_t d2, uint8_t d3) {
    volatile uint8_t *flags = ecmm + 0x222;
    /* Set task busy */
    *flags = (*flags & 0xFC) | 0x01;  /* ECTB=1, ECTE=0 */
    ecmm[0x22B] = cmd;
    ecmm[0x22C] = d1;
    ecmm[0x22D] = d2;
    ecmm[0x22E] = d3;
    ecmm[0x22F] = 0;
    ecmm[0x230] = 0;
    /* Execute */
    *flags = (*flags & 0xFC) | 0x03;  /* ECTB=1, ECTE=1 */
    /* Wait for ECTE to clear (EC completed) — up to 1 second */
    for (int i = 0; i < 100000; i++) {
        if (!(*flags & 0x02)) {
            /* Done — clear busy */
            *flags &= 0xFE;
            return 1;
        }
        for (volatile int d = 0; d < 100; d++) {}  /* ~10us delay */
    }
    *flags &= 0xFC;  /* timeout — clear both */
    return 0;
}

/* Read EC extended address via ECCD (command 0xB0) */
static uint8_t eccd_read(volatile uint8_t *ecmm, uint8_t addr) {
    eccd_cmd(ecmm, 0xB0, addr, 0, 0);
    return ecmm[0x223]; /* ERN1 */
}

/* Write EC extended address via ECCD (command 0xB1) */
static void eccd_write(volatile uint8_t *ecmm, uint8_t addr, uint8_t val) {
    eccd_cmd(ecmm, 0xB1, addr, val, 0);
}

/* EIDR/EIDW emulation — read/write EC registers through ECCD command
 * interface, exactly like DSDT methods do. This path goes through the
 * EC's command processor, which may trigger internal notifications
 * that direct ECMM writes don't. */
static uint8_t eidr(volatile uint8_t *ecmm, uint16_t addr) {
    /* ERIB at ECMM+0x5D (16-bit) = target address */
    *(volatile uint16_t *)(ecmm + 0x5D) = addr;
    /* ECCD command 0xB0, param=0x5F */
    eccd_cmd(ecmm, 0xB0, 0x5F, 0, 0);
    return ecmm[0x223]; /* ERN1 = result */
}

static void eidw(volatile uint8_t *ecmm, uint16_t addr, uint8_t val) {
    *(volatile uint16_t *)(ecmm + 0x5D) = addr;
    eccd_cmd(ecmm, 0xB1, 0x5F, val, 0);
}

/* Compact EC RAM dump — values 0x00-0x3F on 2 lines */
void acpi_ec_dump_compact(void) {
    kprintf("[ec] 00:");
    for (uint8_t a = 0; a < 0x20; a++)
        kprintf("%x ", (unsigned)ec_ram_read(a));
    kprintf("\n[ec] 20:");
    for (uint8_t a = 0x20; a < 0x40; a++)
        kprintf("%x ", (unsigned)ec_ram_read(a));
    kprintf("\n");
}

/* ── EC RAM keyboard detection ─────────────────────────────────────────── *
 * Three-phase approach (all READ-ONLY, safe):                               *
 *   Phase 0: baseline snapshot                                              *
 *   Phase 1: user presses keys — snapshot — find changes                    *
 *   Phase 2: user hands off   — snapshot — find background changes          *
 * Keyboard bytes = changed in phase 1 but NOT in phase 2.                   *
 *                                                                           *
 * Also does rapid multi-sampling during phase 1 to count change frequency.  *
 * Keyboard bytes change many times; thermal bytes change ≤1 time.           */

/* Detected keyboard EC RAM addresses — set by detection, polled by PIT */
#define EC_KBD_MAX 16
static uint8_t  ec_kbd_addrs[EC_KBD_MAX];
static int      ec_kbd_count = 0;
static uint8_t  ec_kbd_prev[EC_KBD_MAX];
static volatile bool ec_kbd_polling = false;

void acpi_ec_ram_diff(void) {
    static uint16_t chg[256];
    uint8_t before[256], after[256];

    /* Phase 0: baseline — snapshot BEFORE key mashing */
    kprintf("[ec] baseline...\n");
    for (int a = 0; a < 256; a++) {
        before[a] = ec_ram_read((uint8_t)a);
        after[a] = before[a];
        chg[a] = 0;
    }

    /* Clear screen so countdown is unmissable */
    console_clear();

    kprintf("\n\n\n");
    kprintf("=============================\n");
    for (int s = 5; s >= 1; s--) {
        kprintf("  MASH KEYS IN %d...\n", s);
        uint64_t t = pit_ticks();
        while (pit_ticks() - t < 100) __asm__ volatile("hlt");
    }
    kprintf("\n");
    kprintf("  >>>>>>>>>>>>>>>>>>>>>>>>\n");
    kprintf("  >>> MASH KEYS NOW!  <<<\n");
    kprintf("  >>>>>>>>>>>>>>>>>>>>>>>>\n");
    kprintf("\n  Sampling: ");

    /* Phase 1: sample EC RAM while user mashes keys.
     * 20 samples over ~8 seconds — long window, visual progress. */
    for (int s = 0; s < 20; s++) {
        kprintf(".");
        uint64_t t = pit_ticks();
        while (pit_ticks() - t < 40) __asm__ volatile("hlt");
        for (int a = 0; a < 256; a++) {
            uint8_t v = ec_ram_read((uint8_t)a);
            if (v != after[a]) chg[a]++;
            after[a] = v;
        }
    }
    kprintf(" DONE!\n\n");

    /* Show ALL addresses that changed — name + before/after.
     * Any address that changed AT ALL is interesting. */
    kprintf("=== EC RAM CHANGES ===\n");
    int total_changed = 0;
    ec_kbd_count = 0;

    for (int a = 0; a < 256; a++) {
        if (chg[a] == 0 && before[a] == after[a]) continue;
        /* Even chg=0 but before!=after means it changed once and
         * happened to be at the new value during all our samples */
        int changed = (chg[a] > 0) || (before[a] != after[a]);
        if (!changed) continue;

        const char *name = ec_field_name_at((uint8_t)a);
        kprintf(" %02x: %02x->%02x x%u",
                (unsigned)a,
                (unsigned)before[a], (unsigned)after[a],
                (unsigned)chg[a]);
        if (name) { kprintf(" "); put4(name); }
        kprintf("\n");
        total_changed++;

        if (chg[a] >= 2 && ec_kbd_count < EC_KBD_MAX) {
            ec_kbd_addrs[ec_kbd_count] = (uint8_t)a;
            ec_kbd_prev[ec_kbd_count] = after[a];
            ec_kbd_count++;
        }
    }

    kprintf("changed=%d kbd=%d\n", total_changed, ec_kbd_count);

    ec_kbd_polling = false;
}

/* PIT-callable: poll detected EC keyboard addresses, feed changes
 * to the keyboard scancode decoder. Called from acpi_ec_poll().
 * Only accepts values that look like valid PS/2 set 1 scancodes
 * to avoid feeding thermal noise as garbage characters. */
static void ec_kbd_poll_tick(void) {
    if (!ec_kbd_polling || ec_kbd_count == 0) return;

    for (int i = 0; i < ec_kbd_count; i++) {
        uint8_t v = ec_ram_read(ec_kbd_addrs[i]);
        if (v == ec_kbd_prev[i]) continue;   /* no change */
        ec_kbd_prev[i] = v;
        if (v == 0x00 || v == 0xFF) continue; /* cleared / invalid */

        /* Accept PS/2 set 1: make 0x01-0x58, break 0x81-0xD8, E0 prefix */
        if (v == 0xE0 || (v >= 0x01 && v <= 0x58) ||
            (v >= 0x81 && v <= 0xD8)) {
            keyboard_on_scancode(v);
        }
        /* Also try as direct ASCII if printable (fallback) */
        else if (v >= 0x20 && v <= 0x7E) {
            keyboard_push_char(v);
        }
    }
}
