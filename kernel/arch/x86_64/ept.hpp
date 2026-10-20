// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Extended Page Tables (EPT) — Intel VT-x
// ========================================
// EPT provides a second level of address translation for guests running under
// VMX. The guest's "physical addresses" (Guest Physical Addresses, GPAs) are
// translated through EPT to actual Host Physical Addresses (HPAs).
//
// This gives the hypervisor complete control over the guest's physical memory
// view without needing to shadow the guest's own page tables. EPT is the Intel
// equivalent of AMD's Nested Page Tables (NPT).
//
// EPT uses a 4-level page table structure similar to the regular x86_64 page
// tables, but with different entry formats:
//   PML4 → PDPT (EPT) → PD (EPT) → PT (EPT) → Physical Page
//
// Key differences from regular page tables:
// - No "user/supervisor" distinction — access control is Read/Write/Execute
// - Memory type field in each entry (UC, WC, WT, WP, WB)
// - No NX bit — instead, there's an explicit Execute permission bit
// - Support for "execute-only" pages (if the CPU supports it)
//
// References:
//   Intel SDM Volume 3, Chapter 28 (VMX Support for Address Translation)

#pragma once

#include <cstdint>
#include <cstddef>

namespace bminos::arch::x86_64 {

// Forward declaration
class VmxManager;

// =============================================================================
// EPT Constants
// =============================================================================

/// Number of entries per EPT page table level (same as regular page tables)
inline constexpr uint64_t EPT_ENTRIES_PER_TABLE = 512;

/// EPT page table level shifts
inline constexpr uint64_t EPT_PML4_SHIFT = 39;
inline constexpr uint64_t EPT_PDPT_SHIFT = 30;
inline constexpr uint64_t EPT_PD_SHIFT   = 21;
inline constexpr uint64_t EPT_PT_SHIFT   = 12;
inline constexpr uint64_t EPT_INDEX_MASK  = 0x1FF; // 9 bits

/// Guest physical address type
using GuestPhysAddr = uint64_t;

/// Host physical address type
using HostPhysAddr = uint64_t;

// =============================================================================
// EPT Memory Types
// =============================================================================
// EPT entries specify the memory type for the mapped page. This interacts with
// the guest's PAT/MTRR settings according to complex combining rules (see
// Intel SDM Volume 3, 28.2.6). In practice:
// - Use WB (Write-Back) for regular RAM
// - Use UC (Uncacheable) for MMIO regions

enum class EptMemoryType : uint8_t {
    Uncacheable     = 0,    // UC — No caching at all (for MMIO)
    WriteCombining  = 1,    // WC — Write-combining (for framebuffers)
    WriteThrough    = 4,    // WT — Reads cached, writes go to memory
    WriteProtect    = 5,    // WP — Reads cached, writes cause cache line invalidation
    WriteBack       = 6,    // WB — Full caching (default for RAM)
};

// =============================================================================
// EPT Entry Flags
// =============================================================================

/// Flags for EPT page table entries.
/// Unlike regular page tables, EPT has explicit Read/Write/Execute permissions
/// as separate bits, and includes a memory type field.
enum class EptFlags : uint64_t {
    None            = 0,

    // Access permissions (bits 2:0)
    Read            = 1ULL << 0,    // Allow reads from this page
    Write           = 1ULL << 1,    // Allow writes to this page
    Execute         = 1ULL << 2,    // Allow instruction fetches (supervisor)

    // Memory type (bits 5:3) — only valid in leaf (final-level) entries
    // These are shifted into position; use set_memory_type() helper
    MemTypeMask     = 7ULL << 3,

    // Misc flags
    IgnorePat       = 1ULL << 6,    // Ignore guest PAT memory type
    LargePage       = 1ULL << 7,    // 2MB page (in PD) or 1GB page (in PDPT)
    Accessed        = 1ULL << 8,    // Page has been accessed (if enabled)
    Dirty           = 1ULL << 9,    // Page has been written (if enabled)
    ExecuteUser     = 1ULL << 10,   // Allow user-mode execute (mode-based EPT)

    // Address mask for the physical address in the entry (bits 51:12)
    AddressMask     = 0x000FFFFFFFFFF000ULL,

