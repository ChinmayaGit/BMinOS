// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Intel VT-x (VMX) Virtualization Support
// =======================================
// This module implements Intel's Virtual Machine Extensions (VMX), the hardware
// foundation for running guest VMs. VMX provides two new CPU modes:
//   - VMX root operation (host/hypervisor runs here)
//   - VMX non-root operation (guest runs here)
//
// Key data structures:
//   - VMXON region: 4KB-aligned page that enables VMX operation on a CPU core
//   - VMCS (Virtual Machine Control Structure): per-vCPU control structure
//     that defines guest state, host state, execution controls, and exit info.
//
// The VMCS is not directly readable — it must be accessed through VMREAD/VMWRITE
// instructions with field encodings. This module provides a clean C++ API over
// those raw instructions.
//
// References:
//   Intel SDM Volume 3, Chapter 23-33 (VMX)

#pragma once

#include <cstdint>
#include <cstddef>

namespace bminos::arch::x86_64 {

// =============================================================================
// MSR Definitions for VMX
// =============================================================================

/// Model-Specific Register addresses relevant to VMX
enum class VmxMsr : uint32_t {
    // VMX capability MSRs — these are read-only and report what the CPU supports
    IA32_VMX_BASIC              = 0x480, // VMCS revision ID, VMXON region size, memory type
    IA32_VMX_PINBASED_CTLS      = 0x481, // Allowed pin-based VM-execution controls
    IA32_VMX_PROCBASED_CTLS     = 0x482, // Allowed primary proc-based controls
    IA32_VMX_EXIT_CTLS          = 0x483, // Allowed VM-exit controls
    IA32_VMX_ENTRY_CTLS         = 0x484, // Allowed VM-entry controls
    IA32_VMX_MISC               = 0x485, // Miscellaneous VMX info
    IA32_VMX_CR0_FIXED0         = 0x486, // CR0 bits that must be 1 in VMX operation
    IA32_VMX_CR0_FIXED1         = 0x487, // CR0 bits that must be 0 in VMX operation
    IA32_VMX_CR4_FIXED0         = 0x488, // CR4 bits that must be 1 in VMX operation
    IA32_VMX_CR4_FIXED1         = 0x489, // CR4 bits that must be 0 in VMX operation
    IA32_VMX_VMCS_ENUM          = 0x48A, // Highest VMCS field index
    IA32_VMX_PROCBASED_CTLS2    = 0x48B, // Allowed secondary proc-based controls
    IA32_VMX_EPT_VPID_CAP       = 0x48C, // EPT and VPID capabilities
    IA32_VMX_TRUE_PINBASED_CTLS = 0x48D, // True pin-based controls (if supported)
    IA32_VMX_TRUE_PROCBASED_CTLS= 0x48E, // True proc-based controls
    IA32_VMX_TRUE_EXIT_CTLS     = 0x48F, // True exit controls
    IA32_VMX_TRUE_ENTRY_CTLS    = 0x490, // True entry controls

    // Feature control MSR — must be configured to allow VMX
    IA32_FEATURE_CONTROL        = 0x03A,
};

// =============================================================================
// VMCS Field Encodings
// =============================================================================
// VMCS fields are accessed via VMREAD/VMWRITE with a 32-bit encoding.
// Encoding format: [14:13]=width, [11:10]=type, [9:1]=index, [0]=access_type
//
// Width: 0=16-bit, 1=64-bit, 2=32-bit, 3=natural-width
// Type: 0=control, 1=VM-exit info, 2=guest state, 3=host state

enum class VmcsField : uint32_t {
    // =========================================================================
    // 16-bit control fields
    // =========================================================================
    VIRTUAL_PROCESSOR_ID            = 0x00000000,
    POSTED_INTERRUPT_NOTIFICATION   = 0x00000002,
    EPTP_INDEX                      = 0x00000004,

    // =========================================================================
    // 16-bit guest-state fields
    // =========================================================================
    GUEST_ES_SELECTOR               = 0x00000800,
    GUEST_CS_SELECTOR               = 0x00000802,
    GUEST_SS_SELECTOR               = 0x00000804,
    GUEST_DS_SELECTOR               = 0x00000806,
    GUEST_FS_SELECTOR               = 0x00000808,
    GUEST_GS_SELECTOR               = 0x0000080A,
    GUEST_LDTR_SELECTOR             = 0x0000080C,
    GUEST_TR_SELECTOR               = 0x0000080E,
    GUEST_INTERRUPT_STATUS          = 0x00000810,
    GUEST_PML_INDEX                 = 0x00000812,

