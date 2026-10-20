// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Shared Memory Regions
// =====================
// Provides shared memory primitives for inter-domain communication. A domain
// (guest OS or service) can create a shared memory region and grant access to
// other domains with fine-grained permissions.
//
// This is a fundamental building block for the hypervisor's IPC mechanism:
// - Shared ring buffers for high-throughput data transfer
// - Shared pages for notification/event channels
// - Memory-mapped device emulation between domains
//
// Design:
// - Each SharedMemoryRegion owns a set of physical pages allocated from the PMM.
// - Domains map/unmap the region into their own virtual address spaces via the VMM.
// - Reference counting ensures pages aren't freed while any domain has them mapped.
// - Access is controlled per-domain with read/write/execute permissions.
// - A spinlock protects the region metadata; the shared pages themselves need
//   higher-level synchronization (provided by the consumer, e.g., ring buffer locks).

#pragma once

#include <cstdint>
#include <cstddef>

namespace bminos::mm {

// Forward declarations
class PhysicalMemoryManager;
class VirtualMemoryManager;

// =============================================================================
// Constants
// =============================================================================

/// Maximum number of domains that can simultaneously map a shared region
inline constexpr uint32_t MAX_SHARED_DOMAINS = 64;

/// Maximum number of active shared memory regions system-wide
inline constexpr uint32_t MAX_SHARED_REGIONS = 256;

/// Invalid/null domain ID
inline constexpr uint32_t INVALID_DOMAIN_ID = 0xFFFFFFFF;

/// Invalid/null region ID
inline constexpr uint32_t INVALID_REGION_ID = 0xFFFFFFFF;

// =============================================================================
// Types
// =============================================================================

/// Domain identifier (matches the domain/VM ID used elsewhere in the hypervisor)
using DomainId = uint32_t;

/// Region identifier for shared memory regions
using RegionId = uint32_t;

/// Access flags for shared memory mappings.
/// These are orthogonal to page table flags — the SharedMemoryRegion enforces
/// these at map time by selecting appropriate page flags.
enum class SharedAccessFlags : uint32_t {
    None    = 0,
    Read    = 1 << 0,   // Domain can read from the shared region
    Write   = 1 << 1,   // Domain can write to the shared region
    Execute = 1 << 2,   // Domain can execute code from the shared region (rare)

