ENTRY(app_start)

MEMORY
{
  APP_CCMRAM (xrw) : ORIGIN = 0x10000000, LENGTH = 64K
}

SECTIONS
{
  .app_header :
  {
    . = ALIGN(4);
    KEEP(*(.app_header))
    . = ALIGN(4);
  } >APP_CCMRAM

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >APP_CCMRAM

  .data :
  {
    . = ALIGN(4);
    *(.data)
    *(.data*)
    . = ALIGN(4);
  } >APP_CCMRAM

  .bss :
  {
    . = ALIGN(4);
    __app_bss_start__ = .;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    __app_bss_end__ = .;
  } >APP_CCMRAM

  __app_image_end__ = .;

  /DISCARD/ :
  {
    *(.comment)
    *(.ARM.attributes)
  }
}