    // Suppress #VE (virtualization exception) — bit 63
    SuppressVE      = 1ULL << 63,

    // Common combinations
    ReadOnly        = Read,
    ReadWrite       = Read | Write,
    ReadExecute     = Read | Execute,
    ReadWriteExecute= Read | Write | Execute,
    FullAccess      = Read | Write | Execute,
};

/// Bitwise operators for EptFlags
[[nodiscard]] inline constexpr EptFlags operator|(EptFlags a, EptFlags b) {
    return static_cast<EptFlags>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}

[[nodiscard]] inline constexpr EptFlags operator&(EptFlags a, EptFlags b) {
    return static_cast<EptFlags>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
}

[[nodiscard]] inline constexpr EptFlags operator~(EptFlags a) {
    return static_cast<EptFlags>(~static_cast<uint64_t>(a));
}

inline constexpr EptFlags& operator|=(EptFlags& a, EptFlags b) {
    a = a | b; return a;
}

[[nodiscard]] inline constexpr bool has_ept_flag(EptFlags flags, EptFlags test) {
    return (flags & test) == test;
}

// =============================================================================
// EPT Entry
// =============================================================================

/// A single EPT page table entry (64 bits).
struct EptEntry {
    uint64_t value = 0;

    /// Check if this entry has any access permissions (effectively "present")
    [[nodiscard]] bool is_present() const {
        return (value & 0x7) != 0; // Read | Write | Execute
    }

    /// Check if this is a large page entry (2MB or 1GB)
    [[nodiscard]] bool is_large_page() const {
        return value & static_cast<uint64_t>(EptFlags::LargePage);
    }

    /// Get the physical address from this entry
    [[nodiscard]] HostPhysAddr address() const {
        return value & static_cast<uint64_t>(EptFlags::AddressMask);
    }

    /// Set the physical address (preserving flags)
    void set_address(HostPhysAddr addr) {
        value = (value & ~static_cast<uint64_t>(EptFlags::AddressMask))
              | (addr & static_cast<uint64_t>(EptFlags::AddressMask));
    }

    /// Set access flags
    void set_flags(EptFlags flags) {
        value |= static_cast<uint64_t>(flags);
    }

    /// Clear specific flags
    void clear_flags(EptFlags flags) {
        value &= ~static_cast<uint64_t>(flags);
    }

    /// Check if specific flags are set
    [[nodiscard]] bool has(EptFlags flags) const {
        return (value & static_cast<uint64_t>(flags)) == static_cast<uint64_t>(flags);
    }

    /// Set the memory type for a leaf entry
    void set_memory_type(EptMemoryType type) {
        value = (value & ~static_cast<uint64_t>(EptFlags::MemTypeMask))
              | (static_cast<uint64_t>(type) << 3);
    }

    /// Get the memory type from a leaf entry
    [[nodiscard]] EptMemoryType memory_type() const {
        return static_cast<EptMemoryType>((value >> 3) & 0x7);
    }

    /// Clear the entry
    void clear() { value = 0; }
};

static_assert(sizeof(EptEntry) == 8, "EPT entry must be 8 bytes");

/// An EPT page table — 512 entries, must be 4KB-aligned.
struct alignas(4096) EptPageTable {
    EptEntry entries[EPT_ENTRIES_PER_TABLE];

