#include <stdint.h>
#include <stddef.h>

#include "acpi.h"
#include "limine.h"
#include "kprintf.h"

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

    /* Prefer XSDT if available */
    if (g_rsdp->revision >= 2 && g_rsdp->xsdt_addr) {
        g_xsdt = (const sdt_hdr_t*)(uintptr_t)g_rsdp->xsdt_addr;
        if (!sdt_ok(g_xsdt)) {
            kprintf("FiFi OS: ACPI: XSDT invalid checksum/size\n");
            g_xsdt = 0;
        }
    }

    if (!g_xsdt && g_rsdp->rsdt_addr) {
        g_rsdt = (const sdt_hdr_t*)(uintptr_t)g_rsdp->rsdt_addr;
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
            const sdt_hdr_t *th = (const sdt_hdr_t*)(uintptr_t)ptrs[i];
            if (!th) continue;
            kprintf("  [%p] %.4s len=%p rev=%p at=%p\n",
                    (void*)(uintptr_t)i,
                    th->sig,
                    (void*)(uintptr_t)th->length,
                    (void*)(uintptr_t)th->revision,
                    (void*)th);
        }
        return;
    }

    if (g_rsdt) {
        const sdt_hdr_t *h = g_rsdt;
        uint32_t entries = (h->length - sizeof(sdt_hdr_t)) / 4;
        const uint32_t *ptrs = (const uint32_t*)((const uint8_t*)h + sizeof(sdt_hdr_t));

        kprintf("ACPI: RSDT entries=%p\n", (void*)(uintptr_t)entries);

        for (uint32_t i = 0; i < entries; i++) {
            const sdt_hdr_t *th = (const sdt_hdr_t*)(uintptr_t)ptrs[i];
            if (!th) continue;
            kprintf("  [%p] %.4s len=%p rev=%p at=%p\n",
                    (void*)(uintptr_t)i,
                    th->sig,
                    (void*)(uintptr_t)th->length,
                    (void*)(uintptr_t)th->revision,
                    (void*)th);
        }
        return;
    }

    kprintf("ACPI: no valid XSDT/RSDT\n");
}
