// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// kernel/core/panic.hpp — Kernel panic and assertion infrastructure.
// When something goes catastrophically wrong, we want to dump as much
// diagnostic information as possible before halting all CPUs. This is
// the last line of defense for catching bugs in the hypervisor.

#pragma once

#include "types.hpp"

namespace bminos {

// ============================================================================
// Panic — Unrecoverable error handler
// ============================================================================

/// Halt the system with an error message. This function never returns.
/// It will:
///   1. Disable interrupts on the current CPU
///   2. Dump CPU registers to serial console
///   3. Walk the stack and print a backtrace
///   4. Print the panic message with source location
///   5. Send an IPI to halt all other CPUs (if SMP is active)
///   6. Enter an infinite halt loop
///
/// The BMINOS_NORETURN attribute lets the compiler optimize callers by
/// eliminating any code after a panic() call.
BMINOS_NORETURN void panic(const char* message);

/// Panic with formatted message support.
BMINOS_NORETURN void panic_fmt(const char* fmt, ...);

/// Panic with source location information — called by BMINOS_ASSERT.
BMINOS_NORETURN void panic_at(const char* file, u32 line, const char* message);

// ============================================================================
// Register State
// ============================================================================
// Architecture-specific register dump structures. These capture the full
// CPU state at the time of a panic for post-mortem analysis.

#if BMINOS_ARCH_X86_64

struct RegisterState {
    // General-purpose registers
    u64 rax, rbx, rcx, rdx;
    u64 rsi, rdi, rbp, rsp;
    u64 r8, r9, r10, r11;
    u64 r12, r13, r14, r15;
    // Instruction pointer and flags
    u64 rip;
    u64 rflags;
    // Segment registers (mostly vestigial in long mode, but still relevant
    // for checking if we're in a sane state)
    u16 cs, ds, es, fs, gs, ss;
    // Control registers — critical for diagnosing page faults and
    // virtualization issues
    u64 cr0, cr2, cr3, cr4;
} BMINOS_PACKED;

#elif BMINOS_ARCH_ARM64

struct RegisterState {
    // General-purpose registers x0-x30
    u64 x[31];
    // Stack pointer (SP_EL1 for hypervisor)
    u64 sp;
    // Exception link register (return address for exception)
    u64 elr;
    // Saved program status register
    u64 spsr;
    // Exception syndrome register — tells us WHY we faulted
    u64 esr;
    // Fault address register
    u64 far;
} BMINOS_PACKED;

#endif

/// Capture current CPU registers into the provided structure.
void capture_registers(RegisterState& state);

/// Print a register dump to the serial console.
void dump_registers(const RegisterState& state);

// ============================================================================
// Stack Trace
// ============================================================================
// Basic frame-pointer-based stack walking. This requires compilation with
// -fno-omit-frame-pointer (which we enforce in our build system) so that
// RBP/x29 always points to the previous frame.

/// Maximum number of stack frames to unwind. This prevents infinite loops
/// if the frame chain is corrupted.
inline constexpr usize MAX_STACK_FRAMES = 32;

/// A single frame in the call stack.
struct StackFrame {
    u64 return_address;
    u64 frame_pointer;
};

/// Walk the stack from the current frame pointer and print addresses.
/// Returns the number of frames found.
usize dump_stack_trace();

/// Walk the stack starting from a given frame pointer.
usize dump_stack_trace_from(u64 frame_pointer);

// ============================================================================
// Assertion Macro
// ============================================================================
// BMINOS_ASSERT is our primary debugging tool. In debug builds, failed
// assertions dump full diagnostic info. In release builds, they can be
// compiled to __builtin_unreachable() to help the optimizer, or left
// as-is for safety.

#ifdef NEXUS_RELEASE
    // In release builds, failed assertions are undefined behavior hints.
    // This is aggressive but produces better code. If you want safe release
    // builds, change this to the debug version.
    #define BMINOS_ASSERT(condition, message) \
        do { \
            if (!(condition)) { \
                __builtin_unreachable(); \
            } \
        } while (0)
#else
    #define BMINOS_ASSERT(condition, message) \
        do { \
            if (!(condition)) [[unlikely]] { \
                bminos::panic_at(__FILE__, __LINE__, \
                    "Assertion failed: " #condition " — " message); \
            } \
        } while (0)
#endif

/// Always-on assertion that is never compiled out. Use sparingly for
/// invariants that, if violated, would cause data corruption or security
/// vulnerabilities.
#define BMINOS_ASSERT_ALWAYS(condition, message) \
    do { \
        if (!(condition)) [[unlikely]] { \
            bminos::panic_at(__FILE__, __LINE__, \
                "Critical assertion failed: " #condition " — " message); \
        } \
    } while (0)

} // namespace bminos
