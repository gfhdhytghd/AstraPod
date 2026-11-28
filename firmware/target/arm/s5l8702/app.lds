#define __ASSEMBLER__
#include "config.h"
#include "cpu.h"

ENTRY(start)

OUTPUT_FORMAT(elf32-littlearm)
OUTPUT_ARCH(arm)
STARTUP(target/arm/s5l8702/crt0.o)

#define PLUGINSIZE PLUGIN_BUFFER_SIZE
#define CODECSIZE CODEC_SIZE

#define IRAMORIG 0x0
#define DRAMORIG 0x08000000

/* End of the audio buffer, where the codec buffer starts */
#define ENDAUDIOADDR  (DRAMORIG + DRAMSIZE)

#define DRAMSIZE (DRAM_SIZE - PLUGINSIZE - CODECSIZE - TTB_SIZE)
#define CODECORIG (ENDAUDIOADDR)
#define IRAMSIZE (48*1024)  /* 256KB total - 48KB for core, 200KB for codecs */

/* Where the codec buffer ends, and the plugin buffer starts */
#define ENDADDR (ENDAUDIOADDR + CODECSIZE)

MEMORY
{
    IRAM : ORIGIN = IRAMORIG, LENGTH = IRAMSIZE
    DRAM : ORIGIN = DRAMORIG, LENGTH = DRAMSIZE
}

SECTIONS
{
    loadaddress = DRAMORIG;

    .intvect : {
        _intvectstart = . ;
        *(.intvect)
        _intvectend = _newstart ;  
    } >IRAM AT> DRAM
    _intvectcopy = LOADADDR(.intvect) ;

    .text :
    {
        _loadaddress = .;
        _textstart = .;
        *(.init.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(0x4);
        _textend = .;
    } > DRAM

    .rodata :
    {
        *(.rodata*)
        . = ALIGN(0x4);
    } > DRAM

    .data :
    {
        *(.data*)
        . = ALIGN(0x4);
    } > DRAM

    /DISCARD/ :
    {
        *(.eh_frame)
    }

    /* FU51: VA page 0 holds nothing after boot. The exception vectors move
     * to the high page (0xffff0000) and page 0 becomes a read-only zero page
     * (FU53; no-access in FU51), so a NULL (or near-NULL) read returns 0 and
     * a write or call faults with the culprit's pc. IRAM code and data start
     * at the next page. */
    .iram 0x1000 :
    {
        _iramstart = .;
        _icodestart = .;
        *(.icode*)
        . = ALIGN(0x4);
        _icodeend = .;
        *(.irodata*)
        *(.idata*)
        . = ALIGN(0x4);
        _iramend = .;
    } > IRAM AT> DRAM
    _iramcopy = LOADADDR(.iram) ;

    .ibss (NOLOAD) :
    {
        _iedata = .;
        *(.qharray)
        *(.ibss*)
        . = ALIGN(0x4);
        _iend = .;
    } > IRAM

    .stack (NOLOAD) :
    {
       *(.stack)
       _irqstackbegin = .;
       . += 0x400;
       _irqstackend = .;
       _fiqstackbegin = .;
       /* FIQ mode runs only the freeze watch's check (its screen has a
        * stack of its own); 0x200 leaves IRAM for the FU51 crumbs */
       . += 0x200;
       _fiqstackend = .;
    } > IRAM
    ASSERT(_intvectend + 8 <= 0x60, "vectors and literals beyond the copied 0x60")

    /* The persistent breadcrumbs sit after the last loaded byte and before
     * the application's zeroed BSS. The bootloader's BSS starts at 8 MiB. */
    .persist (NOLOAD) :
    {
        . = ALIGN(32);
        _persiststart = .;
        *(.persist*)
        . = ALIGN(32);
        _persistend = .;
    } > DRAM
    ASSERT(_persistend <= DRAMORIG + 0x800000, ".persist overlaps the bootloader's .bss")
    ASSERT(_persiststart >= LOADADDR(.iram) + SIZEOF(.iram), ".persist inside the loaded image")

    .bss (NOLOAD) :
    {
       _edata = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(0x4);
       _end = .;
    } > DRAM

    /* The main (SYS) stack, out of IRAM since FU51 freed page 0 (crt0 fills
     * it with 0xdeadbeef; it is not part of the zeroed .bss). 12 KiB: the
     * 8 KiB stack overflowed into __thread_entries (FU24). */
    .mainstack (NOLOAD) :
    {
       . = ALIGN(32);
       stackbegin = .;
       _stackbegin = .;
       . += 0x3000;
       stackend = .;
       _stackend = .;
    } > DRAM

    .audiobuf (NOLOAD) :
    {
        . = ALIGN(4);
        _audiobuffer = .;
        audiobuffer = .;
    } > DRAM

    .audiobufend ENDAUDIOADDR (NOLOAD) :
    {
        audiobufend = .;
        _audiobufend = .;
    } > DRAM

    .codec CODECORIG (NOLOAD) :
    {
        codecbuf = .;
        _codecbuf = .;
    } > DRAM

    .plugin ENDADDR (NOLOAD) :
    {
        _pluginbuf = .;
        pluginbuf = .;
    }
}
