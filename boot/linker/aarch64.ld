/* =========================================================================
 * BMinOS — AArch64 Kernel Linker Script
 * =========================================================================
 * The kernel is loaded physically at 2 MB (0x200000) — aligned to a 2 MB
 * block boundary for efficient Stage-1 page table mapping — and linked
 * to run at the higher-half virtual address 0xFFFF000000000000.
 *
 * ARM64 uses TTBR1_EL2 for the upper virtual address range; early boot
 * code configures the translation tables before branching to C++.
 *
 * Memory layout (virtual):
 *   0xFFFF000000000000  .text    (code)
 *                       .rodata  (read-only data)
 *                       .data    (initialized data)
 *                       .bss     (zero-initialized data)
 *   _kernel_end
 * ========================================================================= */

ENTRY(_start)

/* Virtual base in the TTBR1 range (upper half) */
KERNEL_VMA = 0xFFFF000000000000;

/* Physical load address — 2 MB aligned for block mapping */
KERNEL_LMA = 0x200000;

SECTIONS
{
    . = KERNEL_VMA;

    /* Exported symbol: absolute start of the kernel image */
    PROVIDE(_kernel_start = .);
    PROVIDE(_kernel_phys_start = KERNEL_LMA);

    /* ---- Executable code -------------------------------------------- */
    .text ALIGN(4K) : AT(KERNEL_LMA)
    {
        KEEP(*(.text.boot))   /* Reset vector / entry stub — must be first */
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

    /* End of kernel image (page-aligned) */
    . = ALIGN(4K);
    PROVIDE(_kernel_end = .);
    PROVIDE(_kernel_phys_end = KERNEL_LMA + (_kernel_end - KERNEL_VMA));

    /* ---- Kernel stack (16 KB, 16-byte aligned per AAPCS64) ---------- */
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
        *(.ARM.exidx*)
        *(.ARM.attributes)
        *(.gnu.hash)
        *(.gnu.version*)
    }
}
