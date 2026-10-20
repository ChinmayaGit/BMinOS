// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Virtual CPU (vCPU) abstraction.
//
// Each vCPU represents a single virtual processor within a Domain. On context
// switch, the full architectural state (GPRs, FPU/SIMD, system registers) is
// saved and restored. The actual VM entry/exit is architecture-specific:
//   - x86_64: VMLAUNCH/VMRESUME via VMCS
//   - ARM64: ERET to EL1 via VHE or non-VHE path
//
// This header provides the architecture-independent interface. The arch-specific
// implementations live under arch/x86_64/ and arch/arm64/.

#pragma once

#include "kernel/common/types.hpp"

namespace bminos {

// Forward declarations
struct DomainIdTag;
using DomainId = StrongId<DomainIdTag, u32>;

// ============================================================================
// Strong type for vCPU identifiers
// ============================================================================
struct VcpuIdTag {};
using VcpuId = StrongId<VcpuIdTag, u32>;

// ============================================================================
// vCPU scheduling states
// ============================================================================
enum class VcpuState : u8 {
    Ready,    // Eligible to be scheduled — waiting in the run queue
    Running,  // Currently executing on a physical CPU
    Blocked,  // Waiting on I/O or an event — not schedulable
    Halted,   // Guest executed HLT — wake on interrupt injection
};

// ============================================================================
// VM exit information — architecture-independent description of why the
// guest exited back to the hypervisor.
// ============================================================================
struct VmExitInfo {
    enum class Reason : u32 {
        ExternalInterrupt,     // Host interrupt during guest execution
        IoPort,                // IN/OUT instruction (x86)
        MmioAccess,            // Memory-mapped I/O access
        Hypercall,             // Guest called the hypervisor (VMCALL/HVC)
        CpuidOrMrs,            // CPUID (x86) or MRS/MSR (ARM) trap
        ControlRegAccess,      // CR access (x86) or system register (ARM)
        EptViolation,          // EPT/Stage-2 page fault
        Halt,                  // HLT instruction (x86) or WFI (ARM)
        Shutdown,              // Triple fault or guest shutdown
        TimerExpired,          // Preemption timer or virtual timer
        Unknown,
    } reason = Reason::Unknown;

    u64 exit_qualification;    // Architecture-specific exit qualification
    u64 guest_rip;             // Guest instruction pointer at exit
    u64 guest_physical_addr;   // For EPT violations: faulting GPA
    u32 io_port;               // For I/O exits: port number
    u32 io_size;               // For I/O exits: 1, 2, or 4 bytes
    bool io_is_write;          // For I/O exits: true = OUT, false = IN
};

// ============================================================================
// General purpose register file — architecture-independent view.
// The actual register set differs per architecture, but we present a
// unified interface for the scheduler and domain manager.
// ============================================================================
#if defined(__x86_64__)

struct GprState {
    u64 rax, rbx, rcx, rdx;
    u64 rsi, rdi, rbp, rsp;
    u64 r8, r9, r10, r11;
    u64 r12, r13, r14, r15;
    u64 rip, rflags;

    // Segment selectors
    u16 cs, ds, es, fs, gs, ss;

    // Control registers
    u64 cr0, cr2, cr3, cr4;

    // MSRs saved/restored on VM exit/entry
    u64 msr_efer;
    u64 msr_star;
    u64 msr_lstar;
    u64 msr_kernel_gs_base;
    u64 msr_pat;
};

/// VMCS-related state for Intel VT-x.
/// The VMCS itself is a 4KB hardware-managed structure; we store
/// the physical address and cache frequently-accessed fields.
struct VmcsState {
    PhysAddr vmcs_phys;        // Physical address of the 4KB VMCS region
    bool     launched;         // True after first VMLAUNCH (use VMRESUME after)

    // Host state pointers — set once during vCPU initialization
    u64 host_cr3;              // Host page table root
    u64 host_rsp;              // Host stack pointer for VM exit
    u64 host_rip;              // Host entry point for VM exit handler

    // EPT pointer (set per-domain)
    u64 eptp;
};

#elif defined(__aarch64__)

struct GprState {
    u64 x[31];                 // X0-X30 (X30 = LR)
    u64 sp_el1;                // Guest stack pointer (EL1)
    u64 pc;                    // Program counter (ELR_EL2)
    u64 pstate;                // Saved PSTATE (SPSR_EL2)