    /// Zero out all entries
    void clear() {
        for (auto& e : entries) {
            e.value = 0;
        }
    }
};

static_assert(sizeof(EptPageTable) == 4096, "EPT page table must be exactly 4KB");

// =============================================================================
// EPT Pointer (EPTP)
// =============================================================================
// The EPTP is written to the VMCS EPT_POINTER field. It contains:
//   Bits 2:0  — Memory type for EPT structures (should be WB = 6)
//   Bits 5:3  — EPT page-walk length minus 1 (3 for 4-level = value of 3)
//   Bit 6     — Accessed/dirty flags enable
//   Bits N:12 — Physical address of the PML4 table (4KB-aligned)

/// Construct an EPT pointer from a PML4 physical address.
/// @param pml4_phys Physical address of the EPT PML4 table.
/// @param enable_ad Enable accessed/dirty flag tracking.
/// @return The formatted EPTP value.
[[nodiscard]] inline constexpr uint64_t make_eptp(HostPhysAddr pml4_phys,
                                                   bool enable_ad = false) {
    uint64_t eptp = 0;
    eptp |= static_cast<uint64_t>(EptMemoryType::WriteBack); // Memory type = WB
    eptp |= (3ULL << 3);   // Page-walk length = 4 (value = 3 means 4 levels)
    if (enable_ad) {
        eptp |= (1ULL << 6); // Enable accessed/dirty flags
    }
    eptp |= (pml4_phys & 0x000FFFFFFFFFF000ULL); // PML4 physical address
    return eptp;
}

// =============================================================================
// EPT Violation Information
// =============================================================================

/// Parsed EPT violation details from the VM-exit qualification.
/// When an EPT violation occurs, the exit qualification tells us what the
/// guest was trying to do and why it failed.
struct EptViolationInfo {
    GuestPhysAddr   guest_phys_addr;    // Guest physical address that faulted
    uint64_t        guest_linear_addr;  // Guest linear address (if valid)
    bool            caused_by_read;     // Guest was reading
    bool            caused_by_write;    // Guest was writing
    bool            caused_by_exec;     // Guest was executing
    bool            ept_readable;       // EPT entry allows reads
    bool            ept_writable;       // EPT entry allows writes
    bool            ept_executable;     // EPT entry allows execute
    bool            guest_linear_valid; // guest_linear_addr is valid
    bool            final_violation;    // Violation is at the final EPT level (leaf)
    bool            nmi_unblocking;     // IRET was unblocking NMI when violation occurred
};

// =============================================================================
// EPT Manager
// =============================================================================

/// Manages the Extended Page Tables for a single guest VM.
///
/// Each guest gets its own EptManager with its own PML4. The hypervisor uses
/// this to control the guest's view of physical memory — mapping RAM, MMIO
/// regions, and emulated devices.
///
/// Thread safety: Currently single-threaded per-VM. If multiple vCPUs of the
/// same VM can trigger EPT modifications concurrently, external locking is needed.
class EptManager {
public:
    EptManager() = default;
    ~EptManager() = default;

    // Non-copyable
    EptManager(const EptManager&) = delete;
    EptManager& operator=(const EptManager&) = delete;

    /// Initialize the EPT: allocate the PML4 table.
    /// @return true on success.
    bool init();

    /// Destroy the EPT: free all page table structures.
    void destroy();

    /// Map a guest physical address to a host physical address.
    /// @param guest_phys Guest physical address (4KB-aligned).
    /// @param host_phys Host physical address to map to.
    /// @param flags EPT access flags (Read/Write/Execute).
    /// @param mem_type Memory type for this mapping.
    /// @return true on success.
    bool map_page(GuestPhysAddr guest_phys, HostPhysAddr host_phys,
                  EptFlags flags, EptMemoryType mem_type = EptMemoryType::WriteBack);

    /// Map a 2MB large page.
    bool map_large_page(GuestPhysAddr guest_phys, HostPhysAddr host_phys,
                        EptFlags flags, EptMemoryType mem_type = EptMemoryType::WriteBack);

    /// Unmap a guest physical address.
    /// @param guest_phys Guest physical address to unmap.
    void unmap_page(GuestPhysAddr guest_phys);

    /// Map a contiguous range of guest physical memory.
    /// This is the primary method for setting up a guest's RAM. It creates
    /// identity mappings (GPA == HPA) or offset mappings.
    /// @param guest_phys_start Start of the guest physical range.
    /// @param host_phys_start Start of the host physical range.
    /// @param page_count Number of 4KB pages to map.
    /// @param flags Access flags.
    /// @param mem_type Memory type.
    /// @return true on success.
    bool map_range(GuestPhysAddr guest_phys_start, HostPhysAddr host_phys_start,
                   uint64_t page_count, EptFlags flags,
                   EptMemoryType mem_type = EptMemoryType::WriteBack);

    /// Unmap a range of guest physical addresses.
    void unmap_range(GuestPhysAddr guest_phys_start, uint64_t page_count);

    /// Translate a guest physical address to a host physical address.
    /// Walks the EPT tables.
    /// @param guest_phys Guest physical address to translate.
    /// @return Host physical address, or 0 if not mapped.
    [[nodiscard]] HostPhysAddr translate(GuestPhysAddr guest_phys) const;

