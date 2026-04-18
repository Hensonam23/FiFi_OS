#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "kprintf.h"
#include "panic.h"

#define PAGE_SIZE 4096ULL

/* Dedicated heap virtual arena (NOT HHDM) */
#define HEAP_VIRT_BASE   0xffff910000000000ULL

#ifdef FIFI_HEAP_OVF_TEST
/* Small arena for deterministic guard-page fault test:
   [guard][1 usable page][guard] */
#define HEAP_USABLE_SIZE (PAGE_SIZE)
#else
/* Normal heap arena size */
#define HEAP_USABLE_SIZE (64ULL * 1024ULL * 1024ULL) /* 64 MiB */
#endif

/* Layout:
   HEAP_VIRT_BASE ............ (guard page, unmapped)
   HEAP_USABLE_BASE .......... start usable (mapped on demand)
   HEAP_USABLE_END ........... end usable
   HEAP_GUARD_END ............ (guard page, unmapped)
*/
static uint64_t heap_usable_base = 0;
static uint64_t heap_usable_end  = 0;

/* Current bump state */
static uint64_t cur_page_virt = 0;
static size_t   cur_off = 0;
static uint64_t next_page_virt = 0;

/* ── Free list ─────────────────────────────────────────────────────────
 * Every allocation is preceded by a heap_hdr_t (16 bytes).
 * kfree() chains freed blocks here. kmalloc() checks here first.
 * The 'next' pointer is stored in the first 8 bytes of the user area
 * while the block is on the free list (safe — user isn't using it).
 */
#define HEAP_MAGIC       0xF1F10A110DEAD000ULL
#define HEAP_MAGIC_LARGE 0xF1F10A110DEAD001ULL  /* large alloc — not recycled by free list */

typedef struct {
    size_t   size;   /* usable bytes after this header */
    uint64_t magic;  /* HEAP_MAGIC when valid */
} heap_hdr_t;        /* 16 bytes, keeps 16-byte alloc alignment */

static heap_hdr_t *g_free_list = 0;


static inline uintptr_t align_up_uintptr(uintptr_t x, uintptr_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

static inline uint64_t align_down_u64(uint64_t x) {
    return x & ~0xFFFULL;
}


static void memzero(void *p, size_t n) {
    volatile uint8_t *b = (volatile uint8_t*)p;
    for (size_t i = 0; i < n; i++) b[i] = 0;
}

#ifdef FIFI_HEAP_POISON
static void memfill(void *p, uint8_t v, size_t n) {
    volatile uint8_t *b = (volatile uint8_t*)p;
    for (size_t i = 0; i < n; i++) b[i] = v;
}
#endif


static void heap_map_fresh_page(void) {
    if (next_page_virt >= heap_usable_end) {
        panic("heap: out of virtual space (hit end guard)");
    }

    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        panic("heap: out of physical pages");
    }

    if (!vmm_map_page(next_page_virt, phys, VMM_WRITE)) {
        panic("heap: vmm_map_page failed");
    }

    cur_page_virt = next_page_virt;
    cur_off = 0;
    next_page_virt += PAGE_SIZE;
}

#ifdef FIFI_HEAP_TEST
static void heap_self_test(void) {
    kprintf("Heap test: begin\n");

    void *a = kmalloc(32);
    void *b = kmalloc_aligned(64, 64);
    void *c = kmalloc(5000);

    if (!a || !b || !c) {
        panic("Heap test: allocation returned NULL");
    }

    if (((uintptr_t)b % 64) != 0) {
        panic("Heap test: kmalloc_aligned alignment failed");
    }

    /* Touch memory */
    *(volatile uint64_t*)a = 0x1111222233334444ULL;
    *(volatile uint64_t*)b = 0xaaaabbbbccccddddULL;
    ((volatile uint8_t*)c)[0] = 0x5a;
    ((volatile uint8_t*)c)[4999] = 0xa5;

    /* Translate sanity */
    uint64_t pa = vmm_translate((uint64_t)(uintptr_t)a);
    uint64_t pb = vmm_translate((uint64_t)(uintptr_t)b);
    uint64_t pc = vmm_translate((uint64_t)(uintptr_t)c);

    kprintf("Heap test: a=%p pa=%p\n", a, (void*)pa);
    kprintf("Heap test: b=%p pb=%p\n", b, (void*)pb);
    kprintf("Heap test: c=%p pc=%p\n", c, (void*)pc);

    if (!pa || !pb || !pc) {
        panic("Heap test: translate returned 0 for a heap pointer");
    }

#ifdef FIFI_HEAP_OVF_TEST
    /* Deterministic guard touch: write into the end guard page.
       With the small arena, heap_usable_end is exactly one page after base. */
    kprintf("Heap test: OVF enabled -> touching end guard page now (should #PF)\n");
    volatile uint64_t *bad = (volatile uint64_t*)(uintptr_t)(heap_usable_end);
    *bad = 0xdeadbeefdeadbeefULL;
#endif

    kprintf("Heap test: done\n");
}
#endif

