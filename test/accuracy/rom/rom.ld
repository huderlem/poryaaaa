ENTRY(Start)

gNumMusicPlayers = 4;
gMaxLines = 0;

MEMORY
{
    EWRAM (rwx) : ORIGIN = 0x2000000, LENGTH = 256K
    IWRAM (rwx) : ORIGIN = 0x3000000, LENGTH = 32K
    ROM   (rx)  : ORIGIN = 0x8000000, LENGTH = 32M
}

SECTIONS
{
    ewram 0x2000000 (NOLOAD) : ALIGN(4)
    {
        *(ewram_data)
    } > EWRAM

    iwram 0x3000000 (NOLOAD) : ALIGN(4)
    {
        *(.bss)
        *(.bss.*)
        *(.bss.code)
        *(common_data)
        *(COMMON)
    } > IWRAM

    . = 0x8000000;

    .text : ALIGN(4)
    {
        *crt0.o(.text)
        *(.text)
        *(.text.*)
    } > ROM

    .rodata : ALIGN(4)
    {
        *(.rodata)
        *(.rodata.*)
        *(.data)
        *(.data.*)
    } > ROM

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.ARM.*)
    }
}