    // =========================================================================
    // 16-bit host-state fields
    // =========================================================================
    HOST_ES_SELECTOR                = 0x00000C00,
    HOST_CS_SELECTOR                = 0x00000C02,
    HOST_SS_SELECTOR                = 0x00000C04,
    HOST_DS_SELECTOR                = 0x00000C06,
    HOST_FS_SELECTOR                = 0x00000C08,
    HOST_GS_SELECTOR                = 0x00000C0A,
    HOST_TR_SELECTOR                = 0x00000C0C,

    // =========================================================================
    // 64-bit control fields
    // =========================================================================
    IO_BITMAP_A                     = 0x00002000,
    IO_BITMAP_A_HIGH                = 0x00002001,
    IO_BITMAP_B                     = 0x00002002,
    IO_BITMAP_B_HIGH                = 0x00002003,
    MSR_BITMAP                      = 0x00002004,
    MSR_BITMAP_HIGH                 = 0x00002005,
    VM_EXIT_MSR_STORE_ADDR          = 0x00002006,
    VM_EXIT_MSR_LOAD_ADDR           = 0x00002008,
    VM_ENTRY_MSR_LOAD_ADDR          = 0x0000200A,
    EXECUTIVE_VMCS_POINTER          = 0x0000200C,
    PML_ADDRESS                     = 0x0000200E,
    TSC_OFFSET                      = 0x00002010,
    VIRTUAL_APIC_PAGE_ADDR          = 0x00002012,
    APIC_ACCESS_ADDR                = 0x00002014,
    POSTED_INTERRUPT_DESC_ADDR      = 0x00002016,
    VM_FUNCTION_CONTROLS            = 0x00002018,
    EPT_POINTER                     = 0x0000201A,
    EPT_POINTER_HIGH                = 0x0000201B,
    EOI_EXIT_BITMAP_0               = 0x0000201C,
    EOI_EXIT_BITMAP_1               = 0x0000201E,
    EOI_EXIT_BITMAP_2               = 0x00002020,
    EOI_EXIT_BITMAP_3               = 0x00002022,
    EPTP_LIST_ADDRESS               = 0x00002024,
    VMREAD_BITMAP                   = 0x00002026,
    VMWRITE_BITMAP                  = 0x00002028,
    XSS_EXITING_BITMAP              = 0x0000202C,
    ENCLS_EXITING_BITMAP            = 0x0000202E,
    TSC_MULTIPLIER                  = 0x00002032,

    // =========================================================================
    // 64-bit read-only data fields
    // =========================================================================
    GUEST_PHYSICAL_ADDRESS          = 0x00002400,

    // =========================================================================
    // 64-bit guest-state fields
    // =========================================================================
    VMCS_LINK_POINTER               = 0x00002800,
    GUEST_IA32_DEBUGCTL             = 0x00002802,
    GUEST_IA32_PAT                  = 0x00002804,
    GUEST_IA32_EFER                 = 0x00002806,
    GUEST_IA32_PERF_GLOBAL_CTRL     = 0x00002808,
    GUEST_PDPTE0                    = 0x0000280A,
    GUEST_PDPTE1                    = 0x0000280C,
    GUEST_PDPTE2                    = 0x0000280E,
    GUEST_PDPTE3                    = 0x00002810,
    GUEST_IA32_BNDCFGS              = 0x00002812,

    // =========================================================================
    // 64-bit host-state fields
    // =========================================================================
    HOST_IA32_PAT                   = 0x00002C00,
    HOST_IA32_EFER                  = 0x00002C02,
    HOST_IA32_PERF_GLOBAL_CTRL      = 0x00002C04,

