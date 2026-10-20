// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// IOMMU Management for Device Isolation.
//
// The IOMMU provides DMA remapping — it intercepts all memory accesses from
// PCI devices and translates them through per-device page tables. This is
// critical for security: without IOMMU protection, a passthrough device
// could DMA into any physical memory, including other domains' memory or
// hypervisor code.
//
// Supported hardware:
//   - Intel VT-d (DMA Remapping via DMAR tables)
//   - AMD-Vi (AMD IOMMU via IVRS tables)
//   - ARM SMMU (System MMU for ARM platforms)
//
// Design:
//   - Each domain gets its own IOMMU domain (set of DMA page tables)
//   - Devices assigned to a domain are mapped into that IOMMU domain
//   - DMA addresses are translated: IOVA -> host physical address
//   - Interrupt remapping is also handled (prevent MSI injection attacks)

#pragma once

#include "kernel/common/types.hpp"
#include "kernel/domain/domain.hpp"

namespace bminos {

// Forward declaration
struct PciDevice;

// ============================================================================
// IOMMU hardware type
// ============================================================================
enum class IommuType : u8 {
    None,         // No IOMMU detected (unsafe for passthrough!)
    IntelVTd,     // Intel Virtualization Technology for Directed I/O
    AmdVi,        // AMD I/O Virtualization
    ArmSmmu,      // ARM System Memory Management Unit
};

// ============================================================================
// IOMMU domain — represents a set of DMA page tables for device isolation.
//
// Each hypervisor Domain that has passthrough devices gets an IOMMU domain.
// Multiple PCI devices can share an IOMMU domain if they belong to the same
// IOMMU group (devices that can see each other's DMA traffic).
// ============================================================================
struct IommuDomainIdTag {};
using IommuDomainId = StrongId<IommuDomainIdTag, u32>;

struct IommuDomain {
    IommuDomainId  id          = IommuDomainId::invalid();
    DomainId       owner       = DomainId::invalid();  // Hypervisor domain this belongs to
    PhysAddr       page_table_root = 0;  // Root of DMA page tables
    u32            device_count = 0;     // Number of devices in this domain
    bool           is_active   = false;

    // Statistics
    u64            dma_mappings = 0;     // Number of active DMA mappings
    u64            faults       = 0;     // DMA fault count
};

// ============================================================================
// DMA mapping flags
// ============================================================================
enum class DmaMapFlags : u32 {
    Read       = 0x01,  // Device can read from this mapping
    Write      = 0x02,  // Device can write to this mapping
    ReadWrite  = 0x03,  // Device can read and write
    NoSnoop    = 0x04,  // Skip CPU cache snooping (for coherent devices)
};

/// Bitwise OR for DMA map flags
constexpr DmaMapFlags operator|(DmaMapFlags a, DmaMapFlags b) {
    return static_cast<DmaMapFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}

constexpr bool operator&(DmaMapFlags a, DmaMapFlags b) {
    return (static_cast<u32>(a) & static_cast<u32>(b)) != 0;
}

// ============================================================================
// IOMMU fault information — reported when a device performs an invalid DMA
// ============================================================================
struct IommuFault {
    PhysAddr    faulting_address;   // The IOVA that caused the fault
    u16         source_bus;        // PCI BDF of the faulting device
    u8          source_device;
    u8          source_function;
    bool        is_write;          // True if the fault was on a write
    bool        is_present;        // True if the page was present (permission fault)
    u64         timestamp_ns;
};

// ============================================================================
// IommuManager — detects, initializes, and manages the platform IOMMU.
//
// Usage:
//   IommuManager iommu;
//   iommu.detect();           // Find IOMMU hardware
//   iommu.init();             // Initialize and enable
//   auto dom = iommu.create_domain(guest_domain_id);
//   iommu.map_device(dom, gpu_device);
//   iommu.map_dma(dom, 0x1000, phys_addr, 4096, DmaMapFlags::ReadWrite);
//
// The IOMMU must be initialized before any device passthrough is configured.
// If no IOMMU is detected, passthrough is unsafe and should be refused
// (or at minimum, warned about).
// ============================================================================
class IommuManager {
public:
    /// Detect the platform IOMMU by scanning ACPI tables.
    ///   - Intel: look for DMAR table
    ///   - AMD: look for IVRS table
    ///   - ARM: look for IORT table with SMMU nodes
    Result<void> detect();

