#include <stdint.h>
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
#define USER_STACK_TOP 0x0000000000500000ULL

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
__attribute__((noreturn))
static void userdemo_thread_fn(void *arg) {
    (void)arg;

    // Allocate phys pages
    uint64_t code_phys  = pmm_alloc_page();
    uint64_t stack_phys = pmm_alloc_page();
    if (!code_phys || !stack_phys) {
        kprintf("[userdemo] pmm_alloc_page failed\n");
        thread_exit();
    }

    // Map user pages (IMPORTANT: VMM must propagate PTE_US at all levels)
    if (!vmm_map_page(USER_CODE_VA, code_phys, VMM_USER | VMM_WRITE)) {
        kprintf("[userdemo] vmm_map_page(code) failed\n");
        thread_exit();
    }
    if (!vmm_map_page(USER_STACK_TOP - 0x1000, stack_phys, VMM_USER | VMM_WRITE)) {
        kprintf("[userdemo] vmm_map_page(stack) failed\n");
        thread_exit();
    }

    // Build tiny ring3 code: SYS_LOG("hello from ring3") once, then infinite loop.
    // ABI assumption: syscall number in RAX, arg1 in RDI (matches your sys_call1 usage).
    // Build ring3 program at USER_CODE_VA:
    //   SYS_LOG("hello from ring3")
    //   SYS_EXIT(0)
    uint8_t *code = (uint8_t *)(uintptr_t)USER_CODE_VA;
    const char msg[] = "hello from ring3";
    uintptr_t msg_va = (uintptr_t)USER_CODE_VA + 0x200;

    // copy string into user memory (same mapped page as code)
    memcpy((void*)(uintptr_t)msg_va, msg, sizeof(msg));

    size_t o = 0;

    // mov eax, SYS_LOG
    code[o++] = 0xB8;
    { uint32_t n = (uint32_t)SYS_LOG; memcpy(&code[o], &n, 4); o += 4; }

    // mov rdi, msg_va
    code[o++] = 0x48; code[o++] = 0xBF;
    { uint64_t a = (uint64_t)msg_va; memcpy(&code[o], &a, 8); o += 8; }

    // int 0x80
    code[o++] = 0xCD; code[o++] = 0x80;

    // mov eax, SYS_EXIT
    code[o++] = 0xB8;
    { uint32_t n = (uint32_t)SYS_EXIT; memcpy(&code[o], &n, 4); o += 4; }

    // xor edi, edi   (exit code 0)
    code[o++] = 0x31; code[o++] = 0xFF;

    // int 0x80
    code[o++] = 0xCD; code[o++] = 0x80;

    // jmp $ (should never run if SYS_EXIT works)
    code[o++] = 0xEB; code[o++] = 0xFE;

kprintf("[userdemo] entering ring3 cs=0x%x ds=0x%x rip=%p rsp=%p\n",
            (uint16_t)FIFI_USER_CS, (uint16_t)FIFI_USER_DS, (void*)USER_CODE_VA, (void*)USER_STACK_TOP);

    enter_user_mode(USER_CODE_VA, USER_STACK_TOP - 0x10, (uint16_t)FIFI_USER_CS, (uint16_t)FIFI_USER_DS);
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