    // =========================================================================
    // 32-bit control fields
    // =========================================================================
    PIN_BASED_VM_EXEC_CONTROLS      = 0x00004000,
    PRIMARY_PROC_BASED_VM_EXEC_CONTROLS = 0x00004002,
    EXCEPTION_BITMAP                = 0x00004004,
    PAGE_FAULT_ERROR_CODE_MASK      = 0x00004006,
    PAGE_FAULT_ERROR_CODE_MATCH     = 0x00004008,
    CR3_TARGET_COUNT                = 0x0000400A,
    VM_EXIT_CONTROLS                = 0x0000400C,
    VM_EXIT_MSR_STORE_COUNT         = 0x0000400E,
    VM_EXIT_MSR_LOAD_COUNT          = 0x00004010,
    VM_ENTRY_CONTROLS               = 0x00004012,
    VM_ENTRY_MSR_LOAD_COUNT         = 0x00004014,
    VM_ENTRY_INTERRUPTION_INFO      = 0x00004016,
    VM_ENTRY_EXCEPTION_ERROR_CODE   = 0x00004018,
    VM_ENTRY_INSTRUCTION_LENGTH     = 0x0000401A,
    TPR_THRESHOLD                   = 0x0000401C,
    SECONDARY_PROC_BASED_VM_EXEC_CONTROLS = 0x0000401E,
    PLE_GAP                         = 0x00004020,
    PLE_WINDOW                      = 0x00004022,

    // =========================================================================
    // 32-bit read-only data fields (VM-exit information)
    // =========================================================================
    VM_INSTRUCTION_ERROR            = 0x00004400,
    VM_EXIT_REASON                  = 0x00004402,
    VM_EXIT_INTERRUPTION_INFO       = 0x00004404,
    VM_EXIT_INTERRUPTION_ERROR_CODE = 0x00004406,
    IDT_VECTORING_INFO              = 0x00004408,
    IDT_VECTORING_ERROR_CODE        = 0x0000440A,
    VM_EXIT_INSTRUCTION_LENGTH      = 0x0000440C,
    VM_EXIT_INSTRUCTION_INFO        = 0x0000440E,

    // =========================================================================
    // 32-bit guest-state fields
    // =========================================================================
    GUEST_ES_LIMIT                  = 0x00004800,
    GUEST_CS_LIMIT                  = 0x00004802,
    GUEST_SS_LIMIT                  = 0x00004804,
    GUEST_DS_LIMIT                  = 0x00004806,
    GUEST_FS_LIMIT                  = 0x00004808,
    GUEST_GS_LIMIT                  = 0x0000480A,
    GUEST_LDTR_LIMIT                = 0x0000480C,
    GUEST_TR_LIMIT                  = 0x0000480E,
    GUEST_GDTR_LIMIT                = 0x00004810,
    GUEST_IDTR_LIMIT                = 0x00004812,
    GUEST_ES_ACCESS_RIGHTS          = 0x00004814,
    GUEST_CS_ACCESS_RIGHTS          = 0x00004816,
    GUEST_SS_ACCESS_RIGHTS          = 0x00004818,
    GUEST_DS_ACCESS_RIGHTS          = 0x0000481A,
    GUEST_FS_ACCESS_RIGHTS          = 0x0000481C,
    GUEST_GS_ACCESS_RIGHTS          = 0x0000481E,
    GUEST_LDTR_ACCESS_RIGHTS        = 0x00004820,
    GUEST_TR_ACCESS_RIGHTS          = 0x00004822,
    GUEST_INTERRUPTIBILITY_STATE    = 0x00004824,
    GUEST_ACTIVITY_STATE            = 0x00004826,
    GUEST_SMBASE                    = 0x00004828,
    GUEST_IA32_SYSENTER_CS          = 0x0000482A,
    GUEST_VMX_PREEMPTION_TIMER      = 0x0000482E,

    // =========================================================================
    // 32-bit host-state fields
    // =========================================================================
    HOST_IA32_SYSENTER_CS           = 0x00004C00,

    // =========================================================================
    // Natural-width control fields
    // =========================================================================
    CR0_GUEST_HOST_MASK             = 0x00006000,
    CR4_GUEST_HOST_MASK             = 0x00006002,
    CR0_READ_SHADOW                 = 0x00006004,
    CR4_READ_SHADOW                 = 0x00006006,
    CR3_TARGET_VALUE_0              = 0x00006008,
    CR3_TARGET_VALUE_1              = 0x0000600A,
    CR3_TARGET_VALUE_2              = 0x0000600C,
    CR3_TARGET_VALUE_3              = 0x0000600E,

