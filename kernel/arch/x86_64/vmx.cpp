// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Intel VT-x (VMX) Virtualization Implementation
// ===============================================

#include "vmx.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/core/serial.hpp"
#include "kernel/core/panic.hpp"

namespace bminos::arch::x86_64 {

VmxManager g_vmx;

// =============================================================================
// Low-Level VMX Instruction Wrappers
// =============================================================================

bool vmx_vmread(uint64_t field, uint64_t& value) {
#if defined(__x86_64__)
    uint8_t error = 0;
    asm volatile(
        "vmread %1, %0\n"
        "setc %2\n"
        : "=r"(value)
        : "r"(field), "r"(error)
        : "cc"
    );
    return error == 0;
#else
    (void)field; (void)value; return false;
#endif
}

bool vmx_vmwrite(uint64_t field, uint64_t value) {
#if defined(__x86_64__)
    uint8_t error = 0;
    asm volatile(
        "vmwrite %1, %0\n"
        "setc %2\n"
        :
        : "r"(field), "r"(value), "r"(error)
        : "cc"
    );
    return error == 0;
#else
    (void)field; (void)value; return false;
#endif
}

bool vmx_vmptrld(uint64_t vmcs_phys) {
#if defined(__x86_64__)
    uint8_t error = 0;
    asm volatile(
        "vmptrld %1\n"
        "setc %0\n"
        : "=r"(error)
        : "m"(vmcs_phys)
        : "cc"
    );
    return error == 0;
#else
    (void)vmcs_phys; return false;
#endif
}

bool vmx_vmclear(uint64_t vmcs_phys) {
#if defined(__x86_64__)
    uint8_t error = 0;
    asm volatile(
        "vmclear %1\n"
        "setc %0\n"
        : "=r"(error)
        : "m"(vmcs_phys)
        : "cc"
    );
    return error == 0;
#else
    (void)vmcs_phys; return false;
#endif
}

bool vmx_vmlaunch() {
#if defined(__x86_64__)
    uint8_t error = 0;
    asm volatile(
        "vmlaunch\n"
        "setc %0\n"
        : "=r"(error)
        :
        : "cc"
    );
    return error == 0;
#else
    return false;
#endif
}

bool vmx_vmresume() {
#if defined(__x86_64__)
    uint8_t error = 0;
    asm volatile(
        "vmresume\n"
        "setc %0\n"
        : "=r"(error)
        :
        : "cc"
    );
    return error == 0;
#else
    return false;
#endif
}

bool vmx_invept(uint64_t type, uint64_t eptp) {
#if defined(__x86_64__)
    struct {
        uint64_t eptp;
        uint64_t reserved;
    } descriptor = {eptp, 0};

    uint8_t error = 0;
    asm volatile(
        "invept %1, %2\n"
        "setc %0\n"
        : "=r"(error)
        : "m"(descriptor), "r"(type)
        : "cc"
    );
    return error == 0;
#else
    (void)type; (void)eptp; return false;
#endif
}

bool vmx_invvpid(uint64_t type, uint16_t vpid, uint64_t linear_address) {
#if defined(__x86_64__)
    struct {
        uint64_t vpid : 16;
        uint64_t reserved : 48;
        uint64_t linear_address;
    } descriptor = {vpid, 0, linear_address};

    uint8_t error = 0;
    asm volatile(
        "invvpid %1, %2\n"
        "setc %0\n"
        : "=r"(error)
        : "m"(descriptor), "r"(type)
        : "cc"
    );
    return error == 0;
#else
    (void)type; (void)vpid; (void)linear_address; return false;
#endif
}

// =============================================================================
// VMCS Wrapper Methods
// =============================================================================

bool Vmcs::allocate(uint32_t revision_id) {
    if (m_phys_addr) free();

    m_phys_addr = mm::g_pmm.alloc_page();
    if (!m_phys_addr) return false;

    m_virt_addr = reinterpret_cast<void*>(mm::VirtualMemoryManager::phys_to_virt(m_phys_addr));

    // Zero out the 4KB region
    auto* ptr = static_cast<uint8_t*>(m_virt_addr);
    for (size_t i = 0; i < mm::PAGE_SIZE; ++i) {
        ptr[i] = 0;
    }

    // First 4 bytes: revision ID (with shadow VMCS bit 31 cleared)
    *reinterpret_cast<uint32_t*>(m_virt_addr) = revision_id & 0x7FFFFFFFU;

    return clear();
}

void Vmcs::free() {
    if (m_phys_addr) {
        clear();
        mm::g_pmm.free_page(m_phys_addr);
        m_phys_addr = 0;
        m_virt_addr = nullptr;
    }
}

bool Vmcs::load() {
    if (!m_phys_addr) return false;
    return vmx_vmptrld(m_phys_addr);
}

bool Vmcs::clear() {
    if (!m_phys_addr) return false;
    return vmx_vmclear(m_phys_addr);
}

bool Vmcs::read(VmcsField field, uint64_t& value) const {
    return vmx_vmread(static_cast<uint64_t>(field), value);
}

bool Vmcs::write(VmcsField field, uint64_t value) {
    return vmx_vmwrite(static_cast<uint64_t>(field), value);
}

// =============================================================================
// VmxManager Helpers
// =============================================================================

uint64_t VmxManager::rdmsr(uint32_t msr) {
#if defined(__x86_64__)
    uint32_t low, high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
#else
    (void)msr; return 0;
#endif
}

void VmxManager::wrmsr(uint32_t msr, uint64_t value) {
#if defined(__x86_64__)
    uint32_t low = static_cast<uint32_t>(value);
    uint32_t high = static_cast<uint32_t>(value >> 32);
    asm volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr));