    /// Initialize and enable the IOMMU.
    /// Sets up the root table, enables DMA remapping and interrupt remapping.
    Result<void> init();

    /// Get the detected IOMMU type.
    [[nodiscard]] IommuType type() const { return type_; }

    /// Check if an IOMMU is available and initialized.
    [[nodiscard]] bool is_available() const { return type_ != IommuType::None && initialized_; }

    // --- IOMMU domain management ---

    /// Create a new IOMMU domain for a hypervisor domain.
    /// Allocates DMA page tables.
    Result<IommuDomainId> create_domain(DomainId owner);

    /// Destroy an IOMMU domain and invalidate all its mappings.
    Result<void> destroy_domain(IommuDomainId id);

    /// Get an IOMMU domain by ID.
    [[nodiscard]] IommuDomain* find_domain(IommuDomainId id);

    // --- Device mapping ---

    /// Map a PCI device into an IOMMU domain.
    /// All DMA from this device will be translated through the domain's page tables.
    Result<void> map_device(IommuDomainId domain, const PciDevice& device);

    /// Unmap a device from its IOMMU domain (reclaim to host).
    Result<void> unmap_device(IommuDomainId domain, const PciDevice& device);

    // --- DMA address mapping ---

    /// Create a DMA mapping: IOVA -> physical address.
    /// The device will see `iova` as the DMA address; the IOMMU translates
    /// it to `phys_addr` in host physical memory.
    Result<void> map_dma(IommuDomainId domain, u64 iova, PhysAddr phys_addr,
                          u64 size, DmaMapFlags flags = DmaMapFlags::ReadWrite);

    /// Remove a DMA mapping.
    Result<void> unmap_dma(IommuDomainId domain, u64 iova, u64 size);

    // --- TLB invalidation ---

    /// Invalidate IOMMU TLB entries for a domain.
    /// Must be called after modifying DMA page tables.
    void invalidate_tlb(IommuDomainId domain);

    /// Global IOMMU TLB invalidation (expensive, use sparingly).
    void invalidate_all();

    // --- Fault handling ---

    /// Register a fault handler callback.
    using FaultHandler = void(*)(const IommuFault& fault);
    void set_fault_handler(FaultHandler handler) { fault_handler_ = handler; }

    /// Process pending IOMMU faults (called from interrupt handler).
    void handle_faults();

    /// Get the total number of DMA faults observed.
    [[nodiscard]] u64 total_faults() const { return total_faults_; }

private:
    // --- Platform-specific initialization ---
    Result<void> init_intel_vtd();
    Result<void> init_amd_vi();
    Result<void> init_arm_smmu();

    // --- Platform-specific DMA page table operations ---
    Result<void> platform_map_dma(IommuDomain& domain, u64 iova,
                                    PhysAddr phys_addr, u64 size, DmaMapFlags flags);
    Result<void> platform_unmap_dma(IommuDomain& domain, u64 iova, u64 size);
    Result<void> platform_map_device(IommuDomain& domain, const PciDevice& device);

    // --- State ---

    IommuType      type_        = IommuType::None;
    bool           initialized_ = false;

    // IOMMU hardware registers base address (MMIO)
    VirtAddr       regs_base_   = 0;

    // IOMMU domain table
    static constexpr u32 kMaxIommuDomains = 128;
    IommuDomain    domains_[kMaxIommuDomains]{};
    Atomic<u32>    next_domain_id_{0};

    // Root table (Intel VT-d) / Device table (AMD-Vi) physical address
    PhysAddr       root_table_phys_ = 0;

    // Fault handling
    FaultHandler   fault_handler_ = nullptr;
    u64            total_faults_  = 0;

    Spinlock       lock_{};
};

} // namespace bminos
