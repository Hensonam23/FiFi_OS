#include <stdint.h>
static inline uint64_t align_down_4k(uint64_t x) { return x & ~0xFFFULL; }
static inline uint64_t align_up_4k(uint64_t x) { return (x + 0xFFFULL) & ~0xFFFULL; }

#include <stddef.h>
#include "usermode.h"
#include <string.h>

#include "kprintf.h"
#include "pmm.h"
#include "vmm.h"
#include "thread.h"
#include "syscall.h"
#include "gdt.h"

// Low-half user addresses
#define USER_CODE_VA   0x0000000000400000ULL
#define USER_STACK_TOP ((uint64_t)FIFI_USER_STACK_TOP)

static inline uint64_t read_rflags(void) {
    uint64_t r;
    __asm__ volatile ("pushfq; pop %0" : "=r"(r));
    return r;
}
__attribute__((noreturn))
static void enter_user_mode(uint64_t user_rip, uint64_t user_rsp, uint16_t user_cs, uint16_t user_ds) {
    (void)user_cs;
    (void)user_ds;
    uint64_t rflags = read_rflags() | (1ULL << 9); // IF=1

    // Make sure TSS rsp0 is current kernel stack for privilege transitions
    __asm__ volatile (
        "cli\n"
        "mov %0, %%ds\n"
        "mov %0, %%es\n"
        "mov %0, %%fs\n"
        "mov %0, %%gs\n"
        "pushq %0\n"        // SS
        "pushq %1\n"        // RSP
        "pushq %2\n"        // RFLAGS
        "pushq %3\n"        // CS
        "pushq %4\n"        // RIP
        "iretq\n"
        :
        : "r"((uint64_t)(uint16_t)FIFI_USER_DS),
          "r"(user_rsp),
          "r"(rflags),
          "r"((uint64_t)(uint16_t)FIFI_USER_CS),
          "r"(user_rip)
        : "memory"
    );

    __builtin_unreachable();
}

// This runs in a kernel thread, then jumps to ring3 and never returns.
static int user_map_pages(uint64_t va, uint64_t size, vmm_flags_t flags) {
    uint64_t start = align_down_4k(va);
    uint64_t end   = align_up_4k(va + size);

    for (uint64_t v = start; v < end; v += 0x1000ULL) {
        (void)vmm_unmap_page_and_free(v);
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            (void)vmm_unmap_range_and_free(start, v - start);
            return -1;
        }
        if (!vmm_map_page(v, phys, flags)) {
            pmm_free_page(phys);
            (void)vmm_unmap_range_and_free(start, v - start);
            return -1;
        }

        uint8_t *p = (uint8_t*)(uintptr_t)v;
        for (uint64_t i = 0; i < 0x1000ULL; i++) p[i] = 0;
    }

    if (thread_user_map_add(start, end - start) < 0) {
        (void)vmm_unmap_range_and_free(start, end - start);
        return -1;
    }

    return 0;
}