#else
    (void)msr; (void)value;
#endif
}

uint64_t VmxManager::read_cr0() {
    uint64_t val = 0;
#if defined(__x86_64__)
    asm volatile("mov %%cr0, %0" : "=r"(val));
#endif
    return val;
}

uint64_t VmxManager::read_cr4() {
    uint64_t val = 0;
#if defined(__x86_64__)
    asm volatile("mov %%cr4, %0" : "=r"(val));
#endif
    return val;
}

void VmxManager::write_cr4(uint64_t value) {
#if defined(__x86_64__)
    asm volatile("mov %0, %%cr4" : : "r"(value));
#else
    (void)value;
#endif
}

bool VmxManager::execute_vmxon(uint64_t vmxon_region_phys) {
#if defined(__x86_64__)
    uint8_t error = 0;
    asm volatile(
        "vmxon %1\n"
        "setc %0\n"
        : "=r"(error)
        : "m"(vmxon_region_phys)
        : "cc"
    );
    return error == 0;
#else
    (void)vmxon_region_phys; return false;
#endif
}

void VmxManager::execute_vmxoff() {
#if defined(__x86_64__)
    asm volatile("vmxoff" : : : "cc");
#endif
}

// =============================================================================
// VmxManager Lifecycle
// =============================================================================

bool VmxManager::detect_vmx_support() {
#if defined(__x86_64__)
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));

    // ECX bit 5: VMX feature flag
    if (!(ecx & (1U << 5))) {
        LOG_WARN("CPU does not support Intel VT-x (VMX)");
        return false;
    }

    // Check IA32_FEATURE_CONTROL MSR
    uint64_t feature_ctrl = rdmsr(static_cast<uint32_t>(VmxMsr::IA32_FEATURE_CONTROL));
    if (!(feature_ctrl & 1)) {
        // Lock bit not set; enable VMX outside SMX and set lock bit
        feature_ctrl |= (1ULL << 0) | (1ULL << 2);
        wrmsr(static_cast<uint32_t>(VmxMsr::IA32_FEATURE_CONTROL), feature_ctrl);
    } else if (!(feature_ctrl & (1ULL << 2))) {
        LOG_ERROR("VMX disabled in BIOS/firmware");
        return false;
    }

    return true;
