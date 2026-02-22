#include <stdint.h>
#include <stddef.h>

#include "limine.h"
#include "kprintf.h"
#include "initrd.h"

/* Limine module request (initrd/modules) */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0
};

#define INITRD_MAX_FILES 256

struct initrd_entry {
    const char *name;         // points into the initrd archive memory
    const uint8_t *data;      // points into the initrd archive memory
    uint64_t size;
    uint32_t mode;            // unix mode bits from cpio header
};

static struct {
    int ready;
    const void *base;
    uint64_t size;
    uint64_t count;
    struct initrd_entry files[INITRD_MAX_FILES];
} g_initrd;

static int streq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == '\0' && *b == '\0');
}

static uint32_t hex8_u32(const char *s) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return 0;
    }
    return v;
}

static int memeq6(const char *a, const char *b) {
    for (int i = 0; i < 6; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static uint64_t align4_u64(uint64_t x) {
    return (x + 3ULL) & ~3ULL;
}

static int is_newc_magic(const void *base, uint64_t size) {
    if (!base || size < 6) return 0;
    const char *m = (const char *)base;
    return memeq6(m, "070701") || memeq6(m, "070702");
}

static void initrd_index_newc(const void *base, uint64_t size) {
    g_initrd.ready = 0;
    g_initrd.base = base;
    g_initrd.size = size;
    g_initrd.count = 0;

    const uint8_t *p = (const uint8_t *)base;
    const uint8_t *end = p + size;

    while (p + 110 <= end) {
        const char *hdr = (const char *)p;

        if (!(memeq6(hdr, "070701") || memeq6(hdr, "070702"))) {
            kprintf("FiFi OS: initrd: bad cpio magic at %p (got %.6s)\n", (void*)p, hdr);
            return;
        }

        uint32_t mode     = hex8_u32(hdr + 14);
        uint32_t filesize = hex8_u32(hdr + 54);
        uint32_t namesize = hex8_u32(hdr + 94);

        const uint8_t *namep = p + 110;
        if (namep + namesize > end) {
            kprintf("FiFi OS: initrd: name overruns archive\n");
            return;
        }

        const char *name = (const char *)namep;

        uint64_t name_block = align4_u64(110ULL + (uint64_t)namesize);
        const uint8_t *datap = p + name_block;

        if (datap + filesize > end) {
            kprintf("FiFi OS: initrd: data overruns archive for %s\n", name);
            return;
        }

        if (streq(name, "TRAILER!!!")) {
            g_initrd.ready = 1;
            return;
        }

        if (g_initrd.count < INITRD_MAX_FILES) {
            struct initrd_entry *e = &g_initrd.files[g_initrd.count++];
            e->name = name;
            e->data = datap;
            e->size = (uint64_t)filesize;
            e->mode = mode;
        } else {
            kprintf("FiFi OS: initrd: too many files (max=%d)\n", INITRD_MAX_FILES);
            g_initrd.ready = 1;
            return;
        }

        uint64_t file_block = align4_u64((uint64_t)filesize);
        p = datap + file_block;
    }

    kprintf("FiFi OS: initrd: archive ended unexpectedly\n");
}

void initrd_init(void) {
    struct limine_module_response *resp = module_request.response;

    if (!resp || resp->module_count == 0) {
        kprintf("FiFi OS: initrd: no modules\n");
        return;
    }
    // Pick the first module (this repo's limine.h does not expose cmdline)
    struct limine_file *m = resp->modules[0];

if (!m || !m->address || m->size < 6) {
        kprintf("FiFi OS: initrd: module invalid\n");
        return;
    }

    const char *path = m->path ? m->path : "(null)";
        kprintf("FiFi OS: initrd: using module path=%s addr=%p size=%p\n", path, m->address, (void*)m->size);

    if (!is_newc_magic(m->address, (uint64_t)m->size)) {
        const char *magic = (const char *)m->address;
        kprintf("FiFi OS: initrd: not cpio-newc (magic %.6s)\n", magic);
        return;
    }

    initrd_index_newc(m->address, (uint64_t)m->size);

    if (g_initrd.ready) {
        kprintf("FiFi OS: initrd: indexed %d file(s)\n", (int)g_initrd.count);
        kprintf("FiFi OS: initrd: tip -> try initrd_ls() / initrd_cat(\"hello.txt\")\n");
    }
}

void initrd_ls(void) {
    if (!g_initrd.ready) {
        kprintf("FiFi OS: initrd_ls: initrd not ready\n");
        return;
    }

    kprintf("FiFi OS: initrd_ls: %d file(s)\n", (int)g_initrd.count);
    for (uint64_t i = 0; i < g_initrd.count; i++) {
        const struct initrd_entry *e = &g_initrd.files[i];
        kprintf("  [%p] %s  (size=%p mode=%x)\n",
                (void*)i,
                e->name ? e->name : "(null)",
                (void*)e->size,
                e->mode);
    }
}

int initrd_get(const char *name, const void **data, uint64_t *size) {
    if (!g_initrd.ready || !name || !data || !size) return -1;

    for (uint64_t i = 0; i < g_initrd.count; i++) {
        const struct initrd_entry *e = &g_initrd.files[i];
        if (e->name && streq(e->name, name)) {
            *data = (const void *)e->data;
            *size = e->size;
            return 0;
        }
    }
    return -1;
}

void initrd_cat(const char *name) {
    const void *data = NULL;
    uint64_t size = 0;

    if (initrd_get(name, &data, &size) != 0) {
        kprintf("FiFi OS: initrd_cat: not found: %s\n", name ? name : "(null)");
        return;
    }

    kprintf("FiFi OS: initrd_cat: %s (size=%p)\n", name, (void*)size);

    const uint8_t *p = (const uint8_t *)data;
    uint64_t n = size;
    if (n > 512) {
        kprintf("FiFi OS: initrd_cat: (showing first 512 bytes)\n");
        n = 512;
    }

    for (uint64_t i = 0; i < n; i++) {
        char c = (char)p[i];
        if (c == '\0') break;
        kprintf("%c", c);
    }
    kprintf("\n");
}

/* Keep your old debug entrypoint working too */
void initrd_dump_modules(void) {
    kprintf("FiFi OS: initrd_dump_modules() called\n");

    struct limine_module_response *resp = module_request.response;
    if (!resp || resp->module_count == 0) {
        kprintf("FiFi OS: initrd/modules: none\n");
        return;
    }

    kprintf("FiFi OS: initrd/modules: count=%p\n", (void*)resp->module_count);

    for (uint64_t i = 0; i < resp->module_count; i++) {
        struct limine_file *m = resp->modules[i];

        const char *path = (m && m->path) ? m->path : "(null)";
        kprintf("  mod[%p]: addr=%p size=%p path=%s\n",
                (void*)i,
                m->address,
                (void*)m->size,
                path);
}
}



// -------------------- CPIO "newc" parser (initrd.cpio) --------------------
// We treat the first Limine module as our initrd (since cmdline isn't exposed).

static const uint8_t *g_initrd_base = 0;
static uint64_t g_initrd_size = 0;

const void *initrd_get_base(void) { return g_initrd_base; }
uint64_t initrd_get_size(void) { return g_initrd_size; }

static uint32_t hex8(const char *p) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return 0;
    }
    return v;
}