    /// Handle an EPT violation VM-exit.
    /// Parses the exit qualification and guest physical address, then takes
    /// appropriate action (e.g., map on demand, emulate MMIO).
    /// @param exit_qualification The exit qualification from the VMCS.
    /// @param guest_phys The faulting guest physical address.
    /// @return true if the violation was handled and the guest can be resumed.
    bool handle_violation(uint64_t exit_qualification, GuestPhysAddr guest_phys);

    /// Handle an EPT misconfiguration VM-exit.
    /// This indicates a bug in our EPT setup (invalid entry). Always fatal.
    void handle_misconfiguration(GuestPhysAddr guest_phys);

    /// Get the EPT pointer (EPTP) for this guest's EPT.
    /// This value is written to the VMCS EPT_POINTER field.
    [[nodiscard]] uint64_t get_eptp() const;

    /// Get the physical address of the PML4 table.
    [[nodiscard]] HostPhysAddr pml4_address() const { return m_pml4_phys; }

    /// Invalidate all EPT-derived translations for this guest.
    /// Must be called after modifying EPT entries.
    void invalidate_ept() const;

    /// Parse EPT violation exit qualification into a structured format.
    [[nodiscard]] static EptViolationInfo parse_violation(uint64_t qualification,
                                                          GuestPhysAddr guest_phys,
                                                          uint64_t guest_linear);

    /// Get statistics about EPT usage
    struct EptStatistics {
        uint64_t mapped_pages;      // Total 4KB pages mapped
        uint64_t large_pages;       // Total 2MB large pages mapped
        uint64_t table_pages;       // Pages used for EPT table structures
        uint64_t violations_handled;// EPT violations handled
    };

    [[nodiscard]] EptStatistics get_statistics() const { return m_stats; }

private:
    /// Extract the PML4 index from a guest physical address
    [[nodiscard]] static constexpr uint64_t pml4_index(GuestPhysAddr gpa) {
        return (gpa >> EPT_PML4_SHIFT) & EPT_INDEX_MASK;
    }

    /// Extract the PDPT index
    [[nodiscard]] static constexpr uint64_t pdpt_index(GuestPhysAddr gpa) {
        return (gpa >> EPT_PDPT_SHIFT) & EPT_INDEX_MASK;
    }

    /// Extract the PD index
    [[nodiscard]] static constexpr uint64_t pd_index(GuestPhysAddr gpa) {
        return (gpa >> EPT_PD_SHIFT) & EPT_INDEX_MASK;
    }

    /// Extract the PT index
    [[nodiscard]] static constexpr uint64_t pt_index(GuestPhysAddr gpa) {
        return (gpa >> EPT_PT_SHIFT) & EPT_INDEX_MASK;
    }

    /// Allocate a new EPT page table (zeroed, page-aligned).
    /// @return Pointer to the new table, or nullptr on failure.
    [[nodiscard]] EptPageTable* alloc_ept_table();

    /// Free an EPT page table.
    void free_ept_table(EptPageTable* table);

    /// Ensure the page table hierarchy exists for a given GPA.
    /// Creates intermediate tables as needed.
    /// @return Pointer to the final-level EPT page table, or nullptr.
    [[nodiscard]] EptPageTable* ensure_hierarchy(GuestPhysAddr gpa);

    /// Walk the EPT and return a pointer to the leaf entry for a GPA.
    /// @param gpa Guest physical address to look up.
    /// @return Pointer to the EPT leaf entry, or nullptr if not mapped.
    [[nodiscard]] EptEntry* walk(GuestPhysAddr gpa);
    [[nodiscard]] const EptEntry* walk(GuestPhysAddr gpa) const;

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    HostPhysAddr    m_pml4_phys = 0;    // Physical address of EPT PML4
    EptPageTable*   m_pml4_virt = nullptr; // Virtual address of EPT PML4
    uint64_t        m_eptp = 0;         // Cached EPTP value

    /// Whether accessed/dirty flag tracking is enabled
    bool m_ad_enabled = false;

    /// Statistics
    EptStatistics m_stats = {};

    /// Whether init() has been called
    bool m_initialized = false;
};

} // namespace bminos::arch::x86_64