    // Common combinations
    ReadOnly    = Read,
    ReadWrite   = Read | Write,
    ReadExecute = Read | Execute,
    Full        = Read | Write | Execute,
};

/// Bitwise operators for SharedAccessFlags
[[nodiscard]] inline constexpr SharedAccessFlags operator|(SharedAccessFlags a, SharedAccessFlags b) {
    return static_cast<SharedAccessFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

[[nodiscard]] inline constexpr SharedAccessFlags operator&(SharedAccessFlags a, SharedAccessFlags b) {
    return static_cast<SharedAccessFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

[[nodiscard]] inline constexpr bool has_access(SharedAccessFlags flags, SharedAccessFlags test) {
    return (flags & test) == test;
}

/// Per-domain mapping record within a shared region
struct DomainMapping {
    DomainId    domain_id = INVALID_DOMAIN_ID;  // Which domain
    uint64_t    virt_addr = 0;                   // Where it's mapped in that domain
    SharedAccessFlags access = SharedAccessFlags::None; // Granted permissions
    bool        active = false;                  // Is this mapping currently live?
};

/// State of a shared memory region
enum class SharedRegionState : uint8_t {
    Free,       // Slot is available for allocation
    Active,     // Region is created and can be mapped
    Destroying, // Region is being torn down (unmapping from all domains)
};

// =============================================================================
// Shared Memory Region
// =============================================================================

/// A shared memory region backed by physical pages that can be mapped into
/// multiple domain address spaces with per-domain access control.
///
/// Lifecycle:
///   1. create() — allocates physical pages, sets owner
///   2. grant_access() — allows other domains to map the region
///   3. map_to_domain() — maps the pages into a domain's address space
///   4. unmap_from_domain() — removes the mapping from a domain
///   5. revoke_access() — revokes a domain's permission (and unmaps if needed)
///   6. destroy() — unmaps from all domains, frees physical pages
class SharedMemoryRegion {
public:
    SharedMemoryRegion() = default;

    /// Create a new shared memory region.
    /// @param id Unique region identifier.
    /// @param size_bytes Size in bytes (will be rounded up to page boundary).
    /// @param owner The domain that owns this region.
    /// @param pmm Physical memory manager for allocating backing pages.
    /// @return true on success, false if allocation failed.
    bool create(RegionId id, uint64_t size_bytes, DomainId owner,
                PhysicalMemoryManager& pmm);

    /// Destroy the region: unmap from all domains and free backing pages.
    void destroy(PhysicalMemoryManager& pmm, VirtualMemoryManager& vmm);

    /// Grant a domain access to this region with specified permissions.
    /// This doesn't map the region — the domain must call map_to_domain().
    /// @param domain The domain to grant access to.
    /// @param access The access permissions to grant.
    /// @return true on success, false if max domains reached.
    bool grant_access(DomainId domain, SharedAccessFlags access);

    /// Revoke a domain's access. If the region is currently mapped in that
    /// domain, it will be unmapped first.
    void revoke_access(DomainId domain, VirtualMemoryManager& vmm);

    /// Map this region into a domain's virtual address space.
    /// The domain must have been previously granted access via grant_access().
    /// @param domain The domain to map into.
    /// @param vmm Virtual memory manager for the mapping.
    /// @param hint_vaddr Optional preferred virtual address (0 = let VMM choose).
    /// @return Virtual address where the region was mapped, or 0 on failure.
    [[nodiscard]] uint64_t map_to_domain(DomainId domain, VirtualMemoryManager& vmm,
                                          uint64_t hint_vaddr = 0);

    /// Unmap this region from a domain's virtual address space.
    /// The domain's access grant is preserved — it can re-map later.
    void unmap_from_domain(DomainId domain, VirtualMemoryManager& vmm);

    /// Check if a domain has access to this region.
    [[nodiscard]] bool has_domain_access(DomainId domain) const;

    /// Get the access flags granted to a specific domain.
    [[nodiscard]] SharedAccessFlags get_domain_access(DomainId domain) const;

    /// Check if the region is currently mapped in a specific domain.
    [[nodiscard]] bool is_mapped_in_domain(DomainId domain) const;

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    [[nodiscard]] RegionId id() const { return m_id; }
    [[nodiscard]] DomainId owner() const { return m_owner; }
    [[nodiscard]] uint64_t size() const { return m_size; }
    [[nodiscard]] uint64_t page_count() const { return m_page_count; }
    [[nodiscard]] uint32_t ref_count() const { return m_ref_count; }
    [[nodiscard]] SharedRegionState state() const { return m_state; }

    /// Get the physical address of the i-th backing page.
    /// @param index Page index (0-based).
    /// @return Physical address, or 0 if index is out of range.
    [[nodiscard]] uint64_t get_backing_page(uint64_t index) const;

private:
    /// Find the DomainMapping slot for a given domain.
    /// @return Pointer to the mapping, or nullptr if not found.
    [[nodiscard]] DomainMapping* find_mapping(DomainId domain);
    [[nodiscard]] const DomainMapping* find_mapping(DomainId domain) const;

    /// Find a free DomainMapping slot.
    [[nodiscard]] DomainMapping* find_free_slot();

    /// Acquire the spinlock
    void lock();

    /// Release the spinlock
    void unlock();

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    RegionId    m_id = INVALID_REGION_ID;
    DomainId    m_owner = INVALID_DOMAIN_ID;
    uint64_t    m_size = 0;             // Size in bytes (page-aligned)
    uint64_t    m_page_count = 0;       // Number of backing pages

    /// Physical addresses of backing pages.
    /// For regions up to 1 MB (256 pages), we store them inline.
    /// Larger regions would need a dynamic array — a future enhancement.
    static constexpr uint64_t MAX_INLINE_PAGES = 256;
    uint64_t    m_backing_pages[MAX_INLINE_PAGES] = {};

    /// Per-domain mapping records
    DomainMapping m_mappings[MAX_SHARED_DOMAINS] = {};

    /// Reference count: number of domains currently mapping this region
    uint32_t    m_ref_count = 0;

    /// Region state
    SharedRegionState m_state = SharedRegionState::Free;

    /// Spinlock for thread safety
    volatile uint64_t m_lock = 0;
};

// =============================================================================
// Shared Memory Manager
// =============================================================================

/// Manages the global pool of shared memory regions.
///
/// This is the top-level API that domains call through hypercalls to create
/// and manage shared memory. It maintains a fixed-size array of regions and
/// assigns unique IDs.
class SharedMemoryManager {
public:
    SharedMemoryManager() = default;

    // Non-copyable, non-movable
    SharedMemoryManager(const SharedMemoryManager&) = delete;
    SharedMemoryManager& operator=(const SharedMemoryManager&) = delete;

    /// Initialize the shared memory manager.
    void init(PhysicalMemoryManager& pmm, VirtualMemoryManager& vmm);

    /// Create a new shared memory region.
    /// @param size_bytes Requested size (rounded up to page boundary).
    /// @param owner Domain that owns the region.
    /// @param allowed_domains Array of domain IDs to grant initial access.
    /// @param allowed_count Number of entries in allowed_domains.
    /// @param access Default access flags for allowed domains.
    /// @return Region ID, or INVALID_REGION_ID on failure.
    [[nodiscard]] RegionId create_region(uint64_t size_bytes, DomainId owner,
                                         const DomainId* allowed_domains,
                                         uint32_t allowed_count,
                                         SharedAccessFlags access = SharedAccessFlags::ReadWrite);

    /// Destroy a shared memory region. Only the owner domain can do this.
    /// @param region_id The region to destroy.
    /// @param requester The domain requesting destruction (must be owner).
    /// @return true on success.
    bool destroy_region(RegionId region_id, DomainId requester);

    /// Map a shared region into a domain's address space.
    /// @return Virtual address where mapped, or 0 on failure.
    [[nodiscard]] uint64_t map_region(RegionId region_id, DomainId domain,
                                       uint64_t hint_vaddr = 0);

    /// Unmap a shared region from a domain's address space.
    bool unmap_region(RegionId region_id, DomainId domain);

    /// Grant a domain access to an existing region.
    bool grant_access(RegionId region_id, DomainId requester,
                      DomainId target, SharedAccessFlags access);

    /// Revoke a domain's access to a region.
    bool revoke_access(RegionId region_id, DomainId requester, DomainId target);

    /// Get a reference to a region by ID.
    /// @return Pointer to the region, or nullptr if not found.
    [[nodiscard]] SharedMemoryRegion* get_region(RegionId id);
    [[nodiscard]] const SharedMemoryRegion* get_region(RegionId id) const;

private:
    /// Find a free region slot and assign the next ID.
    [[nodiscard]] SharedMemoryRegion* allocate_region_slot(RegionId& out_id);

    /// Pool of shared memory regions
    SharedMemoryRegion m_regions[MAX_SHARED_REGIONS];

    /// Next region ID to assign (monotonically increasing)
    RegionId m_next_id = 1;

    /// PMM and VMM references
    PhysicalMemoryManager* m_pmm = nullptr;
    VirtualMemoryManager*  m_vmm = nullptr;

    /// Spinlock
    volatile uint64_t m_lock = 0;

    /// Whether init() has been called
    bool m_initialized = false;
};

/// Global shared memory manager instance
extern SharedMemoryManager g_shared_mem;

} // namespace bminos::mm
