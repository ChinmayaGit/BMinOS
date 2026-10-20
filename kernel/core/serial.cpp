// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// kernel/core/serial.cpp — Serial console implementation.
// This is one of the first pieces of code that runs after the bootloader
// hands off to the hypervisor. Getting serial output working is critical
// because it's our only debugging interface during early boot.

#include "serial.hpp"

// We need va_list for kprintf. The compiler provides <stdarg.h> even in
// freestanding mode — it's one of the few headers guaranteed available.
// Using __builtin_va_* directly to avoid any header dependency.
using va_list = __builtin_va_list;
#define va_start(ap, param) __builtin_va_start(ap, param)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)

namespace bminos {

// Static member initialization
bool Serial::initialized_ = false;

// ============================================================================
// Serial Port Initialization
// ============================================================================

void Serial::init() {
    if (initialized_) return;

#if BMINOS_ARCH_X86_64
    // Program the 8250/16550 UART for 115200 baud, 8N1.
    //
    // The initialization sequence:
    // 1. Disable all interrupts (we poll)
    // 2. Set baud rate divisor (115200 baud = divisor 1)
    // 3. Configure 8 data bits, no parity, 1 stop bit (8N1)
    // 4. Enable and clear FIFOs with 14-byte threshold
    // 5. Set DTR + RTS + OUT2 (OUT2 is needed for interrupts, even though
    //    we poll — some hardware requires it)

    outb(COM1_PORT + REG_INT_ENABLE, 0x00);   // Disable all interrupts

    outb(COM1_PORT + REG_LINE_CTRL, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(COM1_PORT + REG_DIVISOR_LOW, 0x01);  // Set divisor to 1 (115200 baud)
    outb(COM1_PORT + REG_DIVISOR_HIGH, 0x00);

    outb(COM1_PORT + REG_LINE_CTRL, 0x03);    // 8 bits, no parity, 1 stop bit
    outb(COM1_PORT + REG_FIFO_CTRL, 0xC7);    // Enable FIFO, clear, 14-byte threshold
    outb(COM1_PORT + REG_MODEM_CTRL, 0x0B);   // DTR + RTS + OUT2

    // Perform a loopback test to verify the UART is functional.
    // Set bit 4 of modem control (loopback mode), send a byte, and check
    // if we receive it back.
    outb(COM1_PORT + REG_MODEM_CTRL, 0x1E);   // Enable loopback mode
    outb(COM1_PORT + REG_DATA, 0xAE);          // Send test byte

    // Check if we get the same byte back
    if (inb(COM1_PORT + REG_DATA) != 0xAE) {
        // UART is not working — we can't do much about this, but we
        // set initialized to true anyway so we don't loop forever trying.
        initialized_ = true;
        return;
    }

    // Test passed — switch back to normal operation mode.
    // Set OUT1 + OUT2 + DTR + RTS for normal operation.
    outb(COM1_PORT + REG_MODEM_CTRL, 0x0F);

#elif BMINOS_ARCH_ARM64
    // PL011 UART initialization for QEMU virt machine.
    //
    // On real ARM hardware, the UART base address and clock frequency
    // would come from the device tree. For now, we hardcode the QEMU
    // virt platform values.

    // Disable the UART while we configure it
    mmio_write(PL011_BASE + UARTCR, 0x00000000);

    // Clear all pending interrupts
    mmio_write(PL011_BASE + UARTIMSC, 0x00000000);

    // Set baud rate to 115200.
    // QEMU's PL011 clock is 24 MHz.
    // Divisor = 24000000 / (16 * 115200) = 13.0208...
    // Integer part = 13, Fractional part = round(0.0208 * 64) = 1
    mmio_write(PL011_BASE + UARTIBRD, 13);
    mmio_write(PL011_BASE + UARTFBRD, 1);

    // Configure line control: 8 bits, no parity, 1 stop bit, enable FIFOs.
    // Bits [6:5] = 11 (8-bit word length), bit 4 = 1 (enable FIFOs)
    mmio_write(PL011_BASE + UARTLCR, (3 << 5) | (1 << 4));

    // Enable UART, TX, and RX
    mmio_write(PL011_BASE + UARTCR, (1 << 0) | (1 << 8) | (1 << 9));
#endif

    initialized_ = true;
}

// ============================================================================
// Character Output
// ============================================================================

void Serial::putchar(char c) {
    // Auto-initialize if someone calls putchar before explicit init.
    // This is a safety net for very early debug prints.
    if (!initialized_) init();

    // Convert \n to \r\n for terminals that expect CRLF.
    if (c == '\n') {
        putchar('\r');
    }

#if BMINOS_ARCH_X86_64
    // Wait for the transmit holding register to be empty.
    // Bit 5 of the Line Status Register (LSR) indicates the THR is empty.
    // In practice, this loop is very short — the UART is much slower than
    // the CPU, but the FIFO handles most of the buffering.
    while ((inb(COM1_PORT + REG_LINE_STATUS) & 0x20) == 0) {
        // Spin. Could add a pause instruction here, but for debug output
        // the latency is irrelevant.
        asm volatile("pause");
    }
    outb(COM1_PORT + REG_DATA, static_cast<u8>(c));

#elif BMINOS_ARCH_ARM64
    // Wait for the transmit FIFO to have space.
    // FR_TXFF (bit 5 of flag register) is set when FIFO is full.
    while (mmio_read(PL011_BASE + UARTFR) & FR_TXFF) {
        asm volatile("yield");  // ARM hint: we're in a spin loop
    }
    mmio_write(PL011_BASE + UARTDR, static_cast<u32>(c));
#endif
}

void Serial::puts(const char* str) {
    if (!str) return;
    while (*str) {
        putchar(*str++);
    }
}

void Serial::write(const char* data, usize len) {
    for (usize i = 0; i < len; i++) {
        putchar(data[i]);
    }
}

// ============================================================================
// Number-to-String Conversion Helpers
// ============================================================================

namespace {

/// Convert an unsigned 64-bit integer to a string in the given base.
/// Returns the number of characters written. Buffer must be at least 65 bytes
/// (64 binary digits + null terminator).
usize u64_to_str(u64 value, char* buf, usize buf_size, int base, bool uppercase) {
    static constexpr char digits_lower[] = "0123456789abcdef";
    static constexpr char digits_upper[] = "0123456789ABCDEF";
    const char* digits = uppercase ? digits_upper : digits_lower;

    if (buf_size == 0) return 0;

    // Build the string in reverse order, then reverse it.
    char tmp[65];
    usize len = 0;

    if (value == 0) {
        tmp[len++] = '0';
    } else {
        while (value > 0 && len < 64) {
            tmp[len++] = digits[value % static_cast<u64>(base)];
            value /= static_cast<u64>(base);
        }
    }

    // Reverse into output buffer
    usize out_len = (len < buf_size - 1) ? len : buf_size - 1;
    for (usize i = 0; i < out_len; i++) {
        buf[i] = tmp[len - 1 - i];
    }
    buf[out_len] = '\0';
    return out_len;
}

/// Convert a signed 64-bit integer to a decimal string.
usize i64_to_str(i64 value, char* buf, usize buf_size) {
    if (buf_size == 0) return 0;

    usize offset = 0;
    if (value < 0) {
        buf[0] = '-';
        offset = 1;
        // Handle INT64_MIN specially to avoid overflow on negation
        if (value == static_cast<i64>(0x8000000000000000ULL)) {
            // -9223372036854775808
            const char* min_str = "9223372036854775808";
            usize i = 0;
            while (min_str[i] && offset + i < buf_size - 1) {
                buf[offset + i] = min_str[i];
                i++;
            }
            buf[offset + i] = '\0';
            return offset + i;
        }
        value = -value;
    }

    usize len = u64_to_str(static_cast<u64>(value), buf + offset, buf_size - offset, 10, false);
    return offset + len;
}

} // anonymous namespace

// ============================================================================
// kprintf — Kernel printf implementation
// ============================================================================

void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    char num_buf[65];  // Enough for 64-bit binary + null

    while (*fmt) {
        if (*fmt != '%') {
            Serial::putchar(*fmt++);
            continue;
        }

        fmt++;  // Skip '%'

        // Check for long modifier
        bool is_long = false;
        if (*fmt == 'l') {
            is_long = true;
            fmt++;
            // Handle 'll' (same as 'l' for us since our integers are 64-bit)
            if (*fmt == 'l') fmt++;
        }

        switch (*fmt) {
            case '\0':
                // Format string ended with a lone '%'
                goto done;

            case '%':
                Serial::putchar('%');
                break;

            case 'c': {
                // char is promoted to int when passed through va_arg
                char c = static_cast<char>(va_arg(args, int));
                Serial::putchar(c);
                break;
            }

            case 's': {
                const char* str = va_arg(args, const char*);
                Serial::puts(str ? str : "(null)");
                break;
            }

            case 'd':
            case 'i': {
                i64 value;
                if (is_long) {
                    value = va_arg(args, i64);
                } else {
                    value = va_arg(args, int);
                }
                i64_to_str(value, num_buf, sizeof(num_buf));
                Serial::puts(num_buf);
                break;
            }

            case 'u': {
                u64 value;
                if (is_long) {
                    value = va_arg(args, u64);
                } else {
                    value = va_arg(args, unsigned int);
                }
                u64_to_str(value, num_buf, sizeof(num_buf), 10, false);
                Serial::puts(num_buf);
                break;
            }

            case 'x': {
                u64 value;
                if (is_long) {
                    value = va_arg(args, u64);
                } else {
                    value = va_arg(args, unsigned int);
                }
                u64_to_str(value, num_buf, sizeof(num_buf), 16, false);
                Serial::puts(num_buf);
                break;
            }

            case 'X': {
                u64 value;
                if (is_long) {
                    value = va_arg(args, u64);
                } else {
                    value = va_arg(args, unsigned int);
                }
                u64_to_str(value, num_buf, sizeof(num_buf), 16, true);
                Serial::puts(num_buf);
                break;
            }

            case 'p': {
                // Pointers are always printed as 0x followed by hex.
                void* ptr = va_arg(args, void*);
                Serial::puts("0x");
                u64_to_str(reinterpret_cast<u64>(ptr), num_buf, sizeof(num_buf), 16, false);
                Serial::puts(num_buf);
                break;
            }

            default:
                // Unknown format specifier — print it literally so the
                // developer can see something went wrong.
                Serial::putchar('%');
                if (is_long) Serial::putchar('l');
                Serial::putchar(*fmt);
                break;
        }

        fmt++;
    }

done:
    va_end(args);
}

// ============================================================================
// klog — Formatted logging with level prefix
// ============================================================================

void klog(LogLevel level, const char* fmt, ...) {
    // Print prefix: "[LEVEL] "
    Serial::putchar('[');
    Serial::puts(log_level_tag(level));
    Serial::puts("] ");

    // Print the formatted message using va_list forwarding.
    // Since we can't directly forward va_args to kprintf, we duplicate
    // the formatting logic here. This is a pragmatic trade-off — the
    // alternative (vkprintf with va_list) adds complexity for little gain.
    va_list args;
    va_start(args, fmt);

    char num_buf[65];

    while (*fmt) {
        if (*fmt != '%') {
            Serial::putchar(*fmt++);
            continue;
        }

        fmt++;

        bool is_long = false;
        if (*fmt == 'l') {
            is_long = true;
            fmt++;
            if (*fmt == 'l') fmt++;
        }

        switch (*fmt) {
            case '\0': goto log_done;
            case '%': Serial::putchar('%'); break;
            case 'c': {
                char c = static_cast<char>(va_arg(args, int));
                Serial::putchar(c);
                break;
            }
            case 's': {
                const char* str = va_arg(args, const char*);
                Serial::puts(str ? str : "(null)");
                break;
            }
            case 'd': case 'i': {
                i64 value = is_long ? va_arg(args, i64) : va_arg(args, int);
                i64_to_str(value, num_buf, sizeof(num_buf));
                Serial::puts(num_buf);
                break;
            }
            case 'u': {
                u64 value = is_long ? va_arg(args, u64) : va_arg(args, unsigned int);
                u64_to_str(value, num_buf, sizeof(num_buf), 10, false);
                Serial::puts(num_buf);
                break;
            }
            case 'x': {
                u64 value = is_long ? va_arg(args, u64) : va_arg(args, unsigned int);
                u64_to_str(value, num_buf, sizeof(num_buf), 16, false);
                Serial::puts(num_buf);
                break;
            }
            case 'X': {
                u64 value = is_long ? va_arg(args, u64) : va_arg(args, unsigned int);
                u64_to_str(value, num_buf, sizeof(num_buf), 16, true);
                Serial::puts(num_buf);
                break;
            }
            case 'p': {
                void* ptr = va_arg(args, void*);
                Serial::puts("0x");
                u64_to_str(reinterpret_cast<u64>(ptr), num_buf, sizeof(num_buf), 16, false);
                Serial::puts(num_buf);
                break;
            }
            default:
                Serial::putchar('%');
                if (is_long) Serial::putchar('l');
                Serial::putchar(*fmt);
                break;
        }

        fmt++;
    }

log_done:
    va_end(args);

    // Auto-append newline — log messages are always line-terminated.
    Serial::putchar('\n');
}

} // namespace bminos
