// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Virtual CPU (vCPU) Implementation
// ==================================

#include "vcpu.hpp"
#include "kernel/arch/x86_64/vmx.hpp"
#include "kernel/core/serial.hpp"

namespace bminos {

Result<void> VCPU::initialize(VcpuId vcpu_id, DomainId domain_id, u32 vcpu_index) {
    id_ = vcpu_id;
    domain_id_ = domain_id;
    vcpu_index_ = vcpu_index;
    state_ = VcpuState::Ready;
    pinned_pcpu_ = -1;
    last_pcpu_ = 0;
    cpu_time_ns_ = 0;
    pending_interrupt_valid_ = false;
    pending_interrupt_vector_ = 0;

    // Reset GPRs
    gprs_ = {};
    // Set standard reset instruction pointer (e.g. 0xFFF0 for real mode / BIOS or entry point)
    gprs_.rip = 0xFFF0;
    gprs_.rflags = 0x0002;

    return Result<void>::ok();
}

void VCPU::inject_interrupt(u32 vector) {
    pending_interrupt_valid_ = true;
    pending_interrupt_vector_ = vector;
}

void VCPU::save_context() {
    // In full implementation: read remaining dirty registers from VMCS
}

void VCPU::restore_context() {
    // If there is a pending interrupt, inject via VMCS VM-entry controls
    if (pending_interrupt_valid_) {
#if defined(__x86_64__)
        // Hardware interrupt injection (Type 0 = ExtInt, bit 31 = valid)
        u32 intr_info = (pending_interrupt_vector_ & 0xFF) | (1U << 31);
        arch::x86_64::vmx_vmwrite(0x00004016 /* VM_ENTRY_INTR_INFO_FIELD */, intr_info);
#endif
        pending_interrupt_valid_ = false;
    }
}

VmExitInfo VCPU::run() {
    state_ = VcpuState::Running;
    restore_context();

    VmExitInfo info{};

#if defined(__x86_64__)
    // Enter guest execution via VMRESUME / VMLAUNCH
    bool resume_ok = arch::x86_64::vmx_vmresume();
    if (!resume_ok) {
        // Fall back to vmlaunch
        arch::x86_64::vmx_vmlaunch();
    }

    // When guest exits back to hypervisor:
    save_context();

    uint64_t exit_reason_raw = 0;
    arch::x86_64::vmx_vmread(0x00004402 /* VM_EXIT_REASON */, exit_reason_raw);
    uint64_t qual = 0;
    arch::x86_64::vmx_vmread(0x00006400 /* EXIT_QUALIFICATION */, qual);
    uint64_t rip = 0;
    arch::x86_64::vmx_vmread(0x0000681E /* GUEST_RIP */, rip);

    info.guest_rip = rip;
    info.exit_qualification = qual;

    uint32_t basic_reason = static_cast<uint32_t>(exit_reason_raw & 0xFFFF);
    switch (basic_reason) {
        case 10: info.reason = VmExitInfo::Reason::CpuidOrMrs; break;
        case 12: info.reason = VmExitInfo::Reason::Halt; break;
        case 18: info.reason = VmExitInfo::Reason::Hypercall; break;
        case 30: info.reason = VmExitInfo::Reason::IoPort; break;
        case 48: info.reason = VmExitInfo::Reason::EptViolation; break;
        default: info.reason = VmExitInfo::Reason::Unknown; break;
    }
#else
    info.reason = VmExitInfo::Reason::Unknown;
#endif

    state_ = VcpuState::Ready;
    return info;
}

} // namespace bminos

