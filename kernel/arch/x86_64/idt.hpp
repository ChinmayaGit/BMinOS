// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Interrupt Descriptor Table (IDT) — x86_64
// ==========================================
// The IDT maps interrupt/exception vectors (0-255) to handler functions.
// When an interrupt fires, the CPU:
//   1. Pushes SS, RSP, RFLAGS, CS, RIP (and error code for some exceptions)
//   2. Looks up the vector in the IDT
//   3. Loads the handler's CS:RIP from the IDT entry
//   4. Optionally switches to an IST stack
//   5. Begins executing the handler
//
// Vector assignments:
//   0-31:   CPU exceptions (divide error, page fault, etc.)
//   32-47:  Legacy IRQs (remapped via IOAPIC in our case)
//   48-255: Software interrupts, IPIs, etc.
//
// Our handler strategy:
// - Assembly stubs for each vector that save all registers, push the vector
//   number, and call a common C++ dispatch function.
// - The dispatch function looks up a registered handler and calls it.
// - This two-stage approach keeps the assembly minimal and the logic in C++.

#pragma once

#include <cstdint>

namespace bminos::arch::x86_64 {

// =============================================================================
// Constants
// =============================================================================

/// Total number of IDT entries (256 vectors)
inline constexpr uint32_t IDT_ENTRY_COUNT = 256;

/// Exception vectors (CPU-defined, 0-31)
inline constexpr uint8_t EXCEPTION_DIVIDE_ERROR         = 0;
inline constexpr uint8_t EXCEPTION_DEBUG                = 1;
inline constexpr uint8_t EXCEPTION_NMI                  = 2;
inline constexpr uint8_t EXCEPTION_BREAKPOINT           = 3;
inline constexpr uint8_t EXCEPTION_OVERFLOW             = 4;
inline constexpr uint8_t EXCEPTION_BOUND_RANGE          = 5;
inline constexpr uint8_t EXCEPTION_INVALID_OPCODE       = 6;
inline constexpr uint8_t EXCEPTION_DEVICE_NOT_AVAILABLE = 7;
inline constexpr uint8_t EXCEPTION_DOUBLE_FAULT         = 8;
inline constexpr uint8_t EXCEPTION_COPROCESSOR_OVERRUN  = 9;  // Legacy, not used
inline constexpr uint8_t EXCEPTION_INVALID_TSS          = 10;
inline constexpr uint8_t EXCEPTION_SEGMENT_NOT_PRESENT  = 11;
inline constexpr uint8_t EXCEPTION_STACK_FAULT          = 12;
inline constexpr uint8_t EXCEPTION_GENERAL_PROTECTION   = 13;
inline constexpr uint8_t EXCEPTION_PAGE_FAULT           = 14;
// Vector 15 is reserved
inline constexpr uint8_t EXCEPTION_X87_FLOAT            = 16;
inline constexpr uint8_t EXCEPTION_ALIGNMENT_CHECK      = 17;
inline constexpr uint8_t EXCEPTION_MACHINE_CHECK        = 18;
inline constexpr uint8_t EXCEPTION_SIMD_FLOAT           = 19;
inline constexpr uint8_t EXCEPTION_VIRTUALIZATION       = 20;
inline constexpr uint8_t EXCEPTION_CONTROL_PROTECTION   = 21;
// Vectors 22-31 are reserved

/// First IRQ vector (after PIC remapping)
inline constexpr uint8_t IRQ_BASE = 32;

/// Specific IRQ assignments
inline constexpr uint8_t IRQ_TIMER          = IRQ_BASE + 0;   // PIT / LAPIC timer
inline constexpr uint8_t IRQ_KEYBOARD       = IRQ_BASE + 1;
inline constexpr uint8_t IRQ_CASCADE        = IRQ_BASE + 2;   // PIC cascade (unused with IOAPIC)
inline constexpr uint8_t IRQ_COM2           = IRQ_BASE + 3;
inline constexpr uint8_t IRQ_COM1           = IRQ_BASE + 4;
inline constexpr uint8_t IRQ_LPT2           = IRQ_BASE + 5;
inline constexpr uint8_t IRQ_FLOPPY         = IRQ_BASE + 6;
inline constexpr uint8_t IRQ_SPURIOUS       = IRQ_BASE + 7;

/// Software interrupt vectors
inline constexpr uint8_t VECTOR_SYSCALL     = 0x80;  // Hypercall vector
inline constexpr uint8_t VECTOR_SPURIOUS    = 0xFF;  // APIC spurious interrupt
inline constexpr uint8_t VECTOR_IPI_TLB     = 0xFE;  // TLB shootdown IPI
inline constexpr uint8_t VECTOR_IPI_HALT    = 0xFD;  // Halt IPI (for shutdown)
inline constexpr uint8_t VECTOR_IPI_RESCHED = 0xFC;  // Reschedule IPI

// =============================================================================
// IDT Gate Types
// =============================================================================

/// Gate type field values for IDT entries in 64-bit mode.
/// In long mode, only interrupt gates and trap gates are valid.
enum class GateType : uint8_t {
    Interrupt   = 0x0E,  // Clears IF (interrupts disabled in handler)
    Trap        = 0x0F,  // Does NOT clear IF (interrupts remain enabled)
};

// =============================================================================
// IDT Entry
// =============================================================================

/// A 64-bit mode IDT entry (16 bytes).
/// Each entry describes a gate — where to jump when this vector fires.
struct __attribute__((packed)) IdtEntry {
    uint16_t offset_low;    // Handler address bits 0-15
    uint16_t selector;      // Code segment selector (usually SEGMENT_KERNEL_CODE)
    uint8_t  ist;           // Interrupt Stack Table index (0 = no IST, 1-7 = IST entry)
    uint8_t  type_attr;     // Type (4 bits), 0 (1 bit), DPL (2 bits), Present (1 bit)
    uint16_t offset_mid;    // Handler address bits 16-31
    uint32_t offset_high;   // Handler address bits 32-63
    uint32_t reserved;      // Must be zero

