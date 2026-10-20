// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Physical Memory Manager (PMM)
// =============================
// Bitmap-based physical page allocator for 4KB pages. The PMM is the lowest
// level of the memory management hierarchy — it tracks which physical frames
// are available and hands them out to the VMM, heap, EPT, and other subsystems.
//
// Design decisions:
// - Bitmap over free-list: Bitmaps give O(1) per-page status checks and are
//   cache-friendly for sequential scans. For a hypervisor managing potentially
//   hundreds of GB, the bitmap overhead (~32KB per GB) is negligible.
// - Single global allocator: We use a spinlock for thread safety. Per-NUMA or
//   per-CPU free lists can be layered on top later for scalability.

#pragma once

#include <cstdint>
#include <cstddef>

namespace bminos::mm {

// =============================================================================
// Constants
// =============================================================================

/// Standard x86_64 page size (4 KiB)
inline constexpr uint64_t PAGE_SIZE = 4096;

/// Shift count to convert between addresses and page frame numbers
inline constexpr uint64_t PAGE_SHIFT = 12;

/// Maximum physical memory we support (256 GB should be plenty for now)
inline constexpr uint64_t MAX_PHYSICAL_MEMORY = 256ULL * 1024 * 1024 * 1024;

/// Maximum number of page frames
inline constexpr uint64_t MAX_PAGE_FRAMES = MAX_PHYSICAL_MEMORY / PAGE_SIZE;

/// Bits per bitmap element (uint64_t)
inline constexpr uint64_t BITS_PER_ELEMENT = 64;

/// Size of the bitmap array
inline constexpr uint64_t BITMAP_SIZE = MAX_PAGE_FRAMES / BITS_PER_ELEMENT;

// =============================================================================
// Types
// =============================================================================

/// Physical address type — strong typedef to prevent mixing with virtual addrs
using PhysAddr = uint64_t;

/// Memory region type classifications from the firmware memory map.
/// These mirror UEFI memory descriptor types but are firmware-agnostic.
enum class MemoryRegionType : uint8_t {
    Available,      // Free RAM, usable by the hypervisor
    Reserved,       // Reserved by firmware, do not touch
    AcpiReclaimable,// ACPI tables — reclaimable after parsing
    AcpiNvs,        // ACPI Non-Volatile Storage — must be preserved
    Mmio,           // Memory-mapped I/O regions
    Hypervisor,     // Claimed by BMinOS kernel/hypervisor code
    BadMemory,      // Known-bad physical memory
};

/// A single entry in the firmware-provided memory map.
/// The bootloader converts UEFI memory descriptors into this format.
struct MemoryRegion {
    PhysAddr    base;       // Start of the region (page-aligned)
    uint64_t    length;     // Length in bytes
    MemoryRegionType type;  // Classification
    uint8_t     _pad[7];    // Explicit padding for alignment
};

/// The complete memory map passed from the bootloader.
struct MemoryMap {
    MemoryRegion*   regions;        // Array of memory regions
    uint64_t        region_count;   // Number of entries
    uint64_t        total_memory;   // Total detected physical memory in bytes
};

/// Memory statistics snapshot
struct PmmStatistics {
    uint64_t total_pages;       // Total physical pages detected
    uint64_t used_pages;        // Pages currently allocated
    uint64_t free_pages;        // Pages available for allocation
    uint64_t reserved_pages;    // Pages marked reserved (firmware, MMIO, etc.)
    uint64_t hypervisor_pages;  // Pages used by the hypervisor itself
    uint64_t total_bytes;       // Convenience: total_pages * PAGE_SIZE
    uint64_t free_bytes;        // Convenience: free_pages * PAGE_SIZE
};

// =============================================================================
// Physical Memory Manager
// =============================================================================

/// Bitmap-based physical memory allocator.
///
/// The allocator maintains a bitmap where each bit represents one 4KB page frame.
/// A set bit (1) means the page is **used/allocated**, a clear bit (0) means free.
///
/// Thread safety: All public methods are internally synchronized with a spinlock.
class PhysicalMemoryManager {
public:
    PhysicalMemoryManager() = default;

