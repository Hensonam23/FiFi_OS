#pragma once
#include <stdint.h>

void initrd_dump_modules(void);
void initrd_list_files(void);

/* initrd (cpio) API */
void initrd_init(void);
void initrd_ls(void);
void initrd_cat(const char *name);
int  initrd_get(const char *name, const void **data, uint64_t *size);