void heap_init(void) {
    heap_usable_base = HEAP_VIRT_BASE + PAGE_SIZE; /* skip first guard page */
    heap_usable_end  = heap_usable_base + HEAP_USABLE_SIZE;
    next_page_virt   = heap_usable_base;
    cur_page_virt    = 0;
    cur_off          = 0;

    kprintf("Heap arena: base=%p end=%p (guard before/after)\n",
            (void*)heap_usable_base, (void*)heap_usable_end);

    /* map first usable page so kmalloc works immediately */
    heap_map_fresh_page();

#ifdef FIFI_HEAP_TEST
    heap_self_test();
#endif
}

/* Round size up to 8-byte multiple. */
static inline size_t round_size(size_t s) { return (s + 7UL) & ~7UL; }

/* First-fit free list search. Returns user pointer or 0. */
static void *freelist_take(size_t need) {
    heap_hdr_t **pp = &g_free_list;
    while (*pp) {
        heap_hdr_t *hdr = *pp;
        void       *usr = (void*)(hdr + 1);
        if (hdr->size >= need) {
            *pp = *(heap_hdr_t **)usr;
            hdr->magic = HEAP_MAGIC;
            memzero(usr, hdr->size);
            return usr;
        }
        pp = (heap_hdr_t **)usr;
    }
    return 0;
}

void *kmalloc_aligned(size_t size, size_t align) {
    if (size == 0) return 0;
    if (align < 16) align = 16;
    if (align & (align - 1)) panic("kmalloc_aligned: align not power-of-two");

    /* Large alloc: whole pages, with a header so kfree() doesn't panic.
     * Returns ptr+16 (still 16-byte aligned). Pages are not reclaimed on free. */
    if (size >= PAGE_SIZE) {
        size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t bytes = (uint64_t)pages * PAGE_SIZE;
        uint64_t v = align_down_u64(next_page_virt);
        uint64_t vend = v + bytes;
        if (vend > heap_usable_end) panic("heap: big alloc exceeds arena");
        uint64_t phys = pmm_alloc_pages(pages);
        if (!phys) return 0;
        if (!vmm_map_range(v, phys, bytes, VMM_WRITE)) panic("heap: vmm_map_range failed");
        next_page_virt = vend;
        heap_hdr_t *hdr = (heap_hdr_t *)(uintptr_t)v;
        hdr->size  = (size_t)(bytes - sizeof(heap_hdr_t));
        hdr->magic = HEAP_MAGIC_LARGE;
        void *usr = (void *)(uintptr_t)(v + sizeof(heap_hdr_t));
        memzero(usr, hdr->size < size ? hdr->size : size);
        return usr;
    }

    /* Standard alloc: check free list first (16-byte align only). */
    size_t need = round_size(size < 8 ? 8 : size);
    if (align == 16) {
        void *hit = freelist_take(need);
        if (hit) return hit;
    }

    /* Bump allocate with header prepended. */
    size_t total = sizeof(heap_hdr_t) + need;
    if (!cur_page_virt) return 0;

    uintptr_t base    = (uintptr_t)cur_page_virt;
    uintptr_t hptr    = align_up_uintptr(base + cur_off, (uintptr_t)align);
    size_t    new_off = (size_t)((hptr - base) + total);

    if (new_off > PAGE_SIZE) {
        heap_map_fresh_page();
        base    = (uintptr_t)cur_page_virt;
        hptr    = align_up_uintptr(base + cur_off, (uintptr_t)align);
        new_off = (size_t)((hptr - base) + total);
        if (new_off > PAGE_SIZE) return 0;
    }

    cur_off = new_off;
    heap_hdr_t *hdr = (heap_hdr_t*)hptr;
    hdr->size  = need;
    hdr->magic = HEAP_MAGIC;
    void *usr = (void*)(hdr + 1);
    memzero(usr, need);
    return usr;
}

void *kmalloc(size_t size) { return kmalloc_aligned(size, 16); }

void *kzalloc(size_t size) { return kmalloc(size); }  /* kmalloc already zeroes */

void kfree(void *ptr) {
    if (!ptr) return;
    heap_hdr_t *hdr = (heap_hdr_t*)ptr - 1;
    if (hdr->magic == HEAP_MAGIC_LARGE) {
        hdr->magic = 0;   /* prevent double-free */
        return;            /* pages stay mapped; not worth the VMM complexity to unmap */
    }
    if (hdr->magic != HEAP_MAGIC)
        panic("kfree: bad magic (double-free or corrupt pointer)");
    hdr->magic = 0;
#ifdef FIFI_HEAP_POISON
    memfill(ptr, 0xDD, hdr->size);
#endif
    *(heap_hdr_t **)ptr = g_free_list;
    g_free_list = hdr;
}


// ---- Heap statstats (for shell 'mem' command) ----
void *heap_get_cur_page(void) {
    return (void*)(uintptr_t)cur_page_virt;
}

uint64_t heap_get_offset(void) {
    return (uint64_t)cur_off;
}