    // =========================================================================
    // Natural-width read-only data fields
    // =========================================================================
    EXIT_QUALIFICATION              = 0x00006400,
    IO_RCX                          = 0x00006402,
    IO_RSI                          = 0x00006404,
    IO_RDI                          = 0x00006406,
    IO_RIP                          = 0x00006408,
    GUEST_LINEAR_ADDRESS            = 0x0000640A,

    // =========================================================================
    // Natural-width guest-state fields
    // =========================================================================
    GUEST_CR0                       = 0x00006800,
    GUEST_CR3                       = 0x00006802,
    GUEST_CR4                       = 0x00006804,
    GUEST_ES_BASE                   = 0x00006806,
    GUEST_CS_BASE                   = 0x00006808,
    GUEST_SS_BASE                   = 0x0000680A,
    GUEST_DS_BASE                   = 0x0000680C,
    GUEST_FS_BASE                   = 0x0000680E,
    GUEST_GS_BASE                   = 0x00006810,
    GUEST_LDTR_BASE                 = 0x00006812,
    GUEST_TR_BASE                   = 0x00006814,
    GUEST_GDTR_BASE                 = 0x00006816,
    GUEST_IDTR_BASE                 = 0x00006818,
    GUEST_DR7                       = 0x0000681A,
    GUEST_RSP                       = 0x0000681C,
    GUEST_RIP                       = 0x0000681E,
    GUEST_RFLAGS                    = 0x00006820,
    GUEST_PENDING_DEBUG_EXCEPTIONS  = 0x00006822,
    GUEST_IA32_SYSENTER_ESP         = 0x00006824,
    GUEST_IA32_SYSENTER_EIP         = 0x00006826,

