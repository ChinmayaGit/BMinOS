// BMinOS - Bare Minimum Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// USB Mass Storage & Pendrive Management Subsystem
// =================================================
// Supports USB flash drives, thumb drives, and external USB disks via
// USB Mass Storage Class (Bulk-Only Transport / SCSI Transparent Command Set).
//
// Key Features:
//   1. Pendrive Hot-Plug Detection: Automatically detects when a USB drive is inserted
//   2. Boot/Install from Pendrive: Scans USB drive for bootable OS (ISO/EFI/MBR) and boots it
//   3. Pendrive Flasher / Burner: Writes downloaded ISO images directly to USB media
//   4. Seamless Guest Assignment: Dynamically assigns/passes through USB drive to Windows or Linux guest

#pragma once

#include "kernel/common/types.hpp"
#include "kernel/domain/domain.hpp"

namespace bminos::device {

// =============================================================================
// USB Pendrive Descriptor
// =============================================================================

struct UsbPendrive {
    u32             drive_id;          // Unique drive index (0, 1, ...)
    FixedString<32> vendor_name;       // e.g. "SanDisk", "Kingston", "Samsung"
    FixedString<32> product_name;      // e.g. "Ultra USB 3.0", "DataTraveler"
    FixedString<32> serial_number;
    u64             capacity_bytes;    // Total drive capacity
    u32             sector_size;       // Typically 512 or 4096 bytes
    u64             total_sectors;     // capacity_bytes / sector_size
    bool            is_inserted;       // True if physically plugged in
    bool            is_bootable;       // True if MBR/GPT has valid bootloader/ISO filesystem
    DomainId        assigned_domain;   // Assigned guest domain, or invalid() if host-owned
};

// =============================================================================
// USB Pendrive Manager
// =============================================================================

class UsbStorageManager {
public:
    static constexpr u32 kMaxPendrives = 8;

    /// Initialize the USB Mass Storage subsystem
    static void init();

    /// Scan USB ports and enumerate connected pendrives
    static u32 scan_pendrives();

    /// Total number of currently connected pendrives
    static u32 count();

    /// Get pendrive info by index
    static const UsbPendrive* get_drive(u32 drive_index);

    /// Read sectors from pendrive
    static Result<void> read_sectors(u32 drive_id, u64 lba_start, u32 sector_count, void* buffer);

    /// Write sectors to pendrive (used for flashing ISOs)
    static Result<void> write_sectors(u32 drive_id, u64 lba_start, u32 sector_count, const void* buffer);

    /// Check if pendrive contains a bootable OS installation media
    static bool detect_bootable_media(u32 drive_id);

    /// 1-Click: Create a new guest VM booting directly from the inserted USB pendrive
    static Result<DomainId> boot_from_pendrive(u32 drive_id, const char* vm_name = "USB-Boot-VM");

    /// 1-Click: Flash / Burn an ISO image directly to the selected USB pendrive
    static Result<void> flash_iso_to_pendrive(u32 drive_id, const char* iso_path,
                                              void (*progress_callback)(u8 percent, u64 written_bytes));

    /// Dynamic Passthrough: Hand off the physical pendrive to a running guest OS (Windows/Linux)
    static Result<void> attach_to_domain(u32 drive_id, DomainId domain_id);

    /// Detach pendrive from guest OS back to BMinOS host
    static Result<void> detach_from_domain(u32 drive_id);
};

} // namespace bminos::device

