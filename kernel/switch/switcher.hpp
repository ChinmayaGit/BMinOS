// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Seamless OS Switcher.
//
// The OsSwitcher orchestrates the transition from one active (foreground)
// domain to another. This is the user-facing "switch OS" operation, typically
// triggered by a hotkey (e.g., Ctrl+Alt+1 for Windows, Ctrl+Alt+2 for Linux).
//
// A switch involves:
//   1. Saving the current domain's GPU state (framebuffer, 3D context)
//   2. Telling the scheduler to rebalance (current -> Background, target -> Active)
//   3. Restoring the target domain's GPU state
//   4. Updating the physical display output
//   5. Optionally playing a transition animation
//
// The switch must be fast — we target < 100ms for a seamless experience.
// GPU state save/restore is the bottleneck; we use dirty-tracking to only
// save modified state.

#pragma once

#include "kernel/common/types.hpp"
#include "kernel/domain/domain.hpp"

namespace bminos {

// Forward declarations
class Scheduler;

// ============================================================================
// Transition animation style
// ============================================================================
enum class SwitchAnimation : u8 {
    Instant,       // No animation — immediate cut (fastest, ~0ms overhead)
    FadeToBlack,   // Brief fade out / fade in (clean, ~50ms)
    SlideLeft,     // Current slides left, new slides in from right
    SlideRight,    // Current slides right, new slides in from left
};

// ============================================================================
// Hotkey binding — maps a key combination to a domain switch
// ============================================================================
struct HotkeyBinding {
    u32      key_combo;     // Encoded modifier + key (e.g., Ctrl+Alt+1)
    DomainId target_domain; // Domain to switch to
    bool     is_active;     // Whether this binding is registered

    // Key combo encoding: upper 16 bits = modifiers, lower 16 bits = keycode
    static constexpr u32 kModCtrl  = 0x0001'0000;
    static constexpr u32 kModAlt   = 0x0002'0000;
    static constexpr u32 kModShift = 0x0004'0000;
    static constexpr u32 kModSuper = 0x0008'0000;
};

// ============================================================================
// Switch statistics — for performance monitoring and optimization
// ============================================================================
struct SwitchStats {
    u64 total_switches;          // Total switches performed
    u64 last_switch_time_us;     // Time of last switch in microseconds
    u64 avg_switch_time_us;      // Running average switch time
    u64 min_switch_time_us;      // Fastest switch observed
    u64 max_switch_time_us;      // Slowest switch observed

    // Breakdown of where time is spent
    u64 last_gpu_save_us;        // GPU state save time
    u64 last_rebalance_us;       // Scheduler rebalance time
    u64 last_gpu_restore_us;     // GPU state restore time
    u64 last_display_update_us;  // Display output update time
};

// ============================================================================
// GPU state snapshot — enough to restore the display for a domain.
//
// Full GPU context save/restore is done by the GPU driver in the management
// domain; here we just track the minimal state needed for display switching.
// ============================================================================
struct GpuState {
    PhysAddr framebuffer_phys;   // Primary framebuffer physical address
    u32      width;              // Display width
    u32      height;             // Display height
    u32      stride;             // Framebuffer stride
    u32      bpp;                // Bits per pixel
    bool     is_valid;           // True if GPU state has been captured
};

// ============================================================================
// OsSwitcher — orchestrates seamless transitions between guest domains.
//
// Usage:
//   1. Register hotkeys: register_hotkey(Ctrl+Alt+1, windows_domain)
//   2. On hotkey press: switch_to(windows_domain)
//   3. Query status: get_active_domain(), get_stats()
//
// The switcher coordinates with the Scheduler and GPU subsystem. It does
// not own any hardware resources directly.
// ============================================================================
class OsSwitcher {
public:
    /// Initialize the OS switcher with references to the scheduler.
    Result<void> init(Scheduler* scheduler);

    /// Switch the active (foreground) domain to the given target.
    /// This is the main entry point, called from hotkey handler or API.
    ///
    /// Steps:
    ///   1. Validate target domain exists and is Running or Paused
    ///   2. Save current GPU state
    ///   3. Rebalance scheduler (current -> Background, target -> Active)
    ///   4. Resume target if Paused
    ///   5. Restore target GPU state
    ///   6. Update display output
    ///   7. Record switch timing
    Result<void> switch_to(DomainId target, SwitchAnimation animation = SwitchAnimation::Instant);

    /// Register a hotkey binding for quick domain switching.
    Result<void> register_hotkey(u32 key_combo, DomainId target_domain);

    /// Unregister a hotkey binding.
    Result<void> unregister_hotkey(u32 key_combo);

    /// Handle a key event — check if it matches a registered hotkey.
    /// Returns true if the key was consumed (triggered a switch).
    bool handle_key_event(u32 key_combo);

    /// Get the currently active (foreground) domain.
    [[nodiscard]] DomainId get_active_domain() const { return active_domain_; }

    /// Get switch performance statistics.
    [[nodiscard]] const SwitchStats& get_stats() const { return stats_; }

    /// Set the default animation style for switches.
    void set_default_animation(SwitchAnimation anim) { default_animation_ = anim; }

    /// Read the platform timer for benchmarking (architecture-specific).
    static u64 read_timestamp_us();

private:
    /// Save the GPU state for the given domain.
    Result<void> save_gpu_state(DomainId domain);

    /// Restore the GPU state for the given domain and update the display.
    Result<void> restore_gpu_state(DomainId domain);

    /// Update switch timing statistics.
    void record_switch_time(u64 elapsed_us);

    // --- State ---

    Scheduler*      scheduler_ = nullptr;
    DomainId        active_domain_ = DomainId::invalid();
    SwitchAnimation default_animation_ = SwitchAnimation::Instant;

    // Per-domain GPU state snapshots (indexed same as domain table)
    static constexpr u32 kMaxDomains = 64;
    GpuState        gpu_states_[kMaxDomains]{};

    // Hotkey bindings
    static constexpr u32 kMaxHotkeys = 16;
    HotkeyBinding   hotkeys_[kMaxHotkeys]{};

    // Statistics
    SwitchStats     stats_{};

    Spinlock        lock_{};
    bool            initialized_ = false;
};

} // namespace bminos