    // =========================================================================
    // Natural-width host-state fields
    // =========================================================================
    HOST_CR0                        = 0x00006C00,
    HOST_CR3                        = 0x00006C02,
    HOST_CR4                        = 0x00006C04,
    HOST_FS_BASE                    = 0x00006C06,
    HOST_GS_BASE                    = 0x00006C08,
    HOST_TR_BASE                    = 0x00006C0A,
    HOST_GDTR_BASE                  = 0x00006C0C,
    HOST_IDTR_BASE                  = 0x00006C0E,
    HOST_IA32_SYSENTER_ESP          = 0x00006C10,
    HOST_IA32_SYSENTER_EIP          = 0x00006C12,
    HOST_RSP                        = 0x00006C14,
    HOST_RIP                        = 0x00006C16,
};

// =============================================================================
// VM-Exit Reasons
// =============================================================================
// The basic exit reason is in bits [15:0] of the VM_EXIT_REASON field.
// Bit 31 indicates "entry failure" if set.

enum class VmExitReason : uint32_t {
    EXCEPTION_OR_NMI            = 0,
    EXTERNAL_INTERRUPT          = 1,
    TRIPLE_FAULT                = 2,
    INIT_SIGNAL                 = 3,
    STARTUP_IPI                 = 4,
    IO_SMI                      = 5,
    OTHER_SMI                   = 6,
    INTERRUPT_WINDOW            = 7,
    NMI_WINDOW                  = 8,
    TASK_SWITCH                 = 9,
    CPUID                       = 10,
    GETSEC                      = 11,
    HLT                         = 12,
    INVD                        = 13,
    INVLPG                      = 14,
    RDPMC                       = 15,
    RDTSC                       = 16,
    RSM                         = 17,
    VMCALL                      = 18,
    VMCLEAR                     = 19,
    VMLAUNCH                    = 20,
    VMPTRLD                     = 21,
    VMPTRST                     = 22,
    VMREAD                      = 23,
    VMRESUME                    = 24,
    VMWRITE                     = 25,
    VMXOFF                      = 26,
    VMXON                       = 27,
    CR_ACCESS                   = 28,
    MOV_DR                      = 29,
    IO_INSTRUCTION              = 30,
    RDMSR                       = 31,
    WRMSR                       = 32,
    VM_ENTRY_FAILURE_GUEST      = 33,
    VM_ENTRY_FAILURE_MSR        = 34,
    MWAIT                       = 36,
    MONITOR_TRAP_FLAG           = 37,
    MONITOR                     = 39,
    PAUSE                       = 40,
    VM_ENTRY_FAILURE_MC         = 41,
    TPR_BELOW_THRESHOLD         = 43,
    APIC_ACCESS                 = 44,
    VIRTUALIZED_EOI             = 45,
    GDTR_IDTR_ACCESS            = 46,
    LDTR_TR_ACCESS              = 47,
    EPT_VIOLATION               = 48,
    EPT_MISCONFIGURATION        = 49,
    INVEPT                      = 50,
    RDTSCP                      = 51,
    VMX_PREEMPTION_TIMER        = 52,
    INVVPID                     = 53,
    WBINVD                      = 54,
    XSETBV                      = 55,
    APIC_WRITE                  = 56,
    RDRAND                      = 57,
    INVPCID                     = 58,
    VMFUNC                      = 59,
    ENCLS                       = 60,
    RDSEED                      = 61,
    PML_FULL                    = 62,
    XSAVES                     = 63,
    XRSTORS                     = 64,
};

// =============================================================================
// VM-Execution Control Bits
// =============================================================================

/// Pin-based VM-execution controls
enum class PinBasedControls : uint32_t {
    EXTERNAL_INTERRUPT_EXITING  = 1 << 0,
    NMI_EXITING                 = 1 << 3,
    VIRTUAL_NMIS                = 1 << 5,
    VMX_PREEMPTION_TIMER        = 1 << 6,
    POSTED_INTERRUPTS           = 1 << 7,
};

/// Primary processor-based VM-execution controls
enum class PrimaryProcBasedControls : uint32_t {
    INTERRUPT_WINDOW_EXITING    = 1 << 2,
    USE_TSC_OFFSETTING          = 1 << 3,
    HLT_EXITING                = 1 << 7,
    INVLPG_EXITING              = 1 << 9,
    MWAIT_EXITING               = 1 << 10,
    RDPMC_EXITING               = 1 << 11,
    RDTSC_EXITING               = 1 << 12,
    CR3_LOAD_EXITING            = 1 << 15,
    CR3_STORE_EXITING           = 1 << 16,
    CR8_LOAD_EXITING            = 1 << 19,
    CR8_STORE_EXITING           = 1 << 20,
    USE_TPR_SHADOW              = 1 << 21,
    NMI_WINDOW_EXITING          = 1 << 22,
    MOV_DR_EXITING              = 1 << 23,
    UNCONDITIONAL_IO_EXITING    = 1 << 24,
    USE_IO_BITMAPS              = 1 << 25,
    MONITOR_TRAP_FLAG           = 1 << 27,
    USE_MSR_BITMAPS             = 1 << 28,
    MONITOR_EXITING             = 1 << 29,
    PAUSE_EXITING               = 1 << 30,
    ACTIVATE_SECONDARY_CONTROLS = 1U << 31,
};

/// Secondary processor-based VM-execution controls
enum class SecondaryProcBasedControls : uint32_t {
    VIRTUALIZE_APIC_ACCESSES    = 1 << 0,
    ENABLE_EPT                  = 1 << 1,
    DESCRIPTOR_TABLE_EXITING    = 1 << 2,
    ENABLE_RDTSCP               = 1 << 3,
    VIRTUALIZE_X2APIC_MODE      = 1 << 4,
    ENABLE_VPID                 = 1 << 5,
    WBINVD_EXITING              = 1 << 6,
    UNRESTRICTED_GUEST          = 1 << 7,
    APIC_REGISTER_VIRTUALIZATION= 1 << 8,
    VIRTUAL_INTERRUPT_DELIVERY  = 1 << 9,
    PAUSE_LOOP_EXITING          = 1 << 10,
    RDRAND_EXITING              = 1 << 11,
    ENABLE_INVPCID              = 1 << 12,
    ENABLE_VM_FUNCTIONS         = 1 << 13,
    VMCS_SHADOWING              = 1 << 14,
    ENABLE_ENCLS_EXITING        = 1 << 15,
    RDSEED_EXITING              = 1 << 16,
    ENABLE_PML                  = 1 << 17,
    EPT_VIOLATION_VE            = 1 << 18,
    CONCEAL_VMX_FROM_PT         = 1 << 19,
    ENABLE_XSAVES_XRSTORS      = 1 << 20,
    MODE_BASED_EPT_EXECUTE      = 1 << 22,
    TSC_SCALING                 = 1 << 25,
};

/// VM-exit controls
enum class VmExitControls : uint32_t {
    SAVE_DEBUG_CONTROLS         = 1 << 2,
    HOST_ADDRESS_SPACE_SIZE     = 1 << 9,  // Must be 1 for 64-bit host
    LOAD_IA32_PERF_GLOBAL_CTRL  = 1 << 12,
    ACKNOWLEDGE_INTERRUPT       = 1 << 15,
    SAVE_IA32_PAT               = 1 << 18,
    LOAD_IA32_PAT               = 1 << 19,
    SAVE_IA32_EFER              = 1 << 20,
    LOAD_IA32_EFER              = 1 << 21,
    SAVE_VMX_PREEMPTION_TIMER   = 1 << 22,
    CLEAR_IA32_BNDCFGS          = 1 << 23,
    CONCEAL_VMX_FROM_PT         = 1 << 24,
};

/// VM-entry controls
enum class VmEntryControls : uint32_t {
    LOAD_DEBUG_CONTROLS         = 1 << 2,
    IA32E_MODE_GUEST            = 1 << 9,   // Guest is in 64-bit mode
    ENTRY_TO_SMM                = 1 << 10,
    DEACTIVATE_DUAL_MONITOR     = 1 << 11,
    LOAD_IA32_PERF_GLOBAL_CTRL  = 1 << 13,
    LOAD_IA32_PAT               = 1 << 14,
    LOAD_IA32_EFER              = 1 << 15,
    LOAD_IA32_BNDCFGS           = 1 << 16,
    CONCEAL_VMX_FROM_PT         = 1 << 17,
};

// =============================================================================
// VMX Capability Information
// =============================================================================

/// Parsed VMX capabilities read from MSRs during initialization.
/// This captures what the CPU supports so we can configure VMCS fields correctly.
struct VmxCapabilities {
    uint32_t vmcs_revision_id;          // Must be written to VMXON/VMCS regions
    uint32_t vmxon_region_size;         // Size of VMXON region (usually 4KB)
    bool     true_controls_supported;   // Whether IA32_VMX_TRUE_*_CTLS exist

