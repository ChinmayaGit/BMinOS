/* =========================================================================
 * BMinOS — x86_64 Kernel Linker Script
 * =========================================================================
 * The kernel is loaded physically at 1 MB (0x100000) but linked to run
 * at the higher-half virtual address 0xFFFFFFFF80000000.  Early boot code
 * sets up page tables to map the physical range into the higher half
 * before jumping to the C++ entry point.
 *
 * Memory layout (virtual):
 *   0xFFFFFFFF80000000  .text    (code)
 *                       .rodata  (read-only data, constants)
 *                       .data    (initialized read-write data)
 *                       .bss     (zero-initialized data)
 *   _kernel_end
 * ========================================================================= */

ENTRY(_start)

/* Virtual base where the kernel is mapped (higher half) */
KERNEL_VMA = 0xFFFFFFFF80000000;

/* Physical address where the bootloader places the kernel image */
KERNEL_LMA = 0x100000;  /* 1 MB — above the legacy BIOS region */

SECTIONS
{
    /* -----------------------------------------------------------------
     * All sections start at the virtual address but carry a load address
     * (AT) at the physical location so the bootloader knows where to
     * place bytes in physical memory.
     * ----------------------------------------------------------------- */
    . = KERNEL_VMA;

    /* Exported symbol: absolute start of the kernel image */
    PROVIDE(_kernel_start = .);
    PROVIDE(_kernel_phys_start = KERNEL_LMA);

    /* ---- Executable code -------------------------------------------- */
    .text ALIGN(4K) : AT(KERNEL_LMA)
    {
        *(.text.boot)       /* Early boot stub — must come first */
        *(.text .text.*)
    }

    /* ---- Read-only data --------------------------------------------- */
    .rodata ALIGN(4K) : AT(KERNEL_LMA + (.rodata - KERNEL_VMA))
    {
        *(.rodata .rodata.*)
    }

    /* ---- Initialized read-write data -------------------------------- */
    .data ALIGN(4K) : AT(KERNEL_LMA + (.data - KERNEL_VMA))
    {
        *(.data .data.*)
    }

    /* ---- Zero-initialized data (BSS) -------------------------------- */
    .bss ALIGN(4K) : AT(KERNEL_LMA + (.bss - KERNEL_VMA))
    {
        PROVIDE(_bss_start = .);
        *(.bss .bss.*)
        *(COMMON)
        PROVIDE(_bss_end = .);
    }

    /* Exported symbol: end of the kernel image (page-aligned) */
    . = ALIGN(4K);
    PROVIDE(_kernel_end = .);
    PROVIDE(_kernel_phys_end = KERNEL_LMA + (_kernel_end - KERNEL_VMA));

    /* ---- Kernel stack (16 KB, 16-byte aligned) ---------------------- */
    . = ALIGN(16);
    PROVIDE(_stack_bottom = .);
    . += 16K;
    PROVIDE(_stack_top = .);

    /* ---- Discard unwanted sections ---------------------------------- */
    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.gnu.hash)
        *(.gnu.version*)
    }
}
