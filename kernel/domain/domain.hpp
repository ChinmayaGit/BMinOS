// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Domain (Virtual Machine) abstraction.
//
// A Domain represents a complete isolated guest OS environment. Each domain
// owns a set of vCPUs, memory regions, and optionally assigned devices.
// The management domain (Dom0) has elevated privileges and manages other
// domains. This design draws inspiration from Xen's domain model while
// adding support for seamless OS switching and cross-domain bridging.

#pragma once

#include "kernel/common/types.hpp"
#include "kernel/sched/vcpu.hpp"

namespace bminos {

// ============================================================================
// Strong type for Domain identifiers — prevents accidentally passing a raw
// integer where a DomainId is expected (e.g., mixing with vCPU IDs).
// ============================================================================
struct DomainIdTag {};
using DomainId = StrongId<DomainIdTag, u32>;

// ============================================================================
// Domain lifecycle states
//
// State machine:
//   Creating -> Booting -> Running <-> Paused
//                  |          |           |
//                  v          v           v
//               Crashed   ShuttingDown  Suspended
//                             |
//                             v
//                         (destroyed)
// ============================================================================
enum class DomainState : u8 {
    Creating,      // Resources being allocated, page tables being built
    Booting,       // vCPUs created, guest firmware/kernel loading
    Running,       // At least one vCPU is schedulable
    Paused,        // All vCPUs frozen, state preserved in memory
    Suspended,     // State serialized to storage, memory can be reclaimed
    Crashed,       // Unrecoverable fault detected in guest
    ShuttingDown,  // Guest-initiated or host-initiated shutdown in progress
};

// ============================================================================
// Domain types determine privilege levels and resource access
// ============================================================================
enum class DomainType : u8 {
    Management,    // Dom0 — has direct hardware access, manages other domains
    Standard,      // Full guest OS (Windows, Linux, macOS guest)
    Lightweight,   // Minimal overhead container-like domain for single apps
};

// ============================================================================
// Isolation levels control the degree of separation between domains.
// Higher isolation = better security, lower isolation = better interop.
// ============================================================================
enum class IsolationLevel : u8 {
    Full,              // Complete isolation — no shared resources
    SharedStorage,     // Shared filesystem via paravirt block device
    SharedMemory,      // Shared memory regions for IPC
    FullIntegration,   // Clipboard, filesystem, display sharing enabled
};

// ============================================================================
// Memory region descriptor — represents a contiguous block of guest physical
// memory backed by host physical pages via EPT/Stage-2 page tables.
// ============================================================================
struct MemoryRegion {
    PhysAddr guest_phys_base;    // Guest physical address start
    PhysAddr host_phys_base;     // Backing host physical address
    u64      size_bytes;         // Region size
    bool     is_mmio;            // True if this maps to device MMIO, not RAM
    bool     is_readonly;        // Write-protect the mapping

    enum class CachePolicy : u8 {
        WriteBack,       // Normal RAM — WB caching
        Uncacheable,     // Device MMIO — UC
        WriteCombining,  // Framebuffer — WC for better GPU perf
    } cache_policy = CachePolicy::WriteBack;
};

// ============================================================================
// Device assignment entry — tracks which PCI device is passed through
// ============================================================================
struct DeviceAssignment {
    u16 bus;
    u8  device;
    u8  function;
    bool is_passthrough;  // True = direct passthrough, False = emulated
};

// ============================================================================
// Domain configuration — everything needed to create a new domain.
// Passed to Domain::create() to describe the desired guest environment.
// ============================================================================
struct DomainConfig {
    FixedString<64>   name;             // Human-readable domain name
    DomainType        type       = DomainType::Standard;
    IsolationLevel    isolation  = IsolationLevel::Full;

    u32               vcpu_count = 1;   // Number of virtual CPUs
    u64               memory_mb  = 512; // Memory allocation in MiB

    // OS identification — helps the scheduler and bridge engine optimize
    enum class OsType : u8 {
        Linux,
        Windows,
        MacOS,
        Custom,
    } os_type = OsType::Linux;

    // Devices to assign at creation time (up to 16 passthrough devices)
    static constexpr usize kMaxDevices = 16;
    DeviceAssignment  devices[kMaxDevices]{};
    u32               device_count = 0;

    // Cross-domain bridge support — only meaningful when isolation < Full
    bool              bridge_enabled = false;
};

// ============================================================================
// Domain class — represents a single guest OS virtual machine.
//
// Each domain maintains its own:
//   - Virtual CPUs (vCPUs) with saved register contexts
//   - EPT/Stage-2 page table root for memory isolation
//   - Assigned device list for passthrough
//   - Resource accounting (CPU time, memory usage)
//
// Thread safety: external callers must hold the domain lock (or the global
// domain table lock) before modifying domain state. The scheduler accesses
// vCPU state under its own locking protocol.
// ============================================================================
class Domain {
public:
    static constexpr u32 kMaxVcpus         = 64;
    static constexpr u32 kMaxMemoryRegions = 256;
    static constexpr u32 kMaxDevices       = 16;

