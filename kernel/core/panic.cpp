// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// kernel/core/panic.cpp — Kernel panic handler implementation.
// This file handles the catastrophic failure path. When we get here,
// something has gone irrecoverably wrong. Our only goal is to dump as
// much diagnostic info as possible before halting the machine.

#include "panic.hpp"
#include "serial.hpp"

// va_list support for panic_fmt
using va_list = __builtin_va_list;
#define va_start(ap, param) __builtin_va_start(ap, param)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)

namespace bminos {

// ============================================================================
// CPU Halt
// ============================================================================

namespace {

/// Disable interrupts and halt the current CPU. This is the terminal state
/// after a panic — we loop on HLT in case of spurious wakeups (NMIs on
/// x86_64 can wake us from HLT).
BMINOS_NORETURN void halt_cpu() {
#if BMINOS_ARCH_X86_64
    asm volatile(
        "cli\n"           // Disable interrupts
        "1:\n"
        "hlt\n"           // Halt the CPU
        "jmp 1b\n"        // Loop in case of NMI wakeup
        ::: "memory"
    );
#elif BMINOS_ARCH_ARM64
    asm volatile(
        "msr daifset, #0xF\n"  // Mask all exceptions (Debug, SError, IRQ, FIQ)
        "1:\n"
        "wfi\n"                 // Wait for interrupt (will never come)
        "b 1b\n"               // Loop just in case
        ::: "memory"
    );
#endif
    __builtin_unreachable();
}

} // anonymous namespace

// ============================================================================
// Register Capture
// ============================================================================

void capture_registers(RegisterState& state) {
#if BMINOS_ARCH_X86_64
    // Capture all general-purpose and control registers.
    // We use individual mov instructions rather than pushad (which doesn't
    // exist in 64-bit mode) to get each register precisely.
    asm volatile(
        "mov %%rax, %0\n"
        "mov %%rbx, %1\n"
        "mov %%rcx, %2\n"
        "mov %%rdx, %3\n"
        : "=m"(state.rax), "=m"(state.rbx), "=m"(state.rcx), "=m"(state.rdx)
    );
    asm volatile(
        "mov %%rsi, %0\n"
        "mov %%rdi, %1\n"
        "mov %%rbp, %2\n"
        "mov %%rsp, %3\n"
        : "=m"(state.rsi), "=m"(state.rdi), "=m"(state.rbp), "=m"(state.rsp)
    );
    asm volatile(
        "mov %%r8,  %0\n"
        "mov %%r9,  %1\n"
        "mov %%r10, %2\n"
        "mov %%r11, %3\n"
        : "=m"(state.r8), "=m"(state.r9), "=m"(state.r10), "=m"(state.r11)
    );
    asm volatile(
        "mov %%r12, %0\n"
        "mov %%r13, %1\n"
        "mov %%r14, %2\n"
        "mov %%r15, %3\n"
        : "=m"(state.r12), "=m"(state.r13), "=m"(state.r14), "=m"(state.r15)
    );

    // RIP: Use the return address of this function as an approximation.
    // The actual RIP at the panic site is more useful, but we'd need
    // the caller to pass it.
    u64 rip_val;
    asm volatile("lea (%%rip), %0" : "=r"(rip_val));
    state.rip = rip_val;

    // RFLAGS
    u64 rflags;
    asm volatile("pushfq; pop %0" : "=r"(rflags));
    state.rflags = rflags;

    // Segment registers
    u16 seg;
    asm volatile("mov %%cs, %0" : "=r"(seg)); state.cs = seg;
    asm volatile("mov %%ds, %0" : "=r"(seg)); state.ds = seg;
    asm volatile("mov %%es, %0" : "=r"(seg)); state.es = seg;
    asm volatile("mov %%fs, %0" : "=r"(seg)); state.fs = seg;
    asm volatile("mov %%gs, %0" : "=r"(seg)); state.gs = seg;
    asm volatile("mov %%ss, %0" : "=r"(seg)); state.ss = seg;

    // Control registers — these tell us about paging configuration,
    // fault addresses, and virtualization state.
    asm volatile("mov %%cr0, %0" : "=r"(state.cr0));
    asm volatile("mov %%cr2, %0" : "=r"(state.cr2));  // Last page fault address
    asm volatile("mov %%cr3, %0" : "=r"(state.cr3));  // Page table root
    asm volatile("mov %%cr4, %0" : "=r"(state.cr4));

#elif BMINOS_ARCH_ARM64
    // Capture general-purpose registers x0-x30.
    // We save them one at a time since there's no bulk save instruction
    // that writes to arbitrary memory (STP writes two at a time).
    asm volatile(
        "stp x0,  x1,  [%0, #0]\n"
        "stp x2,  x3,  [%0, #16]\n"
        "stp x4,  x5,  [%0, #32]\n"
        "stp x6,  x7,  [%0, #48]\n"
        "stp x8,  x9,  [%0, #64]\n"
        "stp x10, x11, [%0, #80]\n"
        "stp x12, x13, [%0, #96]\n"
        "stp x14, x15, [%0, #112]\n"
        "stp x16, x17, [%0, #128]\n"
        "stp x18, x19, [%0, #144]\n"
        "stp x20, x21, [%0, #160]\n"
        "stp x22, x23, [%0, #176]\n"
        "stp x24, x25, [%0, #192]\n"
        "stp x26, x27, [%0, #208]\n"
        "stp x28, x29, [%0, #224]\n"
        "str x30,      [%0, #240]\n"
        :
        : "r"(&state.x[0])
        : "memory"
    );

    // System registers
    asm volatile("mov %0, sp" : "=r"(state.sp));
    asm volatile("mrs %0, elr_el1" : "=r"(state.elr));
    asm volatile("mrs %0, spsr_el1" : "=r"(state.spsr));
    asm volatile("mrs %0, esr_el1" : "=r"(state.esr));
    asm volatile("mrs %0, far_el1" : "=r"(state.far));
#endif
}

// ============================================================================
// Register Dump
// ============================================================================

void dump_registers(const RegisterState& state) {
    kprintf("=== CPU Register Dump ===\n");

#if BMINOS_ARCH_X86_64
    kprintf("  RAX=%lx  RBX=%lx  RCX=%lx  RDX=%lx\n",
            state.rax, state.rbx, state.rcx, state.rdx);
    kprintf("  RSI=%lx  RDI=%lx  RBP=%lx  RSP=%lx\n",
            state.rsi, state.rdi, state.rbp, state.rsp);
    kprintf("  R8 =%lx  R9 =%lx  R10=%lx  R11=%lx\n",
            state.r8, state.r9, state.r10, state.r11);
    kprintf("  R12=%lx  R13=%lx  R14=%lx  R15=%lx\n",
            state.r12, state.r13, state.r14, state.r15);
    kprintf("  RIP=%lx  RFLAGS=%lx\n", state.rip, state.rflags);
    kprintf("  CS=%x  DS=%x  ES=%x  FS=%x  GS=%x  SS=%x\n",
            state.cs, state.ds, state.es, state.fs, state.gs, state.ss);
    kprintf("  CR0=%lx  CR2=%lx  CR3=%lx  CR4=%lx\n",
            state.cr0, state.cr2, state.cr3, state.cr4);

#elif BMINOS_ARCH_ARM64
    for (int i = 0; i < 31; i += 4) {
        kprintf("  X%-2d=%lx  X%-2d=%lx  X%-2d=%lx  X%-2d=%lx\n",
                i, state.x[i], i+1, state.x[i+1],
                i+2, state.x[i+2], i+3, (i+3 < 31) ? state.x[i+3] : 0ULL);
    }
    kprintf("  SP  =%lx  ELR=%lx\n", state.sp, state.elr);
    kprintf("  SPSR=%lx  ESR=%lx  FAR=%lx\n",
            state.spsr, state.esr, state.far);
#endif

    kprintf("=========================\n");
}

// ============================================================================
// Stack Trace
// ============================================================================

usize dump_stack_trace() {
    u64 frame_pointer;

#if BMINOS_ARCH_X86_64
    asm volatile("mov %%rbp, %0" : "=r"(frame_pointer));
#elif BMINOS_ARCH_ARM64
    asm volatile("mov %0, x29" : "=r"(frame_pointer));
#endif

    return dump_stack_trace_from(frame_pointer);
}

usize dump_stack_trace_from(u64 frame_pointer) {
    kprintf("=== Stack Trace ===\n");

    usize frame_count = 0;

    // Walk the frame pointer chain.
    // Each stack frame (with frame pointers enabled) looks like:
    //
    //   x86_64:                      ARM64:
    //   [RBP+8] = return address     [FP+8] = return address (LR)
    //   [RBP]   = previous RBP       [FP]   = previous FP
    //
    // We follow the chain until we hit a null frame pointer (top of stack),
    // reach our maximum depth, or encounter an obviously invalid address.

    while (frame_pointer != 0 && frame_count < MAX_STACK_FRAMES) {
        // Basic sanity check: frame pointer should be in a reasonable
        // range and be properly aligned.
        if (frame_pointer < 0x1000) {
            // Likely a null or near-null pointer — stop walking.
            break;
        }

        // On both x86_64 and ARM64, the frame layout is:
        //   [frame_pointer + 0]           = saved frame pointer (previous frame)
        //   [frame_pointer + sizeof(u64)] = return address
        u64* frame = reinterpret_cast<u64*>(frame_pointer);

        u64 return_addr = frame[1];
        u64 prev_frame  = frame[0];

        kprintf("  #%lu: 0x%lx\n", static_cast<u64>(frame_count), return_addr);

        // Sanity: the previous frame should be higher on the stack
        // (stacks grow downward on both architectures).
        if (prev_frame <= frame_pointer && prev_frame != 0) {
            kprintf("  (frame chain corrupted at frame #%lu)\n",
                    static_cast<u64>(frame_count));
            break;
        }

        frame_pointer = prev_frame;
        frame_count++;
    }

    if (frame_count == 0) {
        kprintf("  (no frames — frame pointer may be missing)\n");
        kprintf("  Ensure compilation uses -fno-omit-frame-pointer\n");
    }

    kprintf("===================\n");
    return frame_count;
}

// ============================================================================
// Panic Implementation
// ============================================================================

BMINOS_NORETURN void panic(const char* message) {
    // Disable interrupts immediately to prevent re-entrant panics
    // from interrupt handlers.
#if BMINOS_ARCH_X86_64
    asm volatile("cli" ::: "memory");
#elif BMINOS_ARCH_ARM64
    asm volatile("msr daifset, #0xF" ::: "memory");
#endif

    kprintf("\n");
    kprintf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    kprintf("!!                    KERNEL PANIC                         !!\n");
    kprintf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    kprintf("\n");
    kprintf("  %s\n", message);
    kprintf("\n");

    // Dump registers
    RegisterState regs;
    capture_registers(regs);
    dump_registers(regs);

    // Dump stack trace
    dump_stack_trace();

    kprintf("\n--- System halted ---\n");

    // TODO: Send IPI to halt other CPUs (when SMP is implemented).
    // For now, we just halt the current CPU.
    halt_cpu();
}

BMINOS_NORETURN void panic_at(const char* file, u32 line, const char* message) {
#if BMINOS_ARCH_X86_64
    asm volatile("cli" ::: "memory");
#elif BMINOS_ARCH_ARM64
    asm volatile("msr daifset, #0xF" ::: "memory");
#endif

    kprintf("\n");
    kprintf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    kprintf("!!                    KERNEL PANIC                         !!\n");
    kprintf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    kprintf("\n");
    kprintf("  %s:%u: %s\n", file, line, message);
    kprintf("\n");

    RegisterState regs;
    capture_registers(regs);
    dump_registers(regs);

    dump_stack_trace();

    kprintf("\n--- System halted ---\n");
    halt_cpu();
}

BMINOS_NORETURN void panic_fmt(const char* fmt, ...) {
#if BMINOS_ARCH_X86_64
    asm volatile("cli" ::: "memory");
#elif BMINOS_ARCH_ARM64
    asm volatile("msr daifset, #0xF" ::: "memory");
#endif

    kprintf("\n");
    kprintf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    kprintf("!!                    KERNEL PANIC                         !!\n");
    kprintf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    kprintf("\n  ");

    // Manual format printing since we can't forward va_args to kprintf easily
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
        if (*fmt == 'l') { is_long = true; fmt++; if (*fmt == 'l') fmt++; }

        switch (*fmt) {
            case '\0': goto panic_fmt_done;
            case '%': Serial::putchar('%'); break;
            case 's': Serial::puts(va_arg(args, const char*)); break;
            case 'c': Serial::putchar(static_cast<char>(va_arg(args, int))); break;
            case 'd': case 'i': {
                i64 v = is_long ? va_arg(args, i64) : va_arg(args, int);
                // Simple inline conversion
                if (v < 0) { Serial::putchar('-'); v = -v; }
                char buf[21]; int pos = 0;
                do { buf[pos++] = '0' + (v % 10); v /= 10; } while (v);
                while (pos > 0) Serial::putchar(buf[--pos]);
                break;
            }
            case 'u': case 'x': case 'X': {
                u64 v = is_long ? va_arg(args, u64) : va_arg(args, unsigned int);
                int base = (*fmt == 'u') ? 10 : 16;
                bool upper = (*fmt == 'X');
                // Use the helper from anonymous namespace in serial.cpp
                // Since we're in a different TU, reimplement inline
                const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
                char buf2[65]; int pos2 = 0;
                if (v == 0) { buf2[pos2++] = '0'; }
                else { while (v) { buf2[pos2++] = digits[v % base]; v /= base; } }
                while (pos2 > 0) Serial::putchar(buf2[--pos2]);
                break;
            }
            case 'p': {
                Serial::puts("0x");
                u64 v = reinterpret_cast<u64>(va_arg(args, void*));
                const char* digits = "0123456789abcdef";
                char buf3[17]; int pos3 = 0;
                if (v == 0) { buf3[pos3++] = '0'; }
                else { while (v) { buf3[pos3++] = digits[v % 16]; v /= 16; } }
                while (pos3 > 0) Serial::putchar(buf3[--pos3]);
                break;
            }
            default: Serial::putchar(*fmt); break;
        }
        fmt++;
    }
panic_fmt_done:
    va_end(args);

    kprintf("\n\n");

    RegisterState regs;
    capture_registers(regs);
    dump_registers(regs);

    dump_stack_trace();

    kprintf("\n--- System halted ---\n");
    halt_cpu();
}

} // namespace bminos
