// BMinOS - Bare Minimum Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Dom0 Management Browser Implementation
// =======================================

#include "browser.hpp"
#include "kernel/core/serial.hpp"
#include "kernel/store/auto_installer.hpp"
#include "kernel/net/downloader.hpp"

namespace bminos::ui {

static Bookmark s_bookmarks[ManagementBrowser::kMaxBookmarks];
static u32 s_bookmark_count = 0;
static FixedString<256> s_current_url = "https://bminos.local/store";
static FixedString<64>  s_current_title = "BMinOS — OS Download Portal";
static FixedString<256> s_history[ManagementBrowser::kMaxHistory];
static u32 s_history_count = 0;
static u32 s_history_index = 0;
static bool s_initialized = false;

void ManagementBrowser::init() {
    if (s_initialized) return;

    s_bookmark_count = 0;
    add_bookmark("OS Catalog", "https://bminos.local/store", store::OsFlavorId::Custom_Iso);
    add_bookmark("Microsoft Windows", "https://www.microsoft.com/software-download/", store::OsFlavorId::Windows11_Pro);
    add_bookmark("Ubuntu Linux", "https://ubuntu.com/download/desktop", store::OsFlavorId::Ubuntu_Desktop_2404);
    add_bookmark("Debian Netinst", "https://www.debian.org/distrib/", store::OsFlavorId::Debian_12_Netinst);
    add_bookmark("Fedora Workstation", "https://fedoraproject.org/workstation/download/", store::OsFlavorId::Fedora_40_Workstation);
    add_bookmark("ChromeOS Flex", "https://chromeenterprise.google/os/chromeosflex/", store::OsFlavorId::ChromeOS_Flex);
    add_bookmark("Arch Linux", "https://archlinux.org/download/", store::OsFlavorId::ArchLinux_Latest);
    add_bookmark("Android-x86", "https://www.android-x86.org/download", store::OsFlavorId::Android_x86_9);

    s_current_url = "https://bminos.local/store";
    s_current_title = "BMinOS — OS Download Portal";

    s_history[0] = s_current_url;
    s_history_count = 1;
    s_history_index = 0;

    s_initialized = true;
    LOG_INFO("Dom0 Management Browser initialized with %u bookmarks", s_bookmark_count);
}

void ManagementBrowser::add_bookmark(const char* title, const char* url, store::OsFlavorId flavor) {
    if (s_bookmark_count < kMaxBookmarks) {
        s_bookmarks[s_bookmark_count].title = title;
        s_bookmarks[s_bookmark_count].url = url;
        s_bookmarks[s_bookmark_count].linked_flavor = flavor;
        s_bookmark_count++;
    }
}

u32 ManagementBrowser::bookmark_count() {
    if (!s_initialized) init();
    return s_bookmark_count;
}

const Bookmark* ManagementBrowser::get_bookmark(u32 index) {
    if (!s_initialized) init();
    if (index >= s_bookmark_count) return nullptr;
    return &s_bookmarks[index];
}

void ManagementBrowser::navigate_to(const char* url) {
    if (!url) return;
    s_current_url = url;
    s_current_title = url;

    if (s_history_count < kMaxHistory) {
        s_history[s_history_count++] = url;
        s_history_index = s_history_count - 1;
    }

    LOG_INFO("Browser: Navigated to '%s'", url);
}

void ManagementBrowser::go_back() {
    if (s_history_index > 0) {
        s_history_index--;
        s_current_url = s_history[s_history_index];
        s_current_title = s_current_url.c_str();
        LOG_INFO("Browser: Back -> '%s'", s_current_url.c_str());
    }
}

void ManagementBrowser::go_forward() {
    if (s_history_index + 1 < s_history_count) {
        s_history_index++;
        s_current_url = s_history[s_history_index];
        s_current_title = s_current_url.c_str();
        LOG_INFO("Browser: Forward -> '%s'", s_current_url.c_str());
    }
}

const char* ManagementBrowser::current_url() {
    return s_current_url.c_str();
}

const char* ManagementBrowser::current_title() {
    return s_current_title.c_str();
}

Result<void> ManagementBrowser::handle_download_request(const char* iso_url, DownloadAction action, const char* pendrive_target) {
    if (!iso_url) return Result<void>::error(Status::InvalidArgument);

    LOG_INFO("Browser intercepted OS image download: '%s'", iso_url);

    if (action == DownloadAction::AutoInstallToNewVm) {
        LOG_INFO("Action: Auto-installing OS into new VM...");
        // Start streaming ISO to disk storage
        net::Downloader::start(iso_url, "/disks/installer.iso");

        // Kick off AutoInstaller
        store::InstallConfig cfg{};
        cfg.flavor_id = store::OsFlavorId::Ubuntu_Desktop_2404; // default or matched flavor
        cfg.domain_name = "New-OS-VM";
        cfg.vcpus = 4;
        cfg.ram_mb = 4096;
        cfg.disk_size_gb = 50;
        cfg.enable_bridge = true;
        cfg.target_path = "/disks/vm_root.raw";
        return store::AutoInstaller::start_install(cfg).is_ok() ? Result<void>::ok() : Result<void>::error(Status::InternalError);
    } else if (action == DownloadAction::BurnToUsbPendrive) {
        const char* target = pendrive_target ? pendrive_target : "/dev/usb_pendrive0";
        LOG_INFO("Action: Streaming OS ISO directly to USB Pendrive: '%s'...", target);
        return net::Downloader::start(iso_url, target);
    } else {
        return net::Downloader::start(iso_url, "/disks/downloaded_os.iso");
    }
}

void ManagementBrowser::render(u32* framebuffer, u32 width, u32 height, u32 stride) {
    if (!framebuffer || width == 0 || height == 0) return;
    // Renders address bar, tab header, bookmark bar, and download tray
    // Framebuffer color format: 0xAARRGGBB
    // Top 48 pixels: Navigation and URL bar
    u32 bar_color = 0xFF202225; // Dark slate
    for (u32 y = 0; y < 48 && y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            framebuffer[y * stride + x] = bar_color;
        }
    }
}

} // namespace bminos::ui

