// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Interrupt Descriptor Table (IDT) Implementation
// ================================================

#include "idt.hpp"
#include "gdt.hpp"
#include "kernel/core/serial.hpp"
#include "kernel/core/panic.hpp"

namespace bminos::arch::x86_64 {

IdtManager g_idt;

const char* exception_name(uint8_t vector) {
    switch (vector) {
        case EXCEPTION_DIVIDE_ERROR:         return "Divide Error (#DE)";
        case EXCEPTION_DEBUG:                return "Debug Exception (#DB)";
        case EXCEPTION_NMI:                  return "NMI Interrupt";
        case EXCEPTION_BREAKPOINT:           return "Breakpoint (#BP)";
        case EXCEPTION_OVERFLOW:             return "Overflow (#OF)";
        case EXCEPTION_BOUND_RANGE:          return "BOUND Range Exceeded (#BR)";
        case EXCEPTION_INVALID_OPCODE:       return "Invalid Opcode (#UD)";
        case EXCEPTION_DEVICE_NOT_AVAILABLE: return "Device Not Available (#NM)";
        case EXCEPTION_DOUBLE_FAULT:         return "Double Fault (#DF)";
        case EXCEPTION_COPROCESSOR_OVERRUN:  return "Coprocessor Segment Overrun";
        case EXCEPTION_INVALID_TSS:          return "Invalid TSS (#TS)";
        case EXCEPTION_SEGMENT_NOT_PRESENT:  return "Segment Not Present (#NP)";
        case EXCEPTION_STACK_FAULT:          return "Stack-Segment Fault (#SS)";
        case EXCEPTION_GENERAL_PROTECTION:   return "General Protection Fault (#GP)";
        case EXCEPTION_PAGE_FAULT:           return "Page Fault (#PF)";
        case EXCEPTION_X87_FLOAT:            return "x87 FPU Floating-Point Error (#MF)";
        case EXCEPTION_ALIGNMENT_CHECK:      return "Alignment Check (#AC)";
        case EXCEPTION_MACHINE_CHECK:        return "Machine Check (#MC)";
        case EXCEPTION_SIMD_FLOAT:           return "SIMD Floating-Point Exception (#XM)";
        case EXCEPTION_VIRTUALIZATION:       return "Virtualization Exception (#VE)";
        case EXCEPTION_CONTROL_PROTECTION:   return "Control Protection Exception (#CP)";
        default:                             return "Unknown / Hardware Interrupt";
    }
}

static void default_exception_handler(InterruptFrame* frame) {
    if (!frame) return;

    LOG_FATAL("==================================================");
    LOG_FATAL("UNHANDLED CPU EXCEPTION: %s (Vector %u)",
              exception_name(static_cast<uint8_t>(frame->vector)),
              static_cast<unsigned>(frame->vector));
    LOG_FATAL("Error Code: 0x%016llX", frame->error_code);
    LOG_FATAL("RIP: 0x%016llX  CS: 0x%04llX  RFLAGS: 0x%016llX",
              frame->rip, frame->cs, frame->rflags);
    LOG_FATAL("RSP: 0x%016llX  SS: 0x%04llX", frame->rsp, frame->ss);
    LOG_FATAL("RAX: 0x%016llX  RBX: 0x%016llX  RCX: 0x%016llX",
              frame->rax, frame->rbx, frame->rcx);
    LOG_FATAL("RDX: 0x%016llX  RSI: 0x%016llX  RDI: 0x%016llX",
              frame->rdx, frame->rsi, frame->rdi);
    LOG_FATAL("RBP: 0x%016llX  R8:  0x%016llX  R9:  0x%016llX",
              frame->rbp, frame->r8, frame->r9);
    LOG_FATAL("==================================================");

    panic("Fatal CPU Exception");
}

void IdtManager::set_gate(uint8_t vector, uint64_t handler, uint8_t ist, uint8_t dpl) {
    IdtEntry& entry = m_entries[vector];
    entry.offset_low  = static_cast<uint16_t>(handler & 0xFFFF);
    entry.selector    = SEGMENT_KERNEL_CODE;
    entry.ist         = ist & 0x07;
    // Type_attributes: P=1, DPL, GateType=Interrupt (0x0E) -> 0x8E | (dpl << 5)
    entry.type_attr   = static_cast<uint8_t>(0x8E | ((dpl & 0x03) << 5));
    entry.offset_mid  = static_cast<uint16_t>((handler >> 16) & 0xFFFF);
    entry.offset_high = static_cast<uint32_t>((handler >> 32) & 0xFFFFFFFF);
    entry.reserved    = 0;
}

void IdtManager::init() {
    for (uint32_t i = 0; i < IDT_ENTRY_COUNT; ++i) {
        m_handlers[i] = nullptr;
    }

    // Default handlers for CPU exceptions (0..31)
    for (uint8_t i = 0; i < 32; ++i) {
        register_handler(i, default_exception_handler);
    }

    m_idtr.limit = sizeof(m_entries) - 1;
    m_idtr.base  = reinterpret_cast<uint64_t>(m_entries);
}

void IdtManager::register_handler(uint8_t vector, InterruptHandler handler) {
    m_handlers[vector] = handler;
}

void IdtManager::unregister_handler(uint8_t vector) {
    if (vector < 32) {
        m_handlers[vector] = default_exception_handler;
    } else {
        m_handlers[vector] = nullptr;
    }
}

InterruptHandler IdtManager::get_handler(uint8_t vector) const {
    return m_handlers[vector];
}

void IdtManager::load() {
    m_idtr.limit = sizeof(m_entries) - 1;
    m_idtr.base  = reinterpret_cast<uint64_t>(m_entries);

#if defined(__x86_64__)
    asm volatile("lidt %0" : : "m"(m_idtr) : "memory");
#endif
}

void IdtManager::set_ist(uint8_t vector, uint8_t ist) {
    m_entries[vector].ist = ist & 0x07;
}

void IdtManager::dispatch(InterruptFrame* frame) {
    if (!frame) return;

    uint8_t vector = static_cast<uint8_t>(frame->vector);
    InterruptHandler handler = g_idt.get_handler(vector);
    if (handler) {
        handler(frame);
    } else {
        LOG_WARN("Spurious or unhandled interrupt vector: %u", vector);
    }
}

void IdtManager::set_entry(uint8_t vector, const IdtEntry& entry) {
    m_entries[vector] = entry;
}

const IdtEntry& IdtManager::get_entry(uint8_t vector) const {
    return m_entries[vector];
}

void init_idt() {
    g_idt.init();
    g_idt.load();
}

void register_interrupt_handler(uint8_t vector, InterruptHandler handler) {
    g_idt.register_handler(vector, handler);
}

void load_idt() {
    g_idt.load();
}

} // namespace bminos::arch::x86_64