#else
    return false;
#endif
}

void VmxManager::read_capabilities() {
    uint64_t basic = rdmsr(static_cast<uint32_t>(VmxMsr::IA32_VMX_BASIC));
    m_caps.vmcs_revision_id = static_cast<uint32_t>(basic & 0x7FFFFFFF);
    m_caps.vmcs_size = static_cast<uint16_t>((basic >> 32) & 0x1FFF);
    m_caps.has_true_controls = (basic & (1ULL << 55)) != 0;

    uint64_t pin_msr = m_caps.has_true_controls ?
        static_cast<uint32_t>(VmxMsr::IA32_VMX_TRUE_PINBASED_CTLS) :
        static_cast<uint32_t>(VmxMsr::IA32_VMX_PINBASED_CTLS);
    uint64_t pin_val = rdmsr(static_cast<uint32_t>(pin_msr));
    m_caps.pin_ctls_allowed0 = static_cast<uint32_t>(pin_val);
    m_caps.pin_ctls_allowed1 = static_cast<uint32_t>(pin_val >> 32);

    uint64_t proc_msr = m_caps.has_true_controls ?
        static_cast<uint32_t>(VmxMsr::IA32_VMX_TRUE_PROCBASED_CTLS) :
        static_cast<uint32_t>(VmxMsr::IA32_VMX_PROCBASED_CTLS);
    uint64_t proc_val = rdmsr(static_cast<uint32_t>(proc_msr));
    m_caps.proc_ctls_allowed0 = static_cast<uint32_t>(proc_val);
    m_caps.proc_ctls_allowed1 = static_cast<uint32_t>(proc_val >> 32);

    uint64_t ept_vpid = rdmsr(static_cast<uint32_t>(VmxMsr::IA32_VMX_EPT_VPID_CAP));
    m_caps.has_ept = (ept_vpid & (1ULL << 0)) != 0;
    m_caps.has_vpid = (ept_vpid & (1ULL << 32)) != 0;
    m_caps.has_unrestricted_guest = (m_caps.proc_ctls2_allowed1 & (1ULL << 7)) != 0;
}

uint32_t VmxManager::adjust_control_value(uint32_t desired, uint32_t allowed0, uint32_t allowed1) {
    desired |= allowed0;   // Bits that MUST be 1
    desired &= allowed1;   // Bits that CAN be 1
    return desired;
}

bool VmxManager::enable_vmx() {
    if (!detect_vmx_support()) return false;
    read_capabilities();

    // Allocate 4KB page for VMXON
    m_vmxon_region_phys = mm::g_pmm.alloc_page();
    if (!m_vmxon_region_phys) return false;

    m_vmxon_region_virt = reinterpret_cast<void*>(mm::VirtualMemoryManager::phys_to_virt(m_vmxon_region_phys));
    *reinterpret_cast<uint32_t*>(m_vmxon_region_virt) = m_caps.vmcs_revision_id;

    // Enable CR4.VMXE (bit 13)
    uint64_t cr4 = read_cr4();
    write_cr4(cr4 | (1ULL << 13));

    // Ensure CR0/CR4 comply with fixed bits
    uint64_t cr0 = read_cr0();
    cr0 |= rdmsr(static_cast<uint32_t>(VmxMsr::IA32_VMX_CR0_FIXED0));
    cr0 &= rdmsr(static_cast<uint32_t>(VmxMsr::IA32_VMX_CR0_FIXED1));
#if defined(__x86_64__)
    asm volatile("mov %0, %%cr0" : : "r"(cr0));
#endif

    if (!execute_vmxon(m_vmxon_region_phys)) {
        LOG_ERROR("VMXON execution failed");
        return false;
    }

    m_vmx_enabled = true;
    LOG_INFO("Intel VT-x (VMX) enabled successfully");
    return true;
}

