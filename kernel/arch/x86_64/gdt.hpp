// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Global Descriptor Table (GDT) — x86_64
// =======================================
// In long mode (64-bit), the GDT is mostly vestigial — segmentation is
// effectively disabled. However, we still need it for:
// 1. Defining code/data segments for ring 0 (kernel) and ring 3 (user)
// 2. The Task State Segment (TSS) for interrupt stack switching (IST)
// 3. Setting the correct CS/SS for syscall/sysret and interrupt handlers
//
// Our GDT layout:
//   Entry 0: Null descriptor (required by hardware)
//   Entry 1: Kernel code segment (CS = 0x08) — 64-bit, ring 0
//   Entry 2: Kernel data segment (DS = 0x10) — ring 0
//   Entry 3: User code segment (CS = 0x1B) — 64-bit, ring 3 (with RPL=3)
//   Entry 4: User data segment (DS = 0x23) — ring 3 (with RPL=3)
//   Entry 5-6: TSS descriptor (16 bytes, spans two entries)

#pragma once

#include <cstdint>

namespace bminos::arch::x86_64 {

// =============================================================================
// Segment Selectors
// =============================================================================
// Segment selectors encode: index (bits 15:3), TI (bit 2), RPL (bits 1:0).
// TI = 0 for GDT. RPL = requested privilege level.

/// Null segment selector (index 0)
inline constexpr uint16_t SEGMENT_NULL          = 0x00;

/// Kernel code segment selector (GDT index 1, RPL 0)
inline constexpr uint16_t SEGMENT_KERNEL_CODE   = 0x08;

/// Kernel data segment selector (GDT index 2, RPL 0)
inline constexpr uint16_t SEGMENT_KERNEL_DATA   = 0x10;

/// User code segment selector (GDT index 3, RPL 3)
inline constexpr uint16_t SEGMENT_USER_CODE     = 0x18 | 3;  // 0x1B

/// User data segment selector (GDT index 4, RPL 3)
inline constexpr uint16_t SEGMENT_USER_DATA     = 0x20 | 3;  // 0x23

/// TSS segment selector (GDT index 5, RPL 0)
inline constexpr uint16_t SEGMENT_TSS           = 0x28;

// =============================================================================
// GDT Entry
// =============================================================================

/// A raw 64-bit GDT entry (8 bytes).
/// In 64-bit mode, most fields are ignored by the CPU for code/data segments.
/// The key fields that matter are:
///   - Type/S/DPL/P (access byte, bits 40-47)
///   - L (long mode, bit 53) — must be 1 for 64-bit code segments
///   - D/B (bit 54) — must be 0 when L=1
struct GdtEntry {
    uint16_t limit_low;     // Segment limit (bits 0-15) — ignored in long mode
    uint16_t base_low;      // Base address (bits 0-15) — ignored for code/data
    uint8_t  base_mid;      // Base address (bits 16-23)
    uint8_t  access;        // Access byte: P|DPL|S|Type
    uint8_t  granularity;   // Flags (G|D/B|L|AVL) and limit (bits 16-19)
    uint8_t  base_high;     // Base address (bits 24-31)

    /// Construct a null entry
    static constexpr GdtEntry null() {
        return {0, 0, 0, 0, 0, 0};
    }

    /// Construct a 64-bit code segment entry.
    /// @param dpl Descriptor Privilege Level (0 = kernel, 3 = user)
    static constexpr GdtEntry code_segment(uint8_t dpl) {
        return {
            .limit_low   = 0xFFFF,
            .base_low    = 0,
            .base_mid    = 0,
            // Access byte: P=1, DPL, S=1 (non-system), Type=0b1010 (execute/read)
            .access      = static_cast<uint8_t>(0x9A | ((dpl & 0x3) << 5)),
            // Granularity: G=1, D/B=0, L=1 (64-bit), AVL=0, limit[19:16]=0xF
            .granularity = 0xAF,
            .base_high   = 0,
        };
    }

    /// Construct a data segment entry.
    /// @param dpl Descriptor Privilege Level (0 = kernel, 3 = user)
    static constexpr GdtEntry data_segment(uint8_t dpl) {
        return {
            .limit_low   = 0xFFFF,
            .base_low    = 0,
            .base_mid    = 0,
            // Access byte: P=1, DPL, S=1, Type=0b0010 (read/write)
            .access      = static_cast<uint8_t>(0x92 | ((dpl & 0x3) << 5)),
            // Granularity: G=1, D/B=1, L=0, AVL=0, limit[19:16]=0xF
            .granularity = 0xCF,
            .base_high   = 0,
        };
    }
};

// Verify our struct packing is correct
static_assert(sizeof(GdtEntry) == 8, "GDT entry must be exactly 8 bytes");

/// TSS descriptor in the GDT is 16 bytes (two consecutive GDT entries).
/// This is because in 64-bit mode, system segment descriptors (like TSS) are
/// extended to hold a full 64-bit base address.
struct TssDescriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid_low;
    uint8_t  access;        // P=1, DPL=0, Type=0b1001 (available 64-bit TSS)
    uint8_t  flags_limit;   // Granularity flags and limit[19:16]
    uint8_t  base_mid_high;
    uint32_t base_high;
    uint32_t reserved;

