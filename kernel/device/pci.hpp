// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// PCI/PCIe Bus Enumeration and Management.
//
// Provides PCI configuration space access and device discovery. This is
// essential for the hypervisor because:
//   1. We need to find GPUs, NICs, storage controllers for passthrough
//   2. IOMMU setup requires knowledge of PCI topology
//   3. Device assignment to domains needs BDF (Bus:Device:Function) info
//
// PCI config space access uses the legacy I/O port mechanism (ports 0xCF8/0xCFC)
// for initial enumeration, and MMIO (ECAM) for PCIe extended config space.

#pragma once

#include "kernel/common/types.hpp"
#include "kernel/domain/domain.hpp"

namespace bminos {

// ============================================================================
// PCI device class codes — identifies the type of device
// ============================================================================
enum class PciClassCode : u8 {
    Unclassified     = 0x00,
    MassStorage      = 0x01,
    Network          = 0x02,
    Display          = 0x03,
    Multimedia       = 0x04,
    Memory           = 0x05,
    Bridge           = 0x06,
    Communication    = 0x07,
    SystemPeripheral = 0x08,
    Input            = 0x09,
    Processor        = 0x0B,
    SerialBus        = 0x0C,  // USB, FireWire, etc.
    Wireless         = 0x0D,
    Other            = 0xFF,
};

// ============================================================================
// PCI Base Address Register (BAR) descriptor
// ============================================================================
struct PciBar {
    enum class Type : u8 {
        None,
        Memory32,    // 32-bit MMIO
        Memory64,    // 64-bit MMIO (consumes two BAR slots)
        IoPort,      // I/O port space (legacy)
    } type = Type::None;

    PhysAddr base_address = 0;  // Physical base address (or I/O port number)
    u64      size         = 0;  // Size of the BAR region in bytes
    bool     prefetchable = false;
};

// ============================================================================
// PCI device descriptor — populated during bus enumeration
// ============================================================================
struct PciDevice {
    // BDF (Bus:Device:Function) — unique identifier in PCI topology
    u16 bus      = 0;
    u8  device   = 0;
    u8  function = 0;

    // Identification
    u16 vendor_id     = 0xFFFF;  // 0xFFFF = no device present
    u16 device_id     = 0xFFFF;
    u16 subsystem_vendor_id = 0;
    u16 subsystem_device_id = 0;
    u8  revision_id   = 0;

    // Classification
    PciClassCode class_code    = PciClassCode::Unclassified;
    u8           subclass      = 0;
    u8           prog_interface = 0;

    // Header type
    u8  header_type   = 0;
    bool is_multifunction = false;

    // BARs (up to 6 for type 0 headers)
    static constexpr u32 kMaxBars = 6;
    PciBar bars[kMaxBars]{};

    // Interrupt configuration
    u8  interrupt_line = 0;  // Legacy PIC IRQ
    u8  interrupt_pin  = 0;  // INTA#-INTD# (1-4, 0=none)
    bool has_msi       = false;
    bool has_msix      = false;

    // Domain assignment (DomainId::invalid() = unassigned / host-owned)
    DomainId assigned_domain = DomainId::invalid();

    /// Check if this device slot is populated.
    [[nodiscard]] bool is_present() const { return vendor_id != 0xFFFF; }

    /// Get a human-readable class description.
    [[nodiscard]] const char* class_name() const;
};

// ============================================================================
// PciManager — enumerates and manages all PCI/PCIe devices.
//
// Usage:
//   PciManager pci;
//   pci.init();
//   pci.enumerate();
//   auto* gpu = pci.find_by_class(PciClassCode::Display);
//   pci.assign_to_domain(*gpu, windows_domain_id);
//
// The manager maintains a flat list of discovered devices. A production
// implementation would also build the PCI topology tree for proper bridge
// handling and IOMMU group detection.
// ============================================================================
class PciManager {
public:
    /// Initialize PCI config space access.
    /// Detects whether ECAM (PCIe MMIO) is available via ACPI/MCFG.
    Result<void> init();

    /// Enumerate all PCI buses and discover devices.
    /// Scans bus 0-255, device 0-31, function 0-7.
    /// For efficiency, only scans function > 0 if the device is multi-function.
    Result<void> enumerate();

    // --- Config space access ---

    /// Read a 32-bit value from PCI configuration space.
    /// Uses I/O ports (0xCF8/0xCFC) or ECAM MMIO depending on availability.
    u32 read_config(u16 bus, u8 dev, u8 func, u8 offset);

    /// Write a 32-bit value to PCI configuration space.
    void write_config(u16 bus, u8 dev, u8 func, u8 offset, u32 value);

    // --- Device lookup ---

    /// Find a device by vendor and device ID. Returns nullptr if not found.
    /// If multiple devices match, returns the first one.
    PciDevice* find_device(u16 vendor_id, u16 device_id);

    /// Find all devices of a given class. Returns the count found.
    u32 find_by_class(PciClassCode class_code, PciDevice** out_devices, u32 max_count);

    /// Find a device by BDF address.
    PciDevice* find_by_bdf(u16 bus, u8 device, u8 function);

    // --- Device assignment ---

    /// Assign a PCI device to a domain for passthrough.
    /// This updates the device's assignment record and prepares it for
    /// IOMMU mapping (actual IOMMU setup is done by IommuManager).
    Result<void> assign_to_domain(PciDevice& device, DomainId domain_id);

    /// Reclaim a device from a domain back to the host.
    Result<void> unassign_from_domain(PciDevice& device);

    // --- Accessors ---

    /// Total number of discovered devices.
    [[nodiscard]] u32 device_count() const { return device_count_; }

    /// Get a device by index (0-based).
    [[nodiscard]] PciDevice* device(u32 index);
    [[nodiscard]] const PciDevice* device(u32 index) const;

private:
    /// Scan a single device/function and populate the device table.
    void scan_device(u16 bus, u8 device, u8 function);

    /// Read and decode BAR registers for a device.
    void read_bars(PciDevice& dev);

    /// Check for MSI/MSI-X capability.
    void detect_msi(PciDevice& dev);

    /// Walk the PCI capability list.
    u8 find_capability(u16 bus, u8 dev, u8 func, u8 cap_id);

    // --- I/O port access (x86 only) ---
    static void io_outl(u16 port, u32 value);
    static u32  io_inl(u16 port);

    // --- Device table ---
    static constexpr u32 kMaxDevices = 512;
    PciDevice devices_[kMaxDevices]{};
    u32       device_count_ = 0;

    // ECAM (PCIe MMIO config space) base address — 0 if not available
    PhysAddr  ecam_base_ = 0;
    u16       ecam_start_bus_ = 0;
    u16       ecam_end_bus_ = 0;

    bool      initialized_ = false;
    Spinlock  lock_{};
};

} // namespace bminos