static void userdemo_thread_fn(void *arg) {
    // Map canonical trampoline + stack
    if (user_map_pages((uint64_t)FIFI_USER_TRAMPOLINE_VA, 0x1000ULL, VMM_USER | VMM_WRITE) < 0) {
        kprintf("[userdemo] trampoline map failed\n");
        thread_exit();
    }
    // write trampoline code at the trampoline VA
    for (size_t i = 0; i < sizeof(FIFI_USER_TRAMPOLINE_CODE); i++) {
        ((volatile uint8_t*)(uintptr_t)FIFI_USER_TRAMPOLINE_VA)[i] = FIFI_USER_TRAMPOLINE_CODE[i];
    }

    if (user_map_pages((uint64_t)FIFI_USER_STACK_BASE, (uint64_t)(FIFI_USER_STACK_TOP - FIFI_USER_STACK_BASE),
                       VMM_USER | VMM_WRITE | VMM_NX) < 0) {
        kprintf("[userdemo] stack map failed\n");
        thread_exit();
    }

    // Map one code page at 0x400000
    const uint64_t user_rip = 0x0000000000400000ULL;
    if (user_map_pages(user_rip, 0x1000ULL, VMM_USER | VMM_WRITE) < 0) {
        kprintf("[userdemo] code map failed\n");
        thread_exit();
    }

    // Put message in user memory
    const uint64_t msg_va = user_rip + 0x200ULL;
    const char msg[] = "hello from ring3";
    for (size_t i = 0; i < sizeof(msg); i++) {
        ((volatile uint8_t*)(uintptr_t)msg_va)[i] = (uint8_t)msg[i];
    }

    // Build ring3 program: SYS_LOG(msg) ; SYS_EXIT(0)
    uint8_t *code = (uint8_t *)(uintptr_t)user_rip;
    size_t ud_o = 0;

    // mov eax, SYS_LOG
    code[ud_o++] = 0xB8;
    code[ud_o++] = (uint8_t)((uint32_t)SYS_LOG & 0xFF);
    code[ud_o++] = (uint8_t)(((uint32_t)SYS_LOG >> 8) & 0xFF);
    code[ud_o++] = (uint8_t)(((uint32_t)SYS_LOG >> 16) & 0xFF);
    code[ud_o++] = (uint8_t)(((uint32_t)SYS_LOG >> 24) & 0xFF);

    // mov rdi, msg_va
    code[ud_o++] = 0x48; code[ud_o++] = 0xBF;
    for (int i = 0; i < 8; i++) {
        code[ud_o++] = (uint8_t)(((uint64_t)msg_va >> (8*i)) & 0xFF);
    }

    // int 0x80
    code[ud_o++] = 0xCD; code[ud_o++] = 0x80;

    // mov eax, SYS_EXIT
    code[ud_o++] = 0xB8;
    code[ud_o++] = (uint8_t)((uint32_t)SYS_EXIT & 0xFF);
    code[ud_o++] = (uint8_t)(((uint32_t)SYS_EXIT >> 8) & 0xFF);
    code[ud_o++] = (uint8_t)(((uint32_t)SYS_EXIT >> 16) & 0xFF);
    code[ud_o++] = (uint8_t)(((uint32_t)SYS_EXIT >> 24) & 0xFF);

    // xor edi, edi
    code[ud_o++] = 0x31; code[ud_o++] = 0xFF;

    // int 0x80
    code[ud_o++] = 0xCD; code[ud_o++] = 0x80;

    // jmp $
    code[ud_o++] = 0xEB; code[ud_o++] = 0xFE;

    // DEBUG: dump first 32 bytes of user code so we can verify immediates
    kprintf("[userdemo] code bytes:");
    for (int i = 0; i < 32; i++) {
        uint8_t b = ((uint8_t*)(uintptr_t)user_rip)[i];
        kprintf(" %p", (void*)(uint64_t)b);
    }
    kprintf("\n");
    kprintf("[userdemo] msg_va=%p\n", (void*)(uint64_t)msg_va);


    (void)arg;

kprintf("[userdemo] entering ring3 cs=0x%x ds=0x%x rip=%p rsp=%p\n",
            (uint16_t)FIFI_USER_CS, (uint16_t)FIFI_USER_DS, (void*)USER_CODE_VA, (void*)(uint64_t)FIFI_USER_STACK_TOP);
    // Set TSS.rsp0 for this thread so interrupts/syscalls from ring3 use the right kernel stack
    uint64_t ktop = thread_current_kstack_top();
    if (ktop) gdt_tss_set_rsp0(ktop);


    enter_user_mode(USER_CODE_VA, (uint64_t)FIFI_USER_STACK_TOP - 0x10ULL, (uint16_t)FIFI_USER_CS, (uint16_t)FIFI_USER_DS);
}
 
void userdemo_spawn(void) {
    // Make sure preemption is on, otherwise a ring3 loop can starve everything.
    if (!thread_preempt_get()) {
        kprintf("[userdemo] enabling preempt\n");
        thread_preempt_set(1);
    }
    thread_create("userdemo", userdemo_thread_fn, 0);
    kprintf("[userdemo] spawned\n");
}