    // Allowed settings for each control field (bit set = allowed to be 1)
    uint32_t pin_based_allowed0;        // Bits that can be 0
    uint32_t pin_based_allowed1;        // Bits that can be 1
    uint32_t proc_based_allowed0;
    uint32_t proc_based_allowed1;
    uint32_t secondary_proc_allowed0;
    uint32_t secondary_proc_allowed1;
    uint32_t exit_allowed0;
    uint32_t exit_allowed1;
    uint32_t entry_allowed0;
    uint32_t entry_allowed1;

    // EPT capabilities
    bool     ept_supported;
    bool     ept_execute_only;          // EPT entries can be execute-only
    bool     ept_page_walk_4;           // 4-level EPT page walk
    bool     ept_uncacheable;           // UC memory type in EPT
    bool     ept_writeback;             // WB memory type in EPT
    bool     ept_2mb_pages;             // 2MB EPT pages supported
    bool     ept_1gb_pages;             // 1GB EPT pages supported
    bool     invept_supported;          // INVEPT instruction supported
    bool     invept_single_context;
    bool     invept_all_context;

    // VPID capabilities
    bool     vpid_supported;
    bool     invvpid_individual;
    bool     invvpid_single_context;
    bool     invvpid_all_context;
    bool     invvpid_single_context_retaining_globals;

    // Unrestricted guest
    bool     unrestricted_guest;
};

// =============================================================================
// Guest Register State
// =============================================================================

/// Saved general-purpose register state for a vCPU.
/// These registers are NOT stored in the VMCS — they must be saved/restored
/// manually by the VM-exit/entry handler via push/pop.
struct __attribute__((packed)) GuestRegisters {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;

