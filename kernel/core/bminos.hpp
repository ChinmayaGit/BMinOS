// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// kernel/core/bminos.hpp — Main hypervisor singleton class.
// The Nexus class is the central orchestrator of the entire hypervisor.
// It owns all subsystem references and manages the lifecycle from boot
// through shutdown. Architecture-specific details are hidden behind
// compile-time polymorphism, not virtual dispatch — we can't afford
// vtable overhead in the hot path.

#pragma once

#include "types.hpp"
#include "serial.hpp"

namespace bminos {

// Forward declarations for subsystem managers.
// Each of these is defined in its own header and manages one aspect
// of the hypervisor's functionality.
class MemoryManager;
class Scheduler;
class DeviceManager;
class BridgeEngine;
class DomainManager;

// ============================================================================
// Version Information
// ============================================================================

struct VersionInfo {
    u16 major;
    u16 minor;
    u16 patch;
    const char* codename;
    const char* build_date;
    const char* build_time;
    const char* git_hash;  // Populated by build system, nullptr if unavailable
};

/// Current version of the hypervisor.
inline constexpr VersionInfo BMINOS_VERSION = {
    .major      = 0,
    .minor      = 1,
    .patch      = 0,
    .codename   = "Genesis",
    .build_date = __DATE__,
    .build_time = __TIME__,
    .git_hash   = nullptr,
};

// ============================================================================
// Architecture Type
// ============================================================================

/// Runtime-queryable architecture identifier.
/// While we use compile-time #if for code selection, this enum is useful
/// for logging, configuration, and any runtime decisions that depend on
/// which architecture we're running on.
enum class ArchType : u8 {
    X86_64 = 0,
    ARM64  = 1,
};

/// Returns a human-readable name for the architecture.
constexpr const char* arch_name(ArchType arch) {
    switch (arch) {
        case ArchType::X86_64: return "x86_64";
        case ArchType::ARM64:  return "AArch64";
    }
    return "Unknown";
}

// ============================================================================
// Virtualization Capabilities
// ============================================================================

/// Describes what hardware virtualization features are available.
/// Populated during early boot by detect_virtualization_support().
struct VirtCapabilities {
    bool supported;           // Is any HW virtualization available?
    bool enabled;             // Is it currently enabled (e.g., in BIOS)?

#if BMINOS_ARCH_X86_64
    bool vmx;                 // Intel VT-x
    bool svm;                 // AMD-V (SVM)
    bool ept;                 // Extended Page Tables (Intel)
    bool npt;                 // Nested Page Tables (AMD)
    bool vpid;                // Virtual Processor Identifiers (Intel)
    bool unrestricted_guest;  // Allow real-mode guests (Intel)
#elif BMINOS_ARCH_ARM64
    bool vhe;                 // Virtualization Host Extensions (ARMv8.1)
    bool stage2;              // Stage-2 translation support
    bool gicv3;               // GICv3 virtualization support
    bool sve;                 // Scalable Vector Extension support
#endif
};

// ============================================================================
// Boot Information
// ============================================================================
// Information passed from the bootloader to the hypervisor kernel.
// This is populated by the UEFI bootloader before jumping to the kernel.

struct BootInfo {
    // Physical memory map from UEFI
    PhysAddr memory_map_addr;
    usize    memory_map_size;
    usize    memory_map_desc_size;
    u32      memory_map_desc_version;

    // Framebuffer information (from UEFI GOP, if available)
    PhysAddr framebuffer_addr;
    u32      framebuffer_width;
    u32      framebuffer_height;
    u32      framebuffer_pitch;    // Bytes per scanline
    u32      framebuffer_bpp;      // Bits per pixel

    // UEFI Runtime Services pointer (for runtime UEFI calls)
    u64      runtime_services;

    // ACPI RSDP address (x86_64) or Device Tree address (ARM64)
    PhysAddr acpi_rsdp;
};

// ============================================================================
// Nexus — Main Hypervisor Class (Singleton)
// ============================================================================

class BMinOS {
public:
    /// Get the singleton instance. The Nexus is a singleton because there
    /// is exactly one hypervisor running on the hardware — there's no
    /// meaningful use case for multiple instances.
    static Nexus& instance();

