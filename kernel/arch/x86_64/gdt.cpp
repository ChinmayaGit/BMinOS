// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Global Descriptor Table (GDT) Implementation
// =============================================

#include "gdt.hpp"

namespace bminos::arch::x86_64 {

static Gdt s_gdt;
static Tss s_tss;
static GdtPointer s_gdtr;

Gdt& get_gdt() {
    return s_gdt;
}

Tss& get_tss() {
    return s_tss;
}

void load_gdt(const GdtPointer& gdtr) {
#if defined(__x86_64__)
    asm volatile("lgdt %0" : : "m"(gdtr) : "memory");
#endif
}

void reload_segments() {
#if defined(__x86_64__)
    asm volatile(
        "mov %0, %%ds\n"
        "mov %0, %%es\n"
        "mov %0, %%ss\n"
        "xor %%eax, %%eax\n"
        "mov %%eax, %%fs\n"
        "mov %%eax, %%gs\n"
        // Far return to reload CS
        "pushq %1\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        :
        : "r"(SEGMENT_KERNEL_DATA), "i"(SEGMENT_KERNEL_CODE)
        : "rax", "memory"
    );
#endif
}

void load_tr(uint16_t selector) {
#if defined(__x86_64__)
    asm volatile("ltr %0" : : "r"(selector));
#endif
}

void install_tss(Tss* tss) {
    if (!tss) return;
    s_gdt.tss_descriptor() = TssDescriptor::from_tss(tss, 0);

    // Reload GDTR to reflect the TSS descriptor
    s_gdtr.limit = sizeof(s_gdt.entries) - 1;
    s_gdtr.base  = reinterpret_cast<uint64_t>(&s_gdt);
    load_gdt(s_gdtr);

    load_tr(SEGMENT_TSS);
}

void init_gdt() {
    s_gdt.init();

    s_gdtr.limit = sizeof(s_gdt.entries) - 1;
    s_gdtr.base  = reinterpret_cast<uint64_t>(&s_gdt);

    load_gdt(s_gdtr);
    reload_segments();
}

} // namespace bminos::arch::x86_64