void VmxManager::disable_vmx() {
    if (m_vmx_enabled) {
        execute_vmxoff();
        uint64_t cr4 = read_cr4();
        write_cr4(cr4 & ~(1ULL << 13));
        m_vmx_enabled = false;
    }

    if (m_vmxon_region_phys) {
        mm::g_pmm.free_page(m_vmxon_region_phys);
        m_vmxon_region_phys = 0;
        m_vmxon_region_virt = nullptr;
    }
}

// =============================================================================
// VM-Exit Dispatcher & Handlers
// =============================================================================

void VmxManager::handle_cpuid_exit(GuestRegisters& regs) {
    uint32_t eax = static_cast<uint32_t>(regs.rax);
    uint32_t ecx = static_cast<uint32_t>(regs.rcx);

    uint32_t out_eax = 0, out_ebx = 0, out_ecx = 0, out_edx = 0;
#if defined(__x86_64__)
    asm volatile("cpuid"
        : "=a"(out_eax), "=b"(out_ebx), "=c"(out_ecx), "=d"(out_edx)
        : "a"(eax), "c"(ecx));
#endif

    // Hide hypervisor presence or identify as NEXUS
    if (eax == 0x40000000) {
        out_eax = 0x40000001;
        out_ebx = 0x5558454E; // "NEXU"
        out_ecx = 0x534F535F; // "_SOS"
        out_edx = 0x30303121; // "!100"
    }

    regs.rax = out_eax;
    regs.rbx = out_ebx;
    regs.rcx = out_ecx;
    regs.rdx = out_edx;

    // Advance guest RIP past CPUID (2 bytes)
    uint64_t rip = 0, inst_len = 0;
    vmx_vmread(static_cast<uint64_t>(VmcsField::GUEST_RIP), rip);
    vmx_vmread(static_cast<uint64_t>(VmcsField::VM_EXIT_INSTRUCTION_LEN), inst_len);
    vmx_vmwrite(static_cast<uint64_t>(VmcsField::GUEST_RIP), rip + (inst_len ? inst_len : 2));
}

void VmxManager::handle_io_exit(GuestRegisters&) {
    uint64_t rip = 0, inst_len = 0;
    vmx_vmread(static_cast<uint64_t>(VmcsField::GUEST_RIP), rip);
    vmx_vmread(static_cast<uint64_t>(VmcsField::VM_EXIT_INSTRUCTION_LEN), inst_len);
    vmx_vmwrite(static_cast<uint64_t>(VmcsField::GUEST_RIP), rip + (inst_len ? inst_len : 1));
}

void VmxManager::handle_msr_read_exit(GuestRegisters& regs) {
    uint32_t msr = static_cast<uint32_t>(regs.rcx);
    uint64_t val = rdmsr(msr);
    regs.rax = static_cast<uint32_t>(val);
    regs.rdx = static_cast<uint32_t>(val >> 32);

    uint64_t rip = 0, inst_len = 0;
    vmx_vmread(static_cast<uint64_t>(VmcsField::GUEST_RIP), rip);
    vmx_vmread(static_cast<uint64_t>(VmcsField::VM_EXIT_INSTRUCTION_LEN), inst_len);
    vmx_vmwrite(static_cast<uint64_t>(VmcsField::GUEST_RIP), rip + (inst_len ? inst_len : 2));
}

void VmxManager::handle_msr_write_exit(GuestRegisters& regs) {
    uint32_t msr = static_cast<uint32_t>(regs.rcx);
    uint64_t val = (regs.rdx << 32) | (regs.rax & 0xFFFFFFFF);
    wrmsr(msr, val);

    uint64_t rip = 0, inst_len = 0;
    vmx_vmread(static_cast<uint64_t>(VmcsField::GUEST_RIP), rip);
    vmx_vmread(static_cast<uint64_t>(VmcsField::VM_EXIT_INSTRUCTION_LEN), inst_len);
    vmx_vmwrite(static_cast<uint64_t>(VmcsField::GUEST_RIP), rip + (inst_len ? inst_len : 2));
}