    // XMM registers (for future FPU/SSE state — may be moved to XSAVE area)
    // uint128_t xmm0..xmm15;
};

// =============================================================================
// VMCS Wrapper
// =============================================================================

/// A wrapper around a VMCS (Virtual Machine Control Structure).
/// The VMCS is a 4KB-aligned region of memory that the CPU uses to manage
/// guest/host state transitions. It cannot be read directly — VMREAD/VMWRITE
/// instructions must be used.
///
/// Each vCPU has its own VMCS. Before accessing a VMCS with VMREAD/VMWRITE,
/// it must be made "current" on the physical CPU via VMPTRLD.
class Vmcs {
public:
    Vmcs() = default;
    ~Vmcs() = default;

    // Non-copyable (the physical VMCS region is unique per vCPU)
    Vmcs(const Vmcs&) = delete;
    Vmcs& operator=(const Vmcs&) = delete;

    /// Allocate and initialize the VMCS region.
    /// @param revision_id The VMCS revision ID from IA32_VMX_BASIC MSR.
    /// @return true on success.
    bool allocate(uint32_t revision_id);

    /// Free the VMCS region.
    void free();

    /// Load this VMCS as the current VMCS on this physical CPU (VMPTRLD).
    /// @return true on success.
    bool load();

    /// Clear the VMCS (VMCLEAR) — makes it inactive and ensures writeback.
    /// @return true on success.
    bool clear();

    /// Read a VMCS field (VMREAD).
    /// @param field The field encoding to read.
    /// @param value Output: the field's value.
    /// @return true on success.
    [[nodiscard]] bool read(VmcsField field, uint64_t& value) const;

    /// Write a VMCS field (VMWRITE).
    /// @param field The field encoding to write.
    /// @param value The value to write.
    /// @return true on success.
    bool write(VmcsField field, uint64_t value);

    /// Get the physical address of the VMCS region.
    [[nodiscard]] uint64_t physical_address() const { return m_phys_addr; }

    /// Check if this VMCS has been allocated.
    [[nodiscard]] bool is_allocated() const { return m_phys_addr != 0; }

private:
    uint64_t m_phys_addr = 0;  // Physical address of the VMCS region
    void*    m_virt_addr = nullptr; // Virtual address (via direct map)
};

// =============================================================================
// VMX Manager
// =============================================================================

/// Top-level manager for VMX operations on a single physical CPU core.
///
/// Lifecycle:
///   1. detect_vmx_support() — check CPUID for VMX
///   2. read_capabilities() — read MSRs to learn what the CPU supports
///   3. enable_vmx() — set CR4.VMXE and execute VMXON
///   4. Create VMCS instances for each vCPU, configure them
///   5. vmlaunch() / vmresume() to enter guest execution
///   6. Handle VM-exits in the exit handler
///   7. disable_vmx() — VMXOFF when shutting down
class VmxManager {
public:
    VmxManager() = default;

    // Non-copyable, non-movable (per-CPU instance)
    VmxManager(const VmxManager&) = delete;
    VmxManager& operator=(const VmxManager&) = delete;

    /// Check if the CPU supports VMX via CPUID.
    /// @return true if VMX is supported and can be enabled.
    [[nodiscard]] static bool detect_vmx_support();

    /// Read VMX capability MSRs and populate the capabilities structure.
    /// Must be called before enable_vmx().
    /// @return true on success.
    bool read_capabilities();

    /// Enable VMX operation on this CPU core.
    /// Sets CR4.VMXE, allocates the VMXON region, and executes VMXON.
    /// @return true on success.
    bool enable_vmx();

    /// Disable VMX operation (VMXOFF). Call during shutdown.
    void disable_vmx();

    /// Launch a new guest vCPU (first entry into guest mode).
    /// The VMCS must be fully configured before calling this.
    /// @param vmcs The VMCS to launch.
    /// @param guest_regs Guest general-purpose register state.
    /// @return The VM-exit reason.
    VmExitReason vmlaunch(Vmcs& vmcs, GuestRegisters& guest_regs);

    /// Resume a previously-launched guest vCPU.
    /// @param vmcs The VMCS to resume.
    /// @param guest_regs Guest general-purpose register state.
    /// @return The VM-exit reason.
    VmExitReason vmresume(Vmcs& vmcs, GuestRegisters& guest_regs);

    /// Handle a VM-exit. Called from the assembly VM-exit stub.
    /// Dispatches to the appropriate handler based on the exit reason.
    /// @param guest_regs Saved guest registers.
    void handle_vm_exit(GuestRegisters& guest_regs);

