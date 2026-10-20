// BMinOS - Bare Minimum Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Dom0 Management Browser & Download Portal
// ==========================================
// Embedded lightweight web browser and OS download portal built directly
// into BMinOS Dom0 (Management Domain).
//
// Capabilities:
//   1. Browse official OS distributor sites (Microsoft, Ubuntu, Fedora, ChromeOS, Debian)
//   2. Enter custom URLs to download any custom Linux distro, Windows ISO, or appliance image
//   3. Intercepts .iso, .img, .bin.zip download links with a native dialog:
//      [Auto-Install into VM]  or  [Burn directly to USB Pendrive]
//   4. Shows real-time streaming download speeds, ETA, and progress bar
//   5. Bookmark bar with 1-click access to verified OS repositories

#pragma once

#include "kernel/common/types.hpp"
#include "kernel/store/os_catalog.hpp"
#include "kernel/net/downloader.hpp"

namespace bminos::ui {

struct Bookmark {
    FixedString<32>  title;
    FixedString<128> url;
    store::OsFlavorId linked_flavor;
};

class ManagementBrowser {
public:
    static constexpr u32 kMaxBookmarks = 16;
    static constexpr u32 kMaxHistory   = 32;

    /// Initialize the browser subsystem with preloaded bookmarks
    static void init();

    /// Navigate to a URL
    static void navigate_to(const char* url);

    /// Go back in history
    static void go_back();

    /// Go forward in history
    static void go_forward();

    /// Get current active URL
    static const char* current_url();

    /// Get current page title
    static const char* current_title();

    /// Bookmark management
    static u32 bookmark_count();
    static const Bookmark* get_bookmark(u32 index);
    static void add_bookmark(const char* title, const char* url, store::OsFlavorId flavor = store::OsFlavorId::Custom_Iso);

    /// Action triggered when user clicks an ISO link in the browser
    enum class DownloadAction : u8 {
        AutoInstallToNewVm,
        BurnToUsbPendrive,
        SaveToDiskOnly,
    };

    /// Handle intercepted download request
    static Result<void> handle_download_request(const char* iso_url, DownloadAction action, const char* pendrive_target = nullptr);

    /// Render browser UI into display framebuffer
    static void render(u32* framebuffer, u32 width, u32 height, u32 stride);
};

} // namespace bminos::ui

