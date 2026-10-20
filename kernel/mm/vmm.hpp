// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Virtual Memory Manager (VMM)
// ============================
// Manages the hypervisor's own virtual address space using x86_64 4-level page
// tables (PML4 → PDPT → PD → PT). The VMM sits above the PMM and provides
// virtual-to-physical mappings for the kernel and hypervisor components.
//
// Memory layout (higher-half kernel):
//   0x0000000000000000 - 0x00007FFFFFFFFFFF  User space (for guest passthrough)
//   0xFFFF800000000000 - 0xFFFF8FFFFFFFFFFF  Direct physical memory map
//   0xFFFF900000000000 - 0xFFFF9FFFFFFFFFFF  Kernel heap
//   0xFFFFA00000000000 - 0xFFFFAFFFFFFFFFFF  MMIO mappings
//   0xFFFFFF0000000000 - 0xFFFFFF7FFFFFFFFF  Kernel code/data
//   0xFFFFFF8000000000 - 0xFFFFFFFFFFFFFFFF  Kernel stacks & per-CPU data

#pragma once

#include <cstdint>
#include <cstddef>
#include "pmm.hpp"

namespace bminos::mm {

// =============================================================================
// Virtual Address Layout Constants
// =============================================================================

/// Base of the higher-half direct physical memory mapping.
/// All physical memory is identity-mapped starting here, so phys_to_virt is
/// just (phys + KERNEL_PHYS_MAP_BASE). This avoids the need to walk page
/// tables for most kernel operations.
inline constexpr uint64_t KERNEL_PHYS_MAP_BASE  = 0xFFFF800000000000ULL;

/// Kernel heap virtual address range
inline constexpr uint64_t KERNEL_HEAP_BASE      = 0xFFFF900000000000ULL;
inline constexpr uint64_t KERNEL_HEAP_END        = 0xFFFF9FFFFFFFFFFFULL;

/// MMIO mapping range
inline constexpr uint64_t KERNEL_MMIO_BASE      = 0xFFFFA00000000000ULL;
inline constexpr uint64_t KERNEL_MMIO_END        = 0xFFFFAFFFFFFFFFFFULL;

/// Kernel code/data mapping
inline constexpr uint64_t KERNEL_CODE_BASE      = 0xFFFFFF0000000000ULL;

/// Kernel stack area
inline constexpr uint64_t KERNEL_STACK_BASE     = 0xFFFFFF8000000000ULL;

// =============================================================================
// Page Table Constants
// =============================================================================

/// Number of entries per page table level (512 for x86_64)
inline constexpr uint64_t PAGE_TABLE_ENTRIES = 512;

/// Index masks and shifts for each page table level
inline constexpr uint64_t PML4_SHIFT = 39;
inline constexpr uint64_t PDPT_SHIFT = 30;
inline constexpr uint64_t PD_SHIFT   = 21;
inline constexpr uint64_t PT_SHIFT   = 12;
inline constexpr uint64_t INDEX_MASK = 0x1FF; // 9 bits

// =============================================================================
// Types
// =============================================================================

/// Virtual address type — strong typedef for clarity
using VirtAddr = uint64_t;

/// Page table entry flags.
/// These map directly to x86_64 page table entry bits. Using an enum class
/// with bitwise operators gives us type safety while keeping the hardware
/// mapping transparent.
enum class PageFlags : uint64_t {
    None        = 0,
    Present     = 1ULL << 0,    // P — Page is present in memory
    Writable    = 1ULL << 1,    // R/W — Page is writable
    User        = 1ULL << 2,    // U/S — Accessible from ring 3
    WriteThrough= 1ULL << 3,    // PWT — Write-through caching
    CacheDisable= 1ULL << 4,    // PCD — Cache disabled
    Accessed    = 1ULL << 5,    // A — Page has been accessed
    Dirty       = 1ULL << 6,    // D — Page has been written to
    HugePage    = 1ULL << 7,    // PS — 2MB/1GB huge page
    Global      = 1ULL << 8,    // G — Global page (not flushed on CR3 switch)
    NoExecute   = 1ULL << 63,   // NX — No-execute (requires EFER.NXE)