    /// Adjust a VMCS control field value to satisfy allowed0/allowed1 constraints.
    /// Bits in allowed0 must be 1; bits not in allowed1 must be 0.
    /// @param desired The desired control value.
    /// @param allowed0 Bits that must be 1.
    /// @param allowed1 Bits that may be 1.
    /// @return The adjusted control value.
    [[nodiscard]] static uint32_t adjust_controls(uint32_t desired,
                                                   uint32_t allowed0,
                                                   uint32_t allowed1);

    /// Get the VMX capabilities.
    [[nodiscard]] const VmxCapabilities& capabilities() const { return m_caps; }

    /// Check if VMX is currently enabled on this CPU.
    [[nodiscard]] bool is_enabled() const { return m_vmx_enabled; }

private:
    /// Read a Model-Specific Register.
    [[nodiscard]] static uint64_t rdmsr(uint32_t msr);

    /// Write a Model-Specific Register.
    static void wrmsr(uint32_t msr, uint64_t value);

    /// Read CR0
    [[nodiscard]] static uint64_t read_cr0();

    /// Read CR4
    [[nodiscard]] static uint64_t read_cr4();

    /// Write CR4
    static void write_cr4(uint64_t value);

    /// Execute the VMXON instruction.
    /// @param vmxon_region_phys Physical address of the VMXON region.
    /// @return true on success.
    [[nodiscard]] static bool execute_vmxon(uint64_t vmxon_region_phys);

    /// Execute the VMXOFF instruction.
    static void execute_vmxoff();

    // -------------------------------------------------------------------------
    // VM-exit handlers (dispatched from handle_vm_exit)
    // -------------------------------------------------------------------------

    void handle_cpuid_exit(GuestRegisters& regs);
    void handle_io_exit(GuestRegisters& regs);
    void handle_msr_read_exit(GuestRegisters& regs);
    void handle_msr_write_exit(GuestRegisters& regs);
    void handle_ept_violation(GuestRegisters& regs);
    void handle_vmcall_exit(GuestRegisters& regs);
    void handle_cr_access_exit(GuestRegisters& regs);
    void handle_exception_exit(GuestRegisters& regs);
    void handle_hlt_exit(GuestRegisters& regs);
    void handle_triple_fault(GuestRegisters& regs);

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    VmxCapabilities m_caps{};               // Parsed VMX capabilities
    uint64_t        m_vmxon_region_phys = 0;// Physical address of VMXON region
    void*           m_vmxon_region_virt = nullptr;
    bool            m_vmx_enabled = false;  // Whether VMXON has been executed
};

/// Per-CPU VMX manager instances (indexed by CPU core number)
/// In a real system, this would be per-CPU data. For now, we expose a
/// single instance for the BSP (Bootstrap Processor).
extern VmxManager g_vmx;

// =============================================================================
// Low-level VMX instruction wrappers (implemented in assembly or inline asm)
// =============================================================================

/// Execute VMREAD instruction
/// @return true on success (CF=0 and ZF=0)
bool vmx_vmread(uint64_t field, uint64_t& value);

/// Execute VMWRITE instruction
/// @return true on success
bool vmx_vmwrite(uint64_t field, uint64_t value);

/// Execute VMPTRLD instruction
/// @return true on success
bool vmx_vmptrld(uint64_t vmcs_phys);

/// Execute VMCLEAR instruction
/// @return true on success
bool vmx_vmclear(uint64_t vmcs_phys);

/// Execute VMLAUNCH instruction
/// @return false if the launch failed (should not return on success — returns
///         when a VM-exit occurs, which is handled separately)
bool vmx_vmlaunch();

/// Execute VMRESUME instruction
/// @return false if resume failed
bool vmx_vmresume();

/// Execute INVEPT instruction
/// @param type 1 = single-context, 2 = all-context
/// @param eptp The EPT pointer for single-context invalidation
/// @return true on success
bool vmx_invept(uint64_t type, uint64_t eptp);

/// Execute INVVPID instruction
/// @param type Invalidation type
/// @param vpid The VPID value
/// @param linear_address Linear address for address-specific invalidation
/// @return true on success
bool vmx_invvpid(uint64_t type, uint16_t vpid, uint64_t linear_address);

} // namespace bminos::arch::x86_64