    /// Create a null (not-present) IDT entry
    static constexpr IdtEntry null() {
        return {0, 0, 0, 0, 0, 0, 0};
    }

    /// Create an IDT entry for a handler.
    /// @param handler Address of the handler function (assembly stub).
    /// @param selector Code segment selector.
    /// @param ist IST entry to use (0 = don't switch stacks).
    /// @param gate_type Interrupt gate (clears IF) or trap gate (preserves IF).
    /// @param dpl Descriptor Privilege Level required to invoke via INT instruction.
    static IdtEntry create(uint64_t handler, uint16_t selector,
                           uint8_t ist, GateType gate_type, uint8_t dpl) {
        IdtEntry entry{};
        entry.offset_low  = static_cast<uint16_t>(handler & 0xFFFF);
        entry.selector    = selector;
        entry.ist         = ist & 0x07;
        // type_attr: P=1, DPL, 0, Type
        entry.type_attr   = static_cast<uint8_t>(
            0x80 |                              // Present = 1
            ((dpl & 0x3) << 5) |                // DPL
            static_cast<uint8_t>(gate_type)     // Gate type
        );
        entry.offset_mid  = static_cast<uint16_t>((handler >> 16) & 0xFFFF);
        entry.offset_high = static_cast<uint32_t>((handler >> 32) & 0xFFFFFFFF);
        entry.reserved    = 0;
        return entry;
    }
};

static_assert(sizeof(IdtEntry) == 16, "IDT entry must be exactly 16 bytes");

// =============================================================================
// IDT Pointer (IDTR)
// =============================================================================

/// The value loaded into the IDTR register via LIDT.
struct __attribute__((packed)) IdtPointer {
    uint16_t limit;     // Size of the IDT in bytes, minus 1
    uint64_t base;      // Linear (virtual) address of the IDT
};

static_assert(sizeof(IdtPointer) == 10, "IDTR must be 10 bytes");

// =============================================================================
// Interrupt Frame
// =============================================================================

/// The stack frame pushed by the CPU when an interrupt/exception occurs.
/// Our assembly stubs push additional registers and the vector number on top.
///
/// Stack layout (from top, lowest address first):
///   [pushed by our stub]
///   - All general-purpose registers (GuestRegisters or similar)
///   - Vector number
///   - Error code (pushed by CPU for some exceptions, or 0 pushed by stub)
///   [pushed by CPU]
///   - RIP        — return address
///   - CS         — code segment
///   - RFLAGS     — flags register
///   - RSP        — stack pointer (always pushed in 64-bit mode)
///   - SS         — stack segment (always pushed in 64-bit mode)
struct __attribute__((packed)) InterruptFrame {
    // General-purpose registers (pushed by our assembly stub)
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;

    // Pushed by our stub
    uint64_t vector;        // Interrupt/exception vector number
    uint64_t error_code;    // Error code (0 if not applicable)

