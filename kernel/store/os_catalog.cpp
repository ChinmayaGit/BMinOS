// BMinOS - Bare Minimum Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// OS Catalog Implementation
// =========================

#include "os_catalog.hpp"
#include "kernel/core/serial.hpp"

namespace bminos::store {

static OsCatalogEntry s_entries[OsCatalog::kMaxEntries];
static u32 s_entry_count = 0;
static bool s_initialized = false;

static void register_entry(const OsCatalogEntry& entry) {
    if (s_entry_count < OsCatalog::kMaxEntries) {
        s_entries[s_entry_count++] = entry;
    }
}

void OsCatalog::init() {
    if (s_initialized) return;
    s_entry_count = 0;

    // 1. Windows 11 Pro 64-bit
    {
        OsCatalogEntry win11{};
        win11.id = OsFlavorId::Windows11_Pro;
        win11.family = OsFamily::Windows;
        win11.name = "Windows 11 Pro (24H2)";
        win11.version = "24H2";
        win11.description = "Microsoft Windows 11 with VirtIO drivers & unattended TPM-bypass configuration";
        win11.primary_download_url = "https://software.download.prss.microsoft.com/db/Win11_24H2_English_x64.iso";
        win11.mirror_download_url = "https://archive.org/download/win-11-24h-2-english-x64/Win11_24H2_English_x64.iso";
        win11.expected_sha256 = "b7a66cf5744dc8bdcaee3a13797686561f95a02484a1e9cbf6e60b72ba4313d3";
        win11.approx_size_bytes = 5700ULL * 1024 * 1024; // ~5.5 GB
        win11.unattended_type = UnattendedType::WindowsUnattendedXml;
        win11.requirements = {
            .min_vcpus = 2, .recommended_vcpus = 4,
            .min_ram_mb = 4096, .recommended_ram_mb = 8192,
            .min_disk_gb = 40, .recommended_disk_gb = 80,
            .requires_tpm2 = false, // BMinOS bypasses TPM requirement
            .requires_secure_boot = false,
            .supports_virtio = true
        };
        win11.is_available = true;
        register_entry(win11);
    }

    // 2. Windows 10 Pro 64-bit
    {
        OsCatalogEntry win10{};
        win10.id = OsFlavorId::Windows10_Pro;
        win10.family = OsFamily::Windows;
        win10.name = "Windows 10 Pro (22H2)";
        win10.version = "22H2";
        win10.description = "Microsoft Windows 10 with lightweight footprint and VirtIO drivers";
        win10.primary_download_url = "https://software.download.prss.microsoft.com/db/Win10_22H2_English_x64.iso";
        win10.mirror_download_url = "https://archive.org/download/win10_22h2_x64/Win10_22H2_English_x64.iso";
        win10.expected_sha256 = "a6f470ca6d33c71382402b69a30b8f02923002e24720bb2fb99e05a1ced66313";
        win10.approx_size_bytes = 4800ULL * 1024 * 1024; // ~4.7 GB
        win10.unattended_type = UnattendedType::WindowsUnattendedXml;
        win10.requirements = {
            .min_vcpus = 2, .recommended_vcpus = 4,
            .min_ram_mb = 2048, .recommended_ram_mb = 4096,
            .min_disk_gb = 30, .recommended_disk_gb = 60,
            .requires_tpm2 = false,
            .requires_secure_boot = false,
            .supports_virtio = true
        };
        win10.is_available = true;
        register_entry(win10);
    }

    // 3. Ubuntu 24.04 LTS Desktop
    {
        OsCatalogEntry ubuntu{};
        ubuntu.id = OsFlavorId::Ubuntu_Desktop_2404;
        ubuntu.family = OsFamily::Linux;
        ubuntu.name = "Ubuntu 24.04 LTS (Noble Numbat)";
        ubuntu.version = "24.04";
        ubuntu.description = "Popular developer-friendly Linux with full Wayland/X11 bridge support";
        ubuntu.primary_download_url = "https://releases.ubuntu.com/24.04/ubuntu-24.04-desktop-amd64.iso";
        ubuntu.mirror_download_url = "https://mirrors.kernel.org/ubuntu-releases/24.04/ubuntu-24.04-desktop-amd64.iso";
        ubuntu.expected_sha256 = "81fae9cc21e2b1b3a9a523e0ff3d13502de67bd4a2b9d5e6d862811856b3071b";
        ubuntu.approx_size_bytes = 5900ULL * 1024 * 1024; // ~5.7 GB
        ubuntu.unattended_type = UnattendedType::LinuxCloudInit;
        ubuntu.requirements = {
            .min_vcpus = 2, .recommended_vcpus = 4,
            .min_ram_mb = 2048, .recommended_ram_mb = 4096,
            .min_disk_gb = 25, .recommended_disk_gb = 50,
            .requires_tpm2 = false,
            .requires_secure_boot = false,
            .supports_virtio = true
        };
        ubuntu.is_available = true;
        register_entry(ubuntu);
    }

    // 4. Debian 12 (Bookworm)
    {
        OsCatalogEntry debian{};
        debian.id = OsFlavorId::Debian_12_Netinst;
        debian.family = OsFamily::Linux;
        debian.name = "Debian 12 (Bookworm) Minimal";
        debian.version = "12.7";
        debian.description = "Rock-solid, ultra-fast bare minimum Linux distribution";
        debian.primary_download_url = "https://cdimage.debian.org/debian-cd/current/amd64/iso-cd/debian-12.7.0-amd64-netinst.iso";
        debian.mirror_download_url = "https://mirrors.edge.kernel.org/debian-cd/current/amd64/iso-cd/debian-12.7.0-amd64-netinst.iso";
        debian.expected_sha256 = "5cfdcf5451a44e5bc114cf3e4f711204d1efc6198ffcff05500ceb05531d0449";
        debian.approx_size_bytes = 650ULL * 1024 * 1024; // ~650 MB
        debian.unattended_type = UnattendedType::LinuxPreseed;
        debian.requirements = {
            .min_vcpus = 1, .recommended_vcpus = 2,
            .min_ram_mb = 512, .recommended_ram_mb = 2048,
            .min_disk_gb = 10, .recommended_disk_gb = 25,
            .requires_tpm2 = false,
            .requires_secure_boot = false,
            .supports_virtio = true
        };
        debian.is_available = true;
        register_entry(debian);
    }

    // 5. Fedora 40 Workstation
    {
        OsCatalogEntry fedora{};
        fedora.id = OsFlavorId::Fedora_40_Workstation;
        fedora.family = OsFamily::Linux;
        fedora.name = "Fedora 40 Workstation";
        fedora.version = "40";
        fedora.description = "Cutting-edge Linux workstation with latest GNOME and kernel features";
        fedora.primary_download_url = "https://download.fedoraproject.org/pub/fedora/linux/releases/40/Workstation/x86_64/iso/Fedora-Workstation-Live-x86_64-40-1.14.iso";
        fedora.mirror_download_url = "https://mirrors.kernel.org/fedora/releases/40/Workstation/x86_64/iso/Fedora-Workstation-Live-x86_64-40-1.14.iso";
        fedora.expected_sha256 = "72ec733e8b0313f86e3f01ee3440bf1d0c4167df3c9428adcaecf4a6b2259160";
        fedora.approx_size_bytes = 2200ULL * 1024 * 1024; // ~2.2 GB
        fedora.unattended_type = UnattendedType::LinuxKickstart;
        fedora.requirements = {
            .min_vcpus = 2, .recommended_vcpus = 4,
            .min_ram_mb = 2048, .recommended_ram_mb = 4096,
            .min_disk_gb = 20, .recommended_disk_gb = 40,
            .requires_tpm2 = false,
            .requires_secure_boot = false,
            .supports_virtio = true
        };
        fedora.is_available = true;
        register_entry(fedora);
    }

    // 6. ChromeOS Flex
    {
        OsCatalogEntry chromeos{};
        chromeos.id = OsFlavorId::ChromeOS_Flex;
        chromeos.family = OsFamily::ChromeOS;
        chromeos.name = "ChromeOS Flex";
        chromeos.version = "128";
        chromeos.description = "Cloud-first, lightweight operating system powered by Google Chrome and web apps";
        chromeos.primary_download_url = "https://dl.google.com/chromeos-flex/images/latest.bin.zip";
        chromeos.mirror_download_url = "https://dl.google.com/dl/edgedl/chromeosflex/chromeos_flex_image.bin.zip";
        chromeos.expected_sha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        chromeos.approx_size_bytes = 1800ULL * 1024 * 1024; // ~1.8 GB
        chromeos.unattended_type = UnattendedType::ChromeOSRawDeploy;
        chromeos.requirements = {
            .min_vcpus = 2, .recommended_vcpus = 2,
            .min_ram_mb = 2048, .recommended_ram_mb = 4096,
            .min_disk_gb = 16, .recommended_disk_gb = 32,
            .requires_tpm2 = false,
            .requires_secure_boot = false,
            .supports_virtio = true
        };
        chromeos.is_available = true;
        register_entry(chromeos);
    }

    // 7. Android-x86
    {
        OsCatalogEntry android{};
        android.id = OsFlavorId::Android_x86_9;
        android.family = OsFamily::Android;
        android.name = "Android-x86 9.0 Pie";
        android.version = "9.0-r2";
        android.description = "Run mobile Android applications with hardware 3D graphics acceleration";
        android.primary_download_url = "https://osdn.net/dl/android-x86/android-x86_64-9.0-r2.iso";
        android.mirror_download_url = "https://archive.org/download/android-x86_64-9.0-r2/android-x86_64-9.0-r2.iso";
        android.expected_sha256 = "c0fa43a5ce390f757279cb175780eb72dfa65f90378031db2c5fe90263309aef";
        android.approx_size_bytes = 920ULL * 1024 * 1024; // ~920 MB
        android.unattended_type = UnattendedType::ManualInstaller;
        android.requirements = {
            .min_vcpus = 2, .recommended_vcpus = 4,
            .min_ram_mb = 2048, .recommended_ram_mb = 4096,
            .min_disk_gb = 16, .recommended_disk_gb = 32,
            .requires_tpm2 = false,
            .requires_secure_boot = false,
            .supports_virtio = true
        };
        android.is_available = true;
        register_entry(android);
    }

    s_initialized = true;
    LOG_INFO("OS Catalog initialized with %u operating systems", s_entry_count);
}

u32 OsCatalog::count() {
    if (!s_initialized) init();
    return s_entry_count;
}

const OsCatalogEntry* OsCatalog::get_by_index(u32 index) {
    if (!s_initialized) init();
    if (index >= s_entry_count) return nullptr;
    return &s_entries[index];
}

const OsCatalogEntry* OsCatalog::find(OsFlavorId id) {
    if (!s_initialized) init();
    for (u32 i = 0; i < s_entry_count; ++i) {
        if (s_entries[i].id == id) return &s_entries[i];
    }
    return nullptr;
}

u32 OsCatalog::filter_by_family(OsFamily family, const OsCatalogEntry* out_entries[], u32 max_out) {
    if (!s_initialized) init();
    u32 matched = 0;
    for (u32 i = 0; i < s_entry_count && matched < max_out; ++i) {
        if (s_entries[i].family == family) {
            out_entries[matched++] = &s_entries[i];
        }
    }
    return matched;
}

bool OsCatalog::meets_requirements(OsFlavorId id, u32 available_vcpus, u64 available_ram_mb, u64 available_disk_gb) {
    const auto* entry = find(id);
    if (!entry) return false;
    return (available_vcpus >= entry->requirements.min_vcpus) &&
           (available_ram_mb >= entry->requirements.min_ram_mb) &&
           (available_disk_gb >= entry->requirements.min_disk_gb);
}

} // namespace bminos::store

