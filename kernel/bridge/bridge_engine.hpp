// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Cross-OS Bridge Engine.
//
// The Bridge Engine enables seamless interaction between guest operating systems.
// It allows applications from one OS to be "bridged" into another OS's
// environment — for example, running a Windows application that appears as
// a native window within a Linux desktop, or sharing clipboard contents
// between macOS and Windows guests.
//
// Bridge modes:
//   - RemoteExecution: The app runs in its native domain, display/input are
//     forwarded to the active domain via shared memory and virtual GPU.
//   - ApiTranslation: System calls are intercepted and translated between
//     OS APIs (experimental, limited compatibility).
//   - Disabled: No cross-domain interaction allowed.

#pragma once

#include "kernel/common/types.hpp"
#include "kernel/domain/domain.hpp"

namespace bminos {

// ============================================================================
// Bridge operation mode
// ============================================================================
enum class BridgeMode : u8 {
    RemoteExecution,   // Forward display/input across domains (reliable)
    ApiTranslation,    // Translate API calls (experimental)
    Disabled,          // Bridging not available between these domains
};

// ============================================================================
// Bridge session states
// ============================================================================
enum class BridgeSessionState : u8 {
    Connecting,    // Negotiating capabilities between domains
    Active,        // Bridge is operational, forwarding data
    Suspended,     // Temporarily paused (e.g., domain suspended)
    Closed,        // Session terminated
};

// ============================================================================
// Capabilities that can be shared between two domains.
// Determined by the isolation levels and OS types of both domains.
// ============================================================================
struct BridgeCapability {
    bool can_execute;            // Can run apps from the other domain
    bool can_share_clipboard;    // Clipboard sharing enabled
    bool can_share_filesystem;   // Filesystem sharing (read-only or read-write)
    bool can_share_audio;        // Audio routing between domains
    bool can_forward_display;    // Display frame forwarding
    bool can_forward_input;      // Input event forwarding (keyboard, mouse)
};

// ============================================================================
// Input event forwarded from the active domain to a bridged application
// ============================================================================
struct InputEvent {
    enum class Type : u8 {
        KeyDown,
        KeyUp,
        MouseMove,
        MouseButtonDown,
        MouseButtonUp,
        MouseScroll,
        TouchBegin,
        TouchMove,
        TouchEnd,
    } type;

    union {
        struct { u32 keycode; u32 modifiers; } key;
        struct { i32 x; i32 y; u32 button; }  mouse;
        struct { i32 dx; i32 dy; }             scroll;
        struct { u32 touch_id; i32 x; i32 y; } touch;
    } data;

    u64 timestamp_ns;  // Monotonic timestamp for latency tracking
};

// ============================================================================
// Display frame produced by a bridged application
// ============================================================================
struct DisplayFrame {
    PhysAddr   framebuffer_phys;   // Physical address of the frame data
    u32        width;              // Frame width in pixels
    u32        height;             // Frame height in pixels
    u32        stride;             // Bytes per row
    u64        timestamp_ns;       // When the frame was produced
    bool       is_valid;           // False if no frame is available

    enum class Format : u8 {
        BGRA8888,     // 32-bit BGRA (common on x86)
        RGBA8888,     // 32-bit RGBA
        RGB565,       // 16-bit RGB (low memory)
    } format = Format::BGRA8888;
};

// ============================================================================
// Application identification for bridge requests
// ============================================================================
struct AppInfo {
    FixedString<128> name;          // Application name / path
    FixedString<64>  window_title;  // Desired window title in target domain
    u32              requested_width  = 1280;
    u32              requested_height = 720;
};

// ============================================================================
// Bridge session identifier
// ============================================================================
struct BridgeSessionIdTag {};
using BridgeSessionId = StrongId<BridgeSessionIdTag, u32>;

// ============================================================================
// BridgeSession — represents an active bridge between two domains for a
// specific application.
//
// The session manages the lifecycle of a bridged application:
//   1. Connecting: negotiate capabilities, allocate shared memory
//   2. Active: forward input events, receive display frames
//   3. Suspended: domain paused, buffer state preserved
//   4. Closed: clean up resources
// ============================================================================
class BridgeSession {
public:
    [[nodiscard]] BridgeSessionId  id() const            { return id_; }
    [[nodiscard]] DomainId         source_domain() const { return source_domain_; }
    [[nodiscard]] DomainId         target_domain() const { return target_domain_; }
    [[nodiscard]] BridgeSessionState state() const       { return state_; }
    [[nodiscard]] BridgeMode       mode() const          { return mode_; }
    [[nodiscard]] const AppInfo&   app_info() const      { return app_info_; }

    /// Send an input event from the active domain to the bridged application.
    Result<void> send_input(const InputEvent& event);

    /// Receive the latest display frame from the bridged application.
    /// Returns a frame with is_valid=false if no new frame is available.
    DisplayFrame receive_frame();

    /// Suspend the bridge session (e.g., when a domain is paused).
    Result<void> suspend();

    /// Resume a suspended session.
    Result<void> resume();

    /// Close the session and release resources.
    Result<void> close();

private:
    friend class BridgeEngine;

    BridgeSessionId    id_             = BridgeSessionId::invalid();
    DomainId           source_domain_  = DomainId::invalid();
    DomainId           target_domain_  = DomainId::invalid();
    BridgeSessionState state_          = BridgeSessionState::Connecting;
    BridgeMode         mode_           = BridgeMode::Disabled;
    AppInfo            app_info_{};

    // Shared memory region for frame data
    PhysAddr           shared_buffer_phys_ = 0;
    u64                shared_buffer_size_ = 0;

    // IPC channel ID for event/frame communication
    u32                ipc_channel_id_ = 0;

    // Statistics
    u64                frames_forwarded_ = 0;
    u64                input_events_forwarded_ = 0;
    u64                last_frame_latency_ns_ = 0;
};

// ============================================================================
// BridgeEngine — manages cross-domain bridge sessions.
//
// The engine is responsible for:
//   - Determining bridge capabilities between domain pairs
//   - Creating and managing bridge sessions
//   - Allocating shared memory for frame transfer
//   - Setting up IPC channels for event forwarding
//
// Thread safety: The engine uses a lock to protect the session table.
// Individual sessions are only accessed by their source/target domains.
// ============================================================================
class BridgeEngine {
public:
    /// Initialize the bridge engine.
    Result<void> init();

    /// Query what bridging capabilities exist between two domains.
    /// This depends on both domains' isolation levels and OS types.
    BridgeCapability query_capabilities(DomainId source, DomainId target);

    /// Request a new bridge session between two domains for a specific app.
    /// The source domain is where the user sees the bridged window.
    /// The target domain is where the application actually runs.
    Result<BridgeSession*> request_bridge(DomainId source, DomainId target,
                                          const AppInfo& app);

    /// Close a bridge session and free its resources.
    Result<void> close_session(BridgeSessionId session_id);

    /// Close all sessions involving a specific domain (called on domain destroy).
    void close_all_sessions_for(DomainId domain_id);

    /// Get a session by ID.
    [[nodiscard]] BridgeSession* find_session(BridgeSessionId id);

    /// Get the number of active sessions.
    [[nodiscard]] u32 active_session_count() const { return active_count_; }

private:
    static constexpr u32 kMaxSessions = 128;
    BridgeSession    sessions_[kMaxSessions]{};
    Atomic<u32>      next_session_id_{0};
    u32              active_count_ = 0;
    Spinlock         lock_{};
    bool             initialized_ = false;
};

} // namespace bminos