    /// Construct a TSS descriptor from a TSS base address and limit.
    static TssDescriptor create(uint64_t base, uint32_t limit) {
        return {
            .limit_low    = static_cast<uint16_t>(limit & 0xFFFF),
            .base_low     = static_cast<uint16_t>(base & 0xFFFF),
            .base_mid_low = static_cast<uint8_t>((base >> 16) & 0xFF),
            .access       = 0x89,  // P=1, DPL=0, Type=0b1001 (available TSS)
            .flags_limit  = static_cast<uint8_t>((limit >> 16) & 0x0F),
            .base_mid_high= static_cast<uint8_t>((base >> 24) & 0xFF),
            .base_high    = static_cast<uint32_t>((base >> 32) & 0xFFFFFFFF),
            .reserved     = 0,
        };
    }
};

static_assert(sizeof(TssDescriptor) == 16, "TSS descriptor must be exactly 16 bytes");

// =============================================================================
// Task State Segment (TSS)
// =============================================================================

/// The 64-bit Task State Segment.
/// In long mode, the TSS is primarily used for:
/// - IST (Interrupt Stack Table): up to 7 known-good stack pointers for
///   interrupt/exception handling, allowing the CPU to switch to a clean
///   stack even if the current stack is corrupted.
/// - I/O permission bitmap base.
/// - RSP0: the stack pointer to use when transitioning from ring 3 to ring 0.
struct __attribute__((packed)) Tss {
    uint32_t reserved0;
    uint64_t rsp0;          // Stack pointer for ring 0 (used on privilege change)
    uint64_t rsp1;          // Stack pointer for ring 1 (unused in our design)
    uint64_t rsp2;          // Stack pointer for ring 2 (unused in our design)
    uint64_t reserved1;
    uint64_t ist1;          // Interrupt Stack Table entry 1 (e.g., for NMI)
    uint64_t ist2;          // IST entry 2 (e.g., for double fault)
    uint64_t ist3;          // IST entry 3 (e.g., for machine check)
    uint64_t ist4;          // IST entry 4
    uint64_t ist5;          // IST entry 5
    uint64_t ist6;          // IST entry 6
    uint64_t ist7;          // IST entry 7
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;    // Offset to I/O permission bitmap (from TSS base)
};

static_assert(sizeof(Tss) == 104, "TSS must be 104 bytes");

// =============================================================================
// GDT Pointer (GDTR)
// =============================================================================

/// The value loaded into the GDTR register via LGDT.
struct __attribute__((packed)) GdtPointer {
    uint16_t limit;     // Size of the GDT in bytes, minus 1
    uint64_t base;      // Linear (virtual) address of the GDT
};

static_assert(sizeof(GdtPointer) == 10, "GDTR must be 10 bytes");

// =============================================================================
// GDT Table
// =============================================================================

/// Total number of GDT entries (null + kernel code + kernel data +
/// user code + user data + TSS (2 entries) = 7 entries)
inline constexpr uint32_t GDT_ENTRY_COUNT = 7;

/// The complete GDT structure including all segments and the TSS.
/// Uses a union to overlay the TSS descriptor across entries 5-6.
struct Gdt {
    GdtEntry entries[GDT_ENTRY_COUNT];

    /// Get a mutable reference to the TSS descriptor (entries 5-6)
    TssDescriptor& tss_descriptor() {
        return *reinterpret_cast<TssDescriptor*>(&entries[5]);
    }

    /// Initialize the GDT with standard segment descriptors
    void init() {
        entries[0] = GdtEntry::null();
        entries[1] = GdtEntry::code_segment(0);  // Kernel code
        entries[2] = GdtEntry::data_segment(0);  // Kernel data
        entries[3] = GdtEntry::code_segment(3);  // User code
        entries[4] = GdtEntry::data_segment(3);  // User data
        // Entries 5-6 are set up via install_tss()
        entries[5] = GdtEntry::null();
        entries[6] = GdtEntry::null();
    }
};

// =============================================================================
// Public API
// =============================================================================

/// Initialize and load the GDT.
/// Sets up the standard segments and loads the GDTR. Also reloads CS (via
/// a far return) and the data segment registers.
void init_gdt();

/// Install the TSS into the GDT and load the TR (Task Register).
/// Must be called after init_gdt() and after the TSS is populated with
/// valid stack pointers.
/// @param tss Pointer to the TSS structure (must remain valid).
void install_tss(Tss* tss);

/// Load the GDT pointer into the GDTR register.
/// After this, segment registers must be reloaded to pick up the new descriptors.
void load_gdt(const GdtPointer& gdtr);

/// Reload all segment registers after a GDT change.
/// CS is reloaded via a far return; DS, ES, SS, FS, GS are loaded directly.
void reload_segments();

/// Load the Task Register with the TSS selector.
void load_tr(uint16_t selector);

/// Get a reference to the global GDT instance
Gdt& get_gdt();

/// Get a reference to the global TSS instance
Tss& get_tss();

} // namespace bminos::arch::x86_64