    // Delete copy/move — singleton semantics.
    Nexus(const Nexus&) = delete;
    Nexus& operator=(const Nexus&) = delete;
    Nexus(Nexus&&) = delete;
    Nexus& operator=(Nexus&&) = delete;

    /// Initialize the hypervisor with boot information from the bootloader.
    /// This is the main initialization sequence — it brings up all subsystems
    /// in dependency order:
    ///   1. Serial console (already done by bootloader, re-verified)
    ///   2. Memory manager (physical + virtual)
    ///   3. Virtualization hardware (VT-x / ARM VHE)
    ///   4. Scheduler
    ///   5. Device manager
    ///   6. Bridge engine (OS compatibility layer)
    ///   7. Domain manager (VM management)
    Status init(const BootInfo& boot_info);

    /// Enter the main hypervisor event loop. This function does not return
    /// under normal operation — it processes VM exits, interrupts, and
    /// scheduling events forever.
    BMINOS_NORETURN void run();

    /// Gracefully shut down the hypervisor. This is called in response to
    /// a shutdown request (e.g., from a management VM). It tears down
    /// subsystems in reverse order.
    Status shutdown();

    // ----- Architecture Queries -----

    /// Returns the architecture we're running on. This is a constexpr on
    /// each architecture, so the compiler can eliminate dead code.
    static constexpr ArchType architecture() {
#if BMINOS_ARCH_X86_64
        return ArchType::X86_64;
#elif BMINOS_ARCH_ARM64
        return ArchType::ARM64;
#endif
    }

    /// Returns detailed virtualization capabilities of the hardware.
    [[nodiscard]] const VirtCapabilities& virt_caps() const { return virt_caps_; }

    /// Returns the boot information structure.
    [[nodiscard]] const BootInfo& boot_info() const { return boot_info_; }

    /// Returns the version info.
    [[nodiscard]] static constexpr const VersionInfo& version() { return BMINOS_VERSION; }

    // ----- Subsystem Accessors -----
    // These return references to the subsystem managers. Each is initialized
    // during init() and remains valid until shutdown().
    // For now they return pointers since the subsystems aren't defined yet.

    [[nodiscard]] MemoryManager*  memory_manager()  { return memory_manager_; }
    [[nodiscard]] Scheduler*      scheduler()       { return scheduler_; }
    [[nodiscard]] DeviceManager*  device_manager()  { return device_manager_; }
    [[nodiscard]] BridgeEngine*   bridge_engine()   { return bridge_engine_; }
    [[nodiscard]] DomainManager*  domain_manager()  { return domain_manager_; }

private:
    /// Private constructor — use instance() to access.
    Nexus() = default;

    // ----- Initialization Phases -----
    // Each phase is a separate method for clarity and debuggability.
    // If any phase fails, init() returns the error immediately.

    /// Detect CPU architecture features and capabilities.
    Status detect_architecture();

    /// Detect and validate hardware virtualization support.
    Status detect_virtualization_support();

    /// Initialize the serial console (verify bootloader setup).
    Status init_serial();

    /// Parse the UEFI memory map and initialize the physical page allocator.
    Status init_memory(const BootInfo& boot_info);

    /// Enable hardware virtualization (VMXON / HCR_EL2 setup).
    Status init_virtualization();

    /// Initialize the scheduler subsystem.
    Status init_scheduler();

    /// Initialize the device manager and enumerate hardware.
    Status init_devices();

    /// Print the boot banner with ASCII art and system info.
    void print_boot_banner();

    // ----- State -----

    bool initialized_ = false;

    BootInfo boot_info_ = {};
    VirtCapabilities virt_caps_ = {};

    // Subsystem pointers — set during init().
    // These are raw pointers because the subsystems are allocated from
    // our own memory manager (not new/delete) and their lifetimes are
    // tied to the Nexus singleton.
    MemoryManager*  memory_manager_  = nullptr;
    Scheduler*      scheduler_       = nullptr;
    DeviceManager*  device_manager_  = nullptr;
    BridgeEngine*   bridge_engine_   = nullptr;
    DomainManager*  domain_manager_  = nullptr;
};

} // namespace bminos
