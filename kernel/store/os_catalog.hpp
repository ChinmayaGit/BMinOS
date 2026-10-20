// BMinOS - Bare Minimum Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// OS Catalog Subsystem
// ====================
// Curated catalog of operating systems with metadata, hardware requirements,
// official ISO download mirrors, and unattended installation profiles.
//
// Supported OSes out of the box:
//   - Windows 11 24H2 / Windows 10 22H2 (Pro / Home)
//   - Ubuntu 24.04 LTS (Noble Numbat) / Ubuntu Server
//   - Debian 12 (Bookworm)
//   - Fedora 40 Workstation
//   - Arch Linux (Rolling)
//   - ChromeOS Flex (CloudReady / ChromeOS Flex)
//   - Android-x86 / Bliss OS
//   - Custom user-provided ISO or USB pendrive

#pragma once

#include "kernel/common/types.hpp"
#include "kernel/domain/domain.hpp"

namespace bminos::store {

// =============================================================================
// OS Identifiers
// =============================================================================

enum class OsFamily : u8 {
    Windows,
    Linux,
    ChromeOS,
    Android,
    Custom,
};

enum class OsFlavorId : u32 {
    Windows11_Pro        = 101,
    Windows11_Home       = 102,
    Windows10_Pro        = 103,
    Ubuntu_Desktop_2404  = 201,
    Ubuntu_Server_2404   = 202,
    Debian_12_Netinst    = 203,
    Fedora_40_Workstation= 204,
    ArchLinux_Latest     = 205,
    ChromeOS_Flex        = 301,
    Android_x86_9        = 401,
    Custom_Iso           = 999,
};

// =============================================================================
// Unattended Installation Mode
// =============================================================================

enum class UnattendedType : u8 {
    WindowsUnattendedXml,  // Autounattend.xml (bypasses Win11 TPM/RAM/CPU & online acct)
    LinuxCloudInit,        // cloud-init user-data / meta-data
    LinuxPreseed,          // Debian preseed.cfg
    LinuxKickstart,        // Fedora/RHEL kickstart.ks
    ChromeOSRawDeploy,     // Raw disk image expansion
    ManualInstaller,       // User interacts with installer GUI
};

// =============================================================================
// OS Hardware Profile Recommendations
// =============================================================================

struct OsRequirements {
    u32 min_vcpus;
    u32 recommended_vcpus;
    u64 min_ram_mb;
    u64 recommended_ram_mb;
    u64 min_disk_gb;
    u64 recommended_disk_gb;
    bool requires_tpm2;
    bool requires_secure_boot;
    bool supports_virtio;
};

// =============================================================================
// OS Catalog Entry
// =============================================================================

struct OsCatalogEntry {
    OsFlavorId      id;
    OsFamily        family;
    FixedString<64> name;
    FixedString<32> version;
    FixedString<128> description;
    FixedString<256> primary_download_url;
    FixedString<256> mirror_download_url;
    FixedString<68>  expected_sha256;      // Hex string of SHA256 (64 chars)
    u64             approx_size_bytes;     // Expected ISO/Image file size
    UnattendedType  unattended_type;
    OsRequirements  requirements;
    bool            is_available;          // Verified working mirror
};

// =============================================================================
// OS Catalog Manager
// =============================================================================

class OsCatalog {
public:
    static constexpr u32 kMaxEntries = 32;

    /// Initialize default catalog entries
    static void init();

    /// Get total number of registered OS catalog entries
    static u32 count();

    /// Retrieve entry by index (0-based)
    static const OsCatalogEntry* get_by_index(u32 index);

    /// Retrieve entry by flavor ID
    static const OsCatalogEntry* find(OsFlavorId id);

    /// Filter OSes by family (e.g. all Windows or all Linux)
    static u32 filter_by_family(OsFamily family, const OsCatalogEntry* out_entries[], u32 max_out);

    /// Check if host hardware can run this OS
    static bool meets_requirements(OsFlavorId id, u32 available_vcpus, u64 available_ram_mb, u64 available_disk_gb);
};

} // namespace bminos::store