    // Common flag combinations for convenience
    KernelCode  = Present,                              // RX kernel code
    KernelData  = Present | Writable | NoExecute,       // RW kernel data
    KernelRO    = Present | NoExecute,                  // RO kernel data
    UserCode    = Present | User,                       // RX user code
    UserData    = Present | Writable | User | NoExecute,// RW user data
    Mmio        = Present | Writable | NoExecute | CacheDisable, // MMIO regions
};

/// Bitwise operators for PageFlags — necessary because enum class doesn't
/// provide them implicitly, but we need to combine flags freely.
[[nodiscard]] inline constexpr PageFlags operator|(PageFlags a, PageFlags b) {
    return static_cast<PageFlags>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
}

[[nodiscard]] inline constexpr PageFlags operator&(PageFlags a, PageFlags b) {
    return static_cast<PageFlags>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
}

[[nodiscard]] inline constexpr PageFlags operator~(PageFlags a) {
    return static_cast<PageFlags>(~static_cast<uint64_t>(a));
}

inline constexpr PageFlags& operator|=(PageFlags& a, PageFlags b) {
    a = a | b;
    return a;
}

inline constexpr PageFlags& operator&=(PageFlags& a, PageFlags b) {
    a = a & b;
    return a;
}

/// Check if a flag combination contains a specific flag
[[nodiscard]] inline constexpr bool has_flag(PageFlags flags, PageFlags test) {
    return (flags & test) == test;
}

/// A single page table entry (PTE). This is a thin wrapper around the raw
/// uint64_t that the hardware uses, providing typed access to fields.
struct PageTableEntry {
    uint64_t value = 0;

    /// Set flags on this entry
    void set_flags(PageFlags flags) {
        value |= static_cast<uint64_t>(flags);
    }

    /// Clear specific flags
    void clear_flags(PageFlags flags) {
        value &= ~static_cast<uint64_t>(flags);
    }

    /// Check if specific flags are set
    [[nodiscard]] bool has(PageFlags flags) const {
        return (value & static_cast<uint64_t>(flags)) == static_cast<uint64_t>(flags);
    }

    /// Get the physical address this entry points to (bits 12..51)
    [[nodiscard]] PhysAddr address() const {
        return value & 0x000FFFFFFFFFF000ULL;
    }

    /// Set the physical address (preserving flags)
    void set_address(PhysAddr addr) {
        value = (value & ~0x000FFFFFFFFFF000ULL) | (addr & 0x000FFFFFFFFFF000ULL);
    }

    /// Check if the entry is present
    [[nodiscard]] bool is_present() const {
        return value & static_cast<uint64_t>(PageFlags::Present);
    }
};

/// A page table — 512 entries, must be page-aligned.
/// This struct represents any level of the 4-level hierarchy.
struct alignas(PAGE_SIZE) PageTable {
    PageTableEntry entries[PAGE_TABLE_ENTRIES];

    /// Zero out all entries
    void clear() {
        for (auto& entry : entries) {
            entry.value = 0;
        }
    }
};

/// An address space encapsulates a complete set of page tables (PML4 root).
/// Each guest domain or the hypervisor itself has its own AddressSpace.
struct AddressSpace {
    PhysAddr    pml4_phys = 0;  // Physical address of the PML4 table
    PageTable*  pml4_virt = nullptr; // Virtual address (via direct map) for easy access
    bool        is_kernel = false;   // True for the hypervisor's own address space
};

// =============================================================================
// Virtual Memory Manager
// =============================================================================

/// Manages virtual address spaces using x86_64 4-level page tables.
///
/// The VMM provides the hypervisor with its own virtual address space and can
/// create/manage separate address spaces for guest domains. It uses the PMM
/// to allocate physical pages for page table structures.
class VirtualMemoryManager {
public:
    VirtualMemoryManager() = default;

    // Non-copyable, non-movable
    VirtualMemoryManager(const VirtualMemoryManager&) = delete;
    VirtualMemoryManager& operator=(const VirtualMemoryManager&) = delete;

    /// Initialize the VMM: create the kernel address space with higher-half
    /// mappings, identity map for the physical memory direct map region, and
    /// switch to the new page tables.
    /// @param pmm Reference to the initialized physical memory manager.
    void init(PhysicalMemoryManager& pmm);

    /// Map a single 4KB page.
    /// @param virt Virtual address (will be page-aligned down).
    /// @param phys Physical address to map to.
    /// @param flags Page flags (Present is added automatically).
    /// @return true on success, false if page table allocation failed.
    bool map_page(VirtAddr virt, PhysAddr phys, PageFlags flags);