    // Pushed by CPU automatically
    uint64_t rip;           // Instruction pointer at time of interrupt
    uint64_t cs;            // Code segment
    uint64_t rflags;        // CPU flags
    uint64_t rsp;           // Stack pointer
    uint64_t ss;            // Stack segment
};

// =============================================================================
// Interrupt Handler Type
// =============================================================================

/// Signature for C++ interrupt handler functions.
/// The handler receives the full interrupt frame and can modify it
/// (e.g., to change the return RIP for emulation purposes).
using InterruptHandler = void(*)(InterruptFrame* frame);

// =============================================================================
// IDT Manager
// =============================================================================

/// Manages the Interrupt Descriptor Table and interrupt handler registration.
///
/// Usage:
///   1. init_idt() — set up exception handlers and default interrupt stubs
///   2. register_handler() — install handlers for specific vectors
///   3. load_idt() — load the IDTR to activate the IDT
class IdtManager {
public:
    IdtManager() = default;

    // Non-copyable
    IdtManager(const IdtManager&) = delete;
    IdtManager& operator=(const IdtManager&) = delete;

    /// Initialize the IDT with default handlers for all 256 vectors.
    /// CPU exceptions (0-31) get specific handlers. IRQs and software interrupts
    /// get generic stubs that call the dispatch function.
    void init();

    /// Register a handler for a specific interrupt vector.
    /// Replaces any previously registered handler for this vector.
    /// @param vector The interrupt vector (0-255).
    /// @param handler The handler function to call.
    void register_handler(uint8_t vector, InterruptHandler handler);

    /// Unregister a handler for a specific vector (reverts to default handler).
    void unregister_handler(uint8_t vector);

    /// Get the currently registered handler for a vector.
    /// @return The handler, or nullptr if no custom handler is registered.
    [[nodiscard]] InterruptHandler get_handler(uint8_t vector) const;

    /// Load the IDT into the IDTR register (LIDT instruction).
    void load();

    /// Set the IST (Interrupt Stack Table) entry for a specific vector.
    /// This causes the CPU to switch to the specified IST stack when this
    /// vector fires. Used for critical exceptions (NMI, double fault, MCE)
    /// that need a known-good stack.
    /// @param vector The interrupt vector.
    /// @param ist IST entry number (1-7). Use 0 to disable IST for this vector.
    void set_ist(uint8_t vector, uint8_t ist);

    /// Dispatch function called from the common assembly interrupt stub.
    /// Looks up the registered handler and calls it.
    /// This is NOT called directly — it's invoked from assembly.
    static void dispatch(InterruptFrame* frame);

    /// Set a specific IDT entry directly (for advanced use cases).
    void set_entry(uint8_t vector, const IdtEntry& entry);

    /// Get a reference to a specific IDT entry.
    [[nodiscard]] const IdtEntry& get_entry(uint8_t vector) const;

private:
    /// Set up an IDT entry for an interrupt gate.
    /// @param vector The vector number.
    /// @param handler The assembly stub address.
    /// @param ist IST entry (0 for no stack switch).
    /// @param dpl Privilege level (0 for kernel-only, 3 for user-accessible).
    void set_gate(uint8_t vector, uint64_t handler, uint8_t ist = 0, uint8_t dpl = 0);

    /// The IDT entries (256 × 16 bytes = 4096 bytes = exactly 1 page)
    IdtEntry m_entries[IDT_ENTRY_COUNT] = {};

    /// Registered C++ handlers for each vector (dispatch table)
    InterruptHandler m_handlers[IDT_ENTRY_COUNT] = {};

    /// The IDTR value
    IdtPointer m_idtr = {};
};

/// Global IDT manager instance
extern IdtManager g_idt;

// =============================================================================
// Public API
// =============================================================================

/// Initialize and load the IDT with default exception and interrupt handlers.
void init_idt();

/// Register a C++ handler for an interrupt vector.
void register_interrupt_handler(uint8_t vector, InterruptHandler handler);

/// Load the IDT (LIDT instruction).
void load_idt();

// =============================================================================
// Exception names (for debug/panic output)
// =============================================================================

/// Get a human-readable name for an exception vector (0-31).
/// @return Exception name string, or "Unknown" for non-exception vectors.
const char* exception_name(uint8_t vector);

/// Check if an exception vector pushes an error code.
/// Some exceptions (8, 10-14, 17, 21, 29, 30) push an error code;
/// our assembly stubs push a dummy 0 for the others to keep a uniform frame.
[[nodiscard]] constexpr bool exception_has_error_code(uint8_t vector) {
    // Exceptions that push error codes: 8, 10, 11, 12, 13, 14, 17, 21, 29, 30
    switch (vector) {
        case 8: case 10: case 11: case 12: case 13: case 14:
        case 17: case 21: case 29: case 30:
            return true;
        default:
            return false;
    }
}

} // namespace bminos::arch::x86_64