    // --- Lifecycle management ---

    /// Create and initialize a domain from the given config.
    /// Allocates vCPUs, sets up EPT/Stage-2 root, but does NOT start the domain.
    static Result<DomainId> create(const DomainConfig& config);

    /// Destroy a domain, freeing all resources (vCPUs, memory, device assignments).
    /// The domain must be in Paused or ShuttingDown state.
    static Result<void> destroy(DomainId id);

    /// Pause all vCPUs in the domain (freeze execution, keep state in memory).
    static Result<void> pause(DomainId id);

    /// Resume a paused domain — make all vCPUs schedulable again.
    static Result<void> resume(DomainId id);

    /// Query the current lifecycle state of a domain.
    static Result<DomainState> get_state(DomainId id);

    /// Look up a domain by ID. Returns nullptr if not found.
    static Domain* find(DomainId id);

    // --- Accessors ---

    [[nodiscard]] DomainId          id() const          { return id_; }
    [[nodiscard]] DomainState       state() const       { return state_; }
    [[nodiscard]] DomainType        type() const        { return config_.type; }
    [[nodiscard]] const DomainConfig& config() const    { return config_; }
    [[nodiscard]] const char*       name() const        { return config_.name.c_str(); }
    [[nodiscard]] IsolationLevel    isolation() const    { return config_.isolation; }

    /// Get the number of vCPUs in this domain.
    [[nodiscard]] u32 vcpu_count() const { return vcpu_count_; }

    /// Get a vCPU by index (0-based). Returns nullptr if out of range.
    [[nodiscard]] VCPU* vcpu(u32 index);
    [[nodiscard]] const VCPU* vcpu(u32 index) const;

    /// Get the number of memory regions.
    [[nodiscard]] u32 memory_region_count() const { return memory_region_count_; }

    /// Get a memory region by index.
    [[nodiscard]] const MemoryRegion& memory_region(u32 index) const;

    // --- EPT / Stage-2 page table management ---

    /// Get the architecture-specific page table root pointer.
    /// On x86_64 this is the EPT PML4 physical address.
    /// On ARM64 this is the VTTBR_EL2 value.
    [[nodiscard]] PhysAddr ept_root() const { return ept_root_; }

    /// Set the EPT/Stage-2 root pointer (called during domain setup).
    void set_ept_root(PhysAddr root) { ept_root_ = root; }

    /// Add a memory region mapping (guest physical -> host physical).
    Result<void> add_memory_region(const MemoryRegion& region);

    // --- Device management ---

    /// Assign a PCI device to this domain for passthrough.
    Result<void> assign_device(const DeviceAssignment& dev);

    /// Remove a device assignment from this domain.
    Result<void> unassign_device(u16 bus, u8 device, u8 function);

    [[nodiscard]] u32 assigned_device_count() const { return device_count_; }
    [[nodiscard]] const DeviceAssignment& assigned_device(u32 index) const;

    // --- Resource accounting ---

    /// Total CPU time consumed by this domain across all vCPUs, in nanoseconds.
    [[nodiscard]] u64 total_cpu_time_ns() const { return total_cpu_time_ns_; }

    /// Record CPU time used by one of this domain's vCPUs.
    void account_cpu_time(u64 ns) { total_cpu_time_ns_ += ns; }

    // --- State transitions ---

    /// Transition the domain to a new state. Validates the transition is legal.
    Result<void> transition_to(DomainState new_state);

private:
    Domain() = default;

    DomainId        id_          = DomainId::invalid();
    DomainState     state_       = DomainState::Creating;
    DomainConfig    config_{};

    // Virtual CPUs owned by this domain
    VCPU            vcpus_[kMaxVcpus]{};
    u32             vcpu_count_  = 0;

    // Guest physical memory layout
    MemoryRegion    memory_regions_[kMaxMemoryRegions]{};
    u32             memory_region_count_ = 0;

    // Passthrough device assignments
    DeviceAssignment assigned_devices_[kMaxDevices]{};
    u32             device_count_ = 0;

    // Architecture-specific page table root (EPT PML4 or VTTBR_EL2)
    PhysAddr        ept_root_    = 0;

    // CPU time accounting (nanoseconds)
    u64             total_cpu_time_ns_ = 0;

    // Per-domain lock for concurrent access
    Spinlock        lock_{};

    // --- Global domain table ---
    // We use a simple fixed-size table for now. A production hypervisor would
    // use a more sophisticated data structure (hash map, slab allocator).
    static constexpr u32 kMaxDomains = 64;
    static Domain   domain_table_[kMaxDomains];
    static Atomic<u32> next_domain_id_;
    static Spinlock table_lock_;
};

} // namespace bminos
