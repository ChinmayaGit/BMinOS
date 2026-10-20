// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// kernel/core/serial.hpp — Serial console output for early boot and debug logging.
// On x86_64, this drives the legacy COM1 UART (I/O port 0x3F8).
// On ARM64, this drives a PL011 UART at a configurable MMIO base address.
// The serial console is the first output device available and is used for all
// diagnostic output before (and after) graphical consoles are initialized.

#pragma once

#include "types.hpp"

namespace bminos {

// ============================================================================
// Log Levels
// ============================================================================
// Log levels control verbosity. In release builds, DEBUG messages can be
// compiled out entirely via NEXUS_MIN_LOG_LEVEL.

enum class LogLevel : u8 {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
    Fatal = 4,
};

/// Returns a short tag string for a log level (for formatting log lines).
constexpr const char* log_level_tag(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return " INFO";
        case LogLevel::Warn:  return " WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

// ============================================================================
// Serial Console Interface
// ============================================================================

class Serial {
public:
    /// Initialize the serial port hardware.
    /// On x86_64: programs the 8250/16550 UART divisor, line control, FIFO.
    /// On ARM64: configures the PL011 baud rate, line control, enables TX.
    static void init();

    /// Write a single character to the serial port.
    /// Spins waiting for the transmit buffer to be ready (busy-wait is
    /// acceptable for debug output; this is not a high-throughput path).
    static void putchar(char c);

    /// Write a null-terminated string.
    static void puts(const char* str);

    /// Write exactly `len` characters (may include embedded nulls).
    static void write(const char* data, usize len);

private:
#if BMINOS_ARCH_X86_64
    // x86_64 COM1 port — the de facto standard debug serial port.
    // Nearly all x86 hardware and all virtual machines support this.
    static constexpr u16 COM1_PORT = 0x3F8;

    // 8250/16550 UART register offsets from base port.
    static constexpr u16 REG_DATA          = 0;  // TX/RX data (DLAB=0)
    static constexpr u16 REG_INT_ENABLE    = 1;  // Interrupt enable (DLAB=0)
    static constexpr u16 REG_DIVISOR_LOW   = 0;  // Divisor latch low (DLAB=1)
    static constexpr u16 REG_DIVISOR_HIGH  = 1;  // Divisor latch high (DLAB=1)
    static constexpr u16 REG_LINE_CTRL     = 3;  // Line control register
    static constexpr u16 REG_MODEM_CTRL    = 4;  // Modem control register
    static constexpr u16 REG_LINE_STATUS   = 5;  // Line status register
    static constexpr u16 REG_FIFO_CTRL     = 2;  // FIFO control register

    /// Write a byte to an I/O port.
    static inline void outb(u16 port, u8 value) {
        asm volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
    }

    /// Read a byte from an I/O port.
    static inline u8 inb(u16 port) {
        u8 value;
        asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
        return value;
    }

#elif BMINOS_ARCH_ARM64
    // PL011 UART base address — this is platform-specific.
    // QEMU virt machine uses 0x09000000. Real hardware will differ.
    // TODO: Get this from device tree at runtime.
    static constexpr u64 PL011_BASE = 0x09000000;

    // PL011 register offsets
    static constexpr u32 UARTDR   = 0x000;  // Data register
    static constexpr u32 UARTFR   = 0x018;  // Flag register
    static constexpr u32 UARTIBRD = 0x024;  // Integer baud rate divisor
    static constexpr u32 UARTFBRD = 0x028;  // Fractional baud rate divisor
    static constexpr u32 UARTLCR  = 0x02C;  // Line control register
    static constexpr u32 UARTCR   = 0x030;  // Control register
    static constexpr u32 UARTIMSC = 0x038;  // Interrupt mask set/clear

    // Flag register bits
    static constexpr u32 FR_TXFF  = (1 << 5);  // Transmit FIFO full
    static constexpr u32 FR_BUSY  = (1 << 3);  // UART busy

    /// Write a 32-bit value to a MMIO register.
    static inline void mmio_write(u64 addr, u32 value) {
        *reinterpret_cast<volatile u32*>(addr) = value;
    }

    /// Read a 32-bit value from a MMIO register.
    static inline u32 mmio_read(u64 addr) {
        return *reinterpret_cast<volatile u32*>(addr);
    }
#endif

    static bool initialized_;
};

// ============================================================================
// Formatted Output — kprintf
// ============================================================================
// A minimal printf implementation for the kernel. Supports:
//   %s  — string
//   %c  — character
//   %d  — signed decimal integer
//   %u  — unsigned decimal integer
//   %x  — unsigned hexadecimal (lowercase)
//   %X  — unsigned hexadecimal (uppercase)
//   %p  — pointer (prints as 0x...)
//   %l  — long modifier (e.g., %lu, %lx, %ld)
//   %%  — literal percent sign
//
// This is intentionally simpler than libc printf — we don't support
// width specifiers, padding, or floating point, as they are unnecessary
// for kernel diagnostics.

/// Low-level formatted print to serial console.
void kprintf(const char* fmt, ...);

/// Formatted print with log level prefix: "[LEVEL] message\n"
void klog(LogLevel level, const char* fmt, ...);

// ============================================================================
// Logging Macros
// ============================================================================
// These macros add file/line info in debug builds and can be compiled out
// based on NEXUS_MIN_LOG_LEVEL.

#ifndef NEXUS_MIN_LOG_LEVEL
    #define NEXUS_MIN_LOG_LEVEL 0  // Show everything by default
#endif

#define LOG_DEBUG(fmt, ...) \
    do { if constexpr (NEXUS_MIN_LOG_LEVEL <= 0) bminos::klog(bminos::LogLevel::Debug, fmt __VA_OPT__(,) __VA_ARGS__); } while(0)

#define LOG_INFO(fmt, ...) \
    do { if constexpr (NEXUS_MIN_LOG_LEVEL <= 1) bminos::klog(bminos::LogLevel::Info, fmt __VA_OPT__(,) __VA_ARGS__); } while(0)

#define LOG_WARN(fmt, ...) \
    do { if constexpr (NEXUS_MIN_LOG_LEVEL <= 2) bminos::klog(bminos::LogLevel::Warn, fmt __VA_OPT__(,) __VA_ARGS__); } while(0)

#define LOG_ERROR(fmt, ...) \
    do { if constexpr (NEXUS_MIN_LOG_LEVEL <= 3) bminos::klog(bminos::LogLevel::Error, fmt __VA_OPT__(,) __VA_ARGS__); } while(0)

#define LOG_FATAL(fmt, ...) \
    do { bminos::klog(bminos::LogLevel::Fatal, fmt __VA_OPT__(,) __VA_ARGS__); } while(0)

} // namespace bminos
