// BMinOS - Bare Minimum Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Automated OS Installer Subsystem
// =================================
// Automates the complete lifecycle of creating and installing guest operating
// systems without requiring manual user intervention:
//   1. Creates raw or sparse virtual disk image (or formats USB pendrive target)
//   2. Generates unattended installer answer files:
//      - Windows: autounattend.xml (bypasses Win11 hardware restrictions & sets up VirtIO)
//      - Linux: cloud-init / preseed / kickstart for zero-touch installation
//      - ChromeOS Flex: automated disk flash & partition resizing
//   3. Creates the domain profile (.toml), assigns vCPUs, RAM, and registers with Scheduler
//   4. Boots guest installer and monitors installation progress via serial console / VirtIO

#pragma once

#include "kernel/common/types.hpp"
#include "kernel/domain/domain.hpp"
#include "os_catalog.hpp"

namespace bminos::store {

// =============================================================================
// Installation Target
// =============================================================================

enum class InstallMedium : u8 {
    VirtualDiskFile,     // Create virtual disk (.raw / .qcow2) in local storage
    PhysicalUsbPendrive, // Direct install/burn to a connected USB pendrive
};

struct InstallConfig {
    OsFlavorId      flavor_id;
    FixedString<64> domain_name;
    u32             vcpus;
    u64             ram_mb;
    u64             disk_size_gb;
    InstallMedium   medium = InstallMedium::VirtualDiskFile;
    FixedString<128> target_path;      // e.g. "/disks/win11.raw" or "/dev/usb0"
    FixedString<64>  admin_username;   // Default user name
    FixedString<64>  admin_password;   // Default user password
    bool            enable_bridge;     // Enable cross-OS bridge agent inside guest
    bool            auto_start_on_boot;// Automatically start VM when BMinOS starts
};

// =============================================================================
// Installation Progress State
// =============================================================================

enum class InstallStage : u8 {
    Idle,
    DownloadingIso,
    VerifyingChecksum,
    CreatingDiskImage,
    GeneratingAnswerFile,
    ConfiguringDomain,
    BootingInstaller,
    InstallingOs,
    InjectingVirtIoDrivers,
    Complete,
    Failed,
};

struct InstallProgress {
    InstallStage    stage;
    u8              percent_complete;
    FixedString<128> status_message;
    u64             bytes_transferred;
    u64             total_bytes;
    u32             transfer_rate_kbps;
    DomainId        created_domain_id;
};

// =============================================================================
// Auto-Installer Engine
// =============================================================================

class AutoInstaller {
public:
    static constexpr u32 kMaxXmlBufferSize = 16384;

    /// Initialize the installer engine
    static void init();

    /// Start automated installation for a catalog OS
    static Result<DomainId> start_install(const InstallConfig& config);

    /// Query current progress
    static InstallProgress get_progress();

    /// Cancel active installation
    static void cancel();

    // --- Answer file generators ---

    /// Generate Windows autounattend.xml string (bypasses TPM/RAM/CPU, injects VirtIO)
    static u32 generate_windows_unattend_xml(const InstallConfig& config, char* out_buf, u32 max_len);

    /// Generate Linux cloud-init user-data file
    static u32 generate_linux_cloud_init(const InstallConfig& config, char* out_buf, u32 max_len);

    /// Generate Linux preseed configuration
    static u32 generate_linux_preseed(const InstallConfig& config, char* out_buf, u32 max_len);

    /// Generate domain TOML config string
    static u32 generate_domain_toml(const InstallConfig& config, char* out_buf, u32 max_len);
};

} // namespace bminos::store