    // System registers
    u64 sctlr_el1;
    u64 ttbr0_el1;
    u64 ttbr1_el1;
    u64 tcr_el1;
    u64 mair_el1;
    u64 vbar_el1;
    u64 esr_el1;
    u64 far_el1;
};

/// ARM64 vCPU context — includes timer and GIC state.
struct ArmVcpuContext {
    // Virtual timer state
    u64 cntv_ctl_el0;
    u64 cntv_cval_el0;
    u64 cntvoff_el2;

    // Stage-2 translation table base
    u64 vttbr_el2;

    // HCR_EL2 — controls which operations trap to EL2
    u64 hcr_el2;
};

#else
// Provide a stub for non-target compilation (e.g., static analysis)
struct GprState {
    u64 placeholder[32];
};
#endif

// ============================================================================
// FPU / SIMD state — saved lazily (only on first FP instruction after switch)
// to avoid the cost of saving 512+ bytes of XMM/YMM/ZMM/NEON state every exit.
// ============================================================================
struct FpuState {
    alignas(64) u8 state[4096];  // Enough for XSAVE area (x86) or NEON (ARM)
    bool is_valid = false;       // True if state has been saved
};

// ============================================================================
// VCPU class — virtual processor within a Domain.
//
// The vCPU holds the complete guest CPU state. The scheduler decides which
// vCPU runs on which physical CPU; when selected, run() performs the VM
// entry and returns when the guest exits.
// ============================================================================
class VCPU {
public:
    /// Initialize this vCPU with the given IDs.
    /// Sets up the initial register state for guest boot.
    Result<void> initialize(VcpuId vcpu_id, DomainId domain_id, u32 vcpu_index);

    Result<void> init(DomainId domain_id, VcpuId vcpu_id) {
        return initialize(vcpu_id, domain_id, vcpu_id.raw());
    }

    /// Enter the guest and execute until a VM exit occurs.
    /// Returns information about why the guest exited.
    VmExitInfo run();

    /// Inject a virtual interrupt into the guest.
    /// The interrupt will be delivered on the next VM entry.
    void inject_interrupt(u32 vector);

    /// Save the full vCPU context (GPRs, FPU, arch-specific state).
    /// Called by the scheduler on context switch.
    void save_context();

    /// Restore the vCPU context before VM entry.
    void restore_context();

    // --- Accessors ---

    [[nodiscard]] VcpuId     id() const        { return id_; }
    [[nodiscard]] DomainId   domain_id() const { return domain_id_; }
    [[nodiscard]] u32        index() const     { return vcpu_index_; }
    [[nodiscard]] VcpuState  state() const     { return state_; }

    void set_state(VcpuState s) { state_ = s; }

    [[nodiscard]] GprState&       gpr_state()       { return gprs_; }
    [[nodiscard]] const GprState& gpr_state() const { return gprs_; }

    /// Physical CPU this vCPU is pinned to (or -1 for floating).
    [[nodiscard]] i32 pinned_pcpu() const { return pinned_pcpu_; }
    void pin_to_pcpu(i32 pcpu) { pinned_pcpu_ = pcpu; }

    /// CPU time accounting for this vCPU (nanoseconds).
    [[nodiscard]] u64 cpu_time_ns() const { return cpu_time_ns_; }
    void add_cpu_time(u64 ns) { cpu_time_ns_ += ns; }

    /// The physical CPU this vCPU last ran on (for cache affinity decisions).
    [[nodiscard]] u32 last_pcpu() const { return last_pcpu_; }
    void set_last_pcpu(u32 pcpu) { last_pcpu_ = pcpu; }

    // Pending interrupt tracking
    [[nodiscard]] bool has_pending_interrupt() const { return pending_interrupt_valid_; }
    [[nodiscard]] u32  pending_interrupt_vector() const { return pending_interrupt_vector_; }

private:
    VcpuId      id_          = VcpuId::invalid();
    DomainId    domain_id_   = DomainId::invalid();
    u32         vcpu_index_  = 0;
    VcpuState   state_       = VcpuState::Halted;

    // Register state
    GprState    gprs_{};
    FpuState    fpu_state_{};

    // Architecture-specific VM control structures
#if defined(__x86_64__)
    VmcsState   vmcs_{};
#elif defined(__aarch64__)
    ArmVcpuContext arm_ctx_{};
#endif

    // Scheduling metadata
    i32         pinned_pcpu_ = -1;      // -1 = floating (any pCPU)
    u32         last_pcpu_   = 0;       // Last physical CPU (affinity hint)
    u64         cpu_time_ns_ = 0;       // Total CPU time consumed

    // Pending interrupt injection
    bool        pending_interrupt_valid_ = false;
    u32         pending_interrupt_vector_ = 0;
};

} // namespace bminos
