#pragma once
#include <stddef.h>
#include <stdint.h>

void heap_init(void);

void *kmalloc(size_t size);
void *kmalloc_aligned(size_t size, size_t align);
void *kzalloc(size_t size);
void  kfree(void *ptr);

// Heap stats
void *heap_get_cur_page(void);
uint64_t heap_get_offset(void);