    /// Unmap a single 4KB page and invalidate the TLB entry.
    /// @param virt Virtual address to unmap.
    void unmap_page(VirtAddr virt);

    /// Map a contiguous range of pages.
    /// @param virt_start Starting virtual address.
    /// @param phys_start Starting physical address.
    /// @param count Number of pages to map.
    /// @param flags Page flags.
    /// @return true on success, false if any page table allocation failed.
    bool map_pages(VirtAddr virt_start, PhysAddr phys_start, uint64_t count, PageFlags flags);

    /// Unmap a contiguous range of pages.
    void unmap_pages(VirtAddr virt_start, uint64_t count);

    /// Translate a virtual address to its physical address.
    /// Walks the page tables for the current address space.
    /// @param virt Virtual address to translate.
    /// @return Physical address, or 0 if not mapped.
    [[nodiscard]] PhysAddr virt_to_phys(VirtAddr virt) const;

    /// Create a new, empty address space.
    /// The kernel higher-half mappings are automatically shared (by copying
    /// PML4 entries 256-511 from the kernel's PML4).
    /// @return A new AddressSpace, or one with pml4_phys=0 on failure.
    [[nodiscard]] AddressSpace create_address_space();

    /// Destroy an address space: free all page table structures and
    /// user-space mapped pages.
    void destroy_address_space(AddressSpace& space);

    /// Switch to a different address space by loading its PML4 into CR3.
    void switch_address_space(const AddressSpace& space);

    /// Get the current kernel address space.
    [[nodiscard]] const AddressSpace& kernel_address_space() const {
        return m_kernel_space;
    }

    /// Convert a physical address to its kernel virtual address using the
    /// direct physical map.
    [[nodiscard]] static constexpr VirtAddr phys_to_virt(PhysAddr phys) {
        return phys + KERNEL_PHYS_MAP_BASE;
    }

    /// Convert a kernel virtual address (in the direct map region) back to physical.
    [[nodiscard]] static constexpr PhysAddr virt_to_phys_direct(VirtAddr virt) {
        return virt - KERNEL_PHYS_MAP_BASE;
    }

private:
    /// Extract the PML4 index from a virtual address
    [[nodiscard]] static constexpr uint64_t pml4_index(VirtAddr addr) {
        return (addr >> PML4_SHIFT) & INDEX_MASK;
    }

    /// Extract the PDPT index from a virtual address
    [[nodiscard]] static constexpr uint64_t pdpt_index(VirtAddr addr) {
        return (addr >> PDPT_SHIFT) & INDEX_MASK;
    }

    /// Extract the PD index from a virtual address
    [[nodiscard]] static constexpr uint64_t pd_index(VirtAddr addr) {
        return (addr >> PD_SHIFT) & INDEX_MASK;
    }

    /// Extract the PT index from a virtual address
    [[nodiscard]] static constexpr uint64_t pt_index(VirtAddr addr) {
        return (addr >> PT_SHIFT) & INDEX_MASK;
    }

    /// Allocate a new page table (zeroed), returning both physical and virtual addrs.
    /// @return Pointer to the new PageTable, or nullptr on failure.
    [[nodiscard]] PageTable* alloc_page_table();

    /// Ensure all intermediate page table levels exist for a given virtual address.
    /// Creates PML4E → PDPTE → PDE entries as needed.
    /// @return Pointer to the final-level PageTable (PT), or nullptr on failure.
    [[nodiscard]] PageTable* ensure_page_table_hierarchy(VirtAddr virt);

    /// Invalidate a single TLB entry
    static void invlpg(VirtAddr addr);

    /// Load a new PML4 physical address into CR3
    static void load_cr3(PhysAddr pml4_phys);

    /// Read the current CR3 value
    [[nodiscard]] static PhysAddr read_cr3();

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    /// The hypervisor's own address space
    AddressSpace m_kernel_space{};

    /// Reference to the physical memory manager (set during init)
    PhysicalMemoryManager* m_pmm = nullptr;

    /// Whether init() has been called
    bool m_initialized = false;
};

/// Global VMM instance
extern VirtualMemoryManager g_vmm;

} // namespace bminos::mm