struct cpio_newc_hdr {
    char magic[6];      // "070701"
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];   // hex bytes
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];   // hex bytes, includes NUL
    char check[8];
};

static int streq0(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return (*a == 0 && *b == 0);
}

const void *initrd_find(const char *name, uint64_t *out_size) {
    if (!g_initrd_base || g_initrd_size < sizeof(struct cpio_newc_hdr)) return 0;

    const uint8_t *p = g_initrd_base;
    const uint8_t *end = g_initrd_base + g_initrd_size;

    while (p + sizeof(struct cpio_newc_hdr) <= end) {
        const struct cpio_newc_hdr *h = (const struct cpio_newc_hdr*)p;

        // magic check
        if (!(h->magic[0]=='0' && h->magic[1]=='7' && h->magic[2]=='0' &&
              h->magic[3]=='7' && h->magic[4]=='0' && h->magic[5]=='1')) {
            return 0;
        }

        uint32_t namesz = hex8(h->namesize);
        uint32_t filesz = hex8(h->filesize);

        const uint8_t *namep = p + sizeof(struct cpio_newc_hdr);
        if (namep + namesz > end) return 0;

        const char *fname = (const char*)namep;

        const uint8_t *filep = namep + align4_u64(namesz);
        if (filep + filesz > end) return 0;

        // end marker
        if (streq0(fname, "TRAILER!!!")) return 0;

        if (streq0(fname, name)) {
            if (out_size) *out_size = filesz;
            return filep;
        }

        // advance
        p = filep + align4_u64(filesz);
    }

    return 0;
}
