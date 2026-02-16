ENTRY(kmain)

PHDRS
{
  all PT_LOAD FLAGS(7); /* R + W + X (temporary bring-up) */
}

SECTIONS
{
  . = 0xffffffff80000000;

  .text : ALIGN(0x1000)
  {
    *(.text .text.*)
  } :all

  .rodata : ALIGN(0x1000)
  {
    *(.rodata .rodata.*)
  } :all

  /* Keep Limine request sections intact */
  .limine_requests_start : ALIGN(8)
  {
    KEEP(*(.limine_requests_start))
  } :all

  .limine_requests : ALIGN(8)
  {
    KEEP(*(.limine_requests))
    KEEP(*(.limine_requests.*))
  } :all

  .limine_requests_end : ALIGN(8)
  {
    KEEP(*(.limine_requests_end))
  } :all

  .data : ALIGN(0x1000)
  {
    *(.data .data.*)
  } :all

  .bss : ALIGN(0x1000)
  {
    *(COMMON)
    *(.bss .bss.*)
  } :all

  /DISCARD/ :
  {
    *(.eh_frame*)
    *(.note .note.*)
    *(.comment)
  }
}