    // Non-copyable, non-movable — there's exactly one PMM instance
    PhysicalMemoryManager(const PhysicalMemoryManager&) = delete;
    PhysicalMemoryManager& operator=(const PhysicalMemoryManager&) = delete;

    /// Initialize the PMM from the firmware memory map.
    /// Must be called exactly once, early in kernel boot, before any allocations.
    /// This marks all memory as used, then frees regions flagged as Available.
    void init(const MemoryMap& memory_map);

    /// Allocate a single physical page (4 KB).
    /// @return Physical address of the allocated page, or 0 on failure.
    [[nodiscard]] PhysAddr alloc_page();

    /// Allocate `count` contiguous physical pages.
    /// @param count Number of contiguous pages requested.
    /// @return Physical address of the first page, or 0 if not enough contiguous memory.
    [[nodiscard]] PhysAddr alloc_pages(uint64_t count);

    /// Free a single previously-allocated physical page.
    /// @param addr Physical address of the page (must be page-aligned).
    void free_page(PhysAddr addr);

    /// Free `count` contiguous physical pages starting at `addr`.
    /// @param addr Physical address of the first page (must be page-aligned).
    /// @param count Number of contiguous pages to free.
    void free_pages(PhysAddr addr, uint64_t count);

    /// Check if a specific physical page is allocated.
    /// @param addr Physical address to check (must be page-aligned).
    /// @return true if the page is currently allocated/used.
    [[nodiscard]] bool is_page_used(PhysAddr addr) const;

    /// Get current memory statistics.
    [[nodiscard]] PmmStatistics get_statistics() const;

    /// Mark a range of physical memory as reserved (not allocatable).
    /// Used for MMIO regions, firmware tables, and the hypervisor's own code/data.
    void mark_region_reserved(PhysAddr base, uint64_t length);

    /// Mark a range of physical memory as available for allocation.
    void mark_region_available(PhysAddr base, uint64_t length);

private:
    /// Convert a physical address to a page frame number
    [[nodiscard]] static constexpr uint64_t addr_to_frame(PhysAddr addr) {
        return addr >> PAGE_SHIFT;
    }

    /// Convert a page frame number back to a physical address
    [[nodiscard]] static constexpr PhysAddr frame_to_addr(uint64_t frame) {
        return frame << PAGE_SHIFT;
    }

    /// Set a bit in the bitmap (mark page as used)
    void set_bit(uint64_t frame);

    /// Clear a bit in the bitmap (mark page as free)
    void clear_bit(uint64_t frame);

    /// Test whether a bit is set in the bitmap
    [[nodiscard]] bool test_bit(uint64_t frame) const;

    /// Find the first free page frame starting from `start_frame`
    [[nodiscard]] uint64_t find_first_free(uint64_t start_frame = 0) const;

    /// Find `count` contiguous free page frames
    [[nodiscard]] uint64_t find_contiguous_free(uint64_t count) const;

    /// Acquire the spinlock
    void lock();

    /// Release the spinlock
    void unlock();

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    /// Bitmap: 1 bit per page frame. 1 = used, 0 = free.
    /// Statically sized for MAX_PHYSICAL_MEMORY. At 256 GB max, this is 8 MB —
    /// acceptable for a hypervisor that will manage far more memory than that.
    uint64_t m_bitmap[BITMAP_SIZE] = {};

    /// Total number of physical page frames detected
    uint64_t m_total_frames = 0;

    /// Number of currently used (allocated + reserved) page frames
    uint64_t m_used_frames = 0;

    /// Number of pages reserved by firmware/hypervisor (subset of used)
    uint64_t m_reserved_frames = 0;

    /// Number of pages used by the hypervisor itself (subset of reserved)
    uint64_t m_hypervisor_frames = 0;

    /// Highest usable page frame number (optimization: don't scan beyond this)
    uint64_t m_max_frame = 0;

    /// Simple spinlock for thread safety (0 = unlocked, 1 = locked)
    volatile uint64_t m_lock = 0;

    /// Whether init() has been called
    bool m_initialized = false;
};

/// Global PMM instance — singleton pattern for the physical memory allocator
extern PhysicalMemoryManager g_pmm;

} // namespace bminos::mm