void VmxManager::handle_ept_violation(GuestRegisters&) {
    uint64_t gpa = 0;
    vmx_vmread(static_cast<uint64_t>(VmcsField::GUEST_PHYSICAL_ADDRESS), gpa);
    LOG_WARN("EPT Violation at Guest Physical Address: 0x%016llX", gpa);
}

void VmxManager::handle_vmcall_exit(GuestRegisters& regs) {
    // Hypercall interface
    uint64_t call_nr = regs.rax;
    LOG_INFO("VMCALL received: %llu", call_nr);
    regs.rax = 0; // Status::Ok

    uint64_t rip = 0, inst_len = 0;
    vmx_vmread(static_cast<uint64_t>(VmcsField::GUEST_RIP), rip);
    vmx_vmread(static_cast<uint64_t>(VmcsField::VM_EXIT_INSTRUCTION_LEN), inst_len);
    vmx_vmwrite(static_cast<uint64_t>(VmcsField::GUEST_RIP), rip + (inst_len ? inst_len : 3));
}

void VmxManager::handle_cr_access_exit(GuestRegisters&) {
    uint64_t rip = 0, inst_len = 0;
    vmx_vmread(static_cast<uint64_t>(VmcsField::GUEST_RIP), rip);
    vmx_vmread(static_cast<uint64_t>(VmcsField::VM_EXIT_INSTRUCTION_LEN), inst_len);
    vmx_vmwrite(static_cast<uint64_t>(VmcsField::GUEST_RIP), rip + (inst_len ? inst_len : 3));
}

void VmxManager::handle_exception_exit(GuestRegisters&) {
    LOG_WARN("Guest exception intercepted");
}

void VmxManager::handle_hlt_exit(GuestRegisters&) {
    uint64_t rip = 0, inst_len = 0;
    vmx_vmread(static_cast<uint64_t>(VmcsField::GUEST_RIP), rip);
    vmx_vmread(static_cast<uint64_t>(VmcsField::VM_EXIT_INSTRUCTION_LEN), inst_len);
    vmx_vmwrite(static_cast<uint64_t>(VmcsField::GUEST_RIP), rip + (inst_len ? inst_len : 1));
}

void VmxManager::handle_triple_fault(GuestRegisters&) {
    LOG_FATAL("Guest Triple Fault!");
}

bool VmxManager::handle_vm_exit(GuestRegisters& regs) {
    uint64_t exit_reason_raw = 0;
    vmx_vmread(static_cast<uint64_t>(VmcsField::VM_EXIT_REASON), exit_reason_raw);
    auto reason = static_cast<VmExitReason>(exit_reason_raw & 0xFFFF);

    switch (reason) {
        case VmExitReason::CPUID:
            handle_cpuid_exit(regs);
            break;
        case VmExitReason::IO_INSTRUCTION:
            handle_io_exit(regs);
            break;
        case VmExitReason::RDMSR:
            handle_msr_read_exit(regs);
            break;
        case VmExitReason::WRMSR:
            handle_msr_write_exit(regs);
            break;
        case VmExitReason::EPT_VIOLATION:
            handle_ept_violation(regs);
            break;
        case VmExitReason::VMCALL:
            handle_vmcall_exit(regs);
            break;
        case VmExitReason::CR_ACCESS:
            handle_cr_access_exit(regs);
            break;
        case VmExitReason::EXCEPTION_OR_NMI:
            handle_exception_exit(regs);
            break;
        case VmExitReason::HLT:
            handle_hlt_exit(regs);
            break;
        case VmExitReason::TRIPLE_FAULT:
            handle_triple_fault(regs);
            return false;
        default:
            LOG_WARN("Unhandled VM-exit reason: 0x%04X", static_cast<uint32_t>(reason));
            break;
    }

    return true;
}

} // namespace bminos::arch::x86_64

