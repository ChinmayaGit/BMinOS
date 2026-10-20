// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Kernel Heap Allocator
// =====================
// A hybrid slab + free-list heap allocator for the hypervisor kernel.
//
// Design:
// - Small allocations (≤ 2048 bytes) use slab allocators with fixed-size pools.
//   Slab sizes: 16, 32, 64, 128, 256, 512, 1024, 2048 bytes.
//   This eliminates fragmentation for the most common allocation sizes.
// - Large allocations (> 2048 bytes) use a sorted free-list with first-fit
//   strategy, splitting and coalescing blocks as needed.
// - All allocations are aligned to at least 16 bytes (required for SSE on x86_64).
// - The heap grows on demand by requesting pages from the PMM and mapping them
//   into the kernel heap virtual address range (KERNEL_HEAP_BASE).
//
// Thread safety: All public functions use a spinlock internally.

#pragma once

#include <cstdint>
#include <cstddef>

namespace bminos::mm {

// Forward declarations — we don't want to include pmm.hpp/vmm.hpp in a
// header that everything includes (heap is used everywhere).
class PhysicalMemoryManager;
class VirtualMemoryManager;

// =============================================================================
// Constants
// =============================================================================

/// Minimum allocation alignment (16 bytes for SSE compatibility)
inline constexpr size_t MIN_ALIGNMENT = 16;

/// Maximum allocation size handled by slab allocators
inline constexpr size_t SLAB_MAX_SIZE = 2048;

/// Number of slab size classes
inline constexpr size_t SLAB_CLASS_COUNT = 8;

/// Slab size classes: power-of-two sizes from 16 to 2048
inline constexpr size_t SLAB_SIZES[SLAB_CLASS_COUNT] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

/// Objects per slab page (calculated so each slab fits in a 4KB page)
inline constexpr size_t SLAB_OBJECTS_PER_PAGE[SLAB_CLASS_COUNT] = {
    256, 128, 64, 32, 16, 8, 4, 2
};

/// Initial number of heap pages to map at startup
inline constexpr size_t INITIAL_HEAP_PAGES = 64; // 256 KB

// =============================================================================
// Free-list block header (for large allocations)
// =============================================================================

/// Header prepended to each free-list block.
/// When allocated, only `size` and `magic` are meaningful.
/// When free, `next` and `prev` link it into the free list.
struct BlockHeader {
    uint64_t    size;       // Size of the usable region (excluding this header)
    uint64_t    magic;      // Magic number to detect corruption / double-free
    BlockHeader* next;      // Next free block (only valid when free)
    BlockHeader* prev;      // Previous free block (only valid when free)
};

/// Magic values for corruption detection
inline constexpr uint64_t BLOCK_MAGIC_ALLOCATED = 0x4E455855534D454DULL; // "BMINOSMEM"
inline constexpr uint64_t BLOCK_MAGIC_FREE      = 0x465245454D454D21ULL; // "FREEMEM!"

// =============================================================================
// Slab allocator structures
// =============================================================================

/// A slab is a single page (or group of pages) divided into fixed-size objects.
struct SlabPage {
    SlabPage*   next;           // Next slab page in this size class
    uint64_t    bitmap[4];      // Allocation bitmap (up to 256 objects)
    uint16_t    object_size;    // Size of each object in this slab
    uint16_t    object_count;   // Total objects in this slab
    uint16_t    free_count;     // Number of free objects
    uint16_t    _pad;
    uint8_t*    data_start;     // Start of the object data area
};

/// Per-size-class slab cache
struct SlabCache {
    SlabPage*   partial_slabs;  // Slabs with some free objects
    SlabPage*   full_slabs;     // Completely full slabs (for freeing lookups)
    uint16_t    object_size;    // Object size for this cache
    uint16_t    objects_per_slab;
    uint32_t    total_allocated; // Statistics: total objects allocated
    uint32_t    total_freed;     // Statistics: total objects freed
};

/// Heap statistics
struct HeapStatistics {
    uint64_t total_heap_size;       // Total heap virtual address space used
    uint64_t slab_bytes_allocated;  // Bytes allocated via slab allocators
    uint64_t freelist_bytes_allocated; // Bytes allocated via free-list
    uint64_t slab_bytes_free;       // Free bytes in slab pages
    uint64_t freelist_bytes_free;   // Free bytes in free-list
    uint64_t allocation_count;      // Total number of active allocations
    uint64_t total_allocations;     // Lifetime allocation count
    uint64_t total_frees;           // Lifetime free count
};

// =============================================================================
// Kernel Heap
// =============================================================================

/// The kernel heap allocator.
///
/// Usage:
///   bminos::mm::g_heap.init(pmm, vmm);
///   void* p = bminos::mm::kmalloc(128);
///   bminos::mm::kfree(p);
///
/// Or via C++ operators:
///   auto* obj = new MyStruct{};
///   delete obj;
class KernelHeap {
public:
    KernelHeap() = default;

    // Non-copyable, non-movable
    KernelHeap(const KernelHeap&) = delete;
    KernelHeap& operator=(const KernelHeap&) = delete;

    /// Initialize the heap: map initial pages, set up slab caches and free list.
    /// Must be called after PMM and VMM are initialized.
    void init(PhysicalMemoryManager& pmm, VirtualMemoryManager& vmm);

    /// Allocate `size` bytes with default alignment (16 bytes).
    /// @return Pointer to allocated memory, or nullptr on failure.
    [[nodiscard]] void* allocate(size_t size);

    /// Allocate `size` bytes with specified alignment.
    /// @param size Allocation size in bytes.
    /// @param alignment Required alignment (must be power of 2, ≥ MIN_ALIGNMENT).
    /// @return Pointer to allocated memory, or nullptr on failure.
    [[nodiscard]] void* allocate_aligned(size_t size, size_t alignment);

    /// Free a previously allocated pointer.
    /// Passing nullptr is a no-op.
    void deallocate(void* ptr);

    /// Get current heap statistics.
    [[nodiscard]] HeapStatistics get_statistics() const;

    /// Check heap integrity (debug builds).
    /// Walks the free list and slab structures looking for corruption.
    /// @return true if the heap appears consistent.
    [[nodiscard]] bool verify_integrity() const;

private:
    /// Find the appropriate slab size class for an allocation.
    /// @return Index into m_slab_caches, or SLAB_CLASS_COUNT if too large.
    [[nodiscard]] size_t find_slab_class(size_t size) const;

    /// Allocate from a slab cache.
    [[nodiscard]] void* slab_allocate(size_t class_index);

    /// Free back to a slab cache.
    /// @return true if the pointer belonged to this slab class.
    bool slab_deallocate(void* ptr);

    /// Allocate from the free list (for large allocations).
    [[nodiscard]] void* freelist_allocate(size_t size, size_t alignment);

    /// Free back to the free list, coalescing adjacent blocks.
    void freelist_deallocate(void* ptr);

    /// Grow the heap by mapping additional pages.
    /// @param pages Number of pages to add.
    /// @return true on success.
    bool grow_heap(size_t pages);

    /// Create a new slab page for a given size class.
    [[nodiscard]] SlabPage* create_slab_page(size_t class_index);

    /// Acquire the spinlock
    void lock();

    /// Release the spinlock
    void unlock();

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    /// Slab caches, one per size class
    SlabCache m_slab_caches[SLAB_CLASS_COUNT] = {};

    /// Head of the free list (for large allocations)
    BlockHeader* m_free_list = nullptr;

    /// Current end of the heap (next virtual address to map)
    uint64_t m_heap_end = 0;

    /// PMM and VMM references
    PhysicalMemoryManager* m_pmm = nullptr;
    VirtualMemoryManager*  m_vmm = nullptr;

    /// Statistics
    HeapStatistics m_stats = {};

    /// Spinlock
    volatile uint64_t m_lock = 0;

    /// Whether init() has been called
    bool m_initialized = false;
};

/// Global kernel heap instance
extern KernelHeap g_heap;

// =============================================================================
// Public API — Free functions
// =============================================================================

/// Allocate `size` bytes from the kernel heap.
[[nodiscard]] inline void* kmalloc(size_t size) {
    return g_heap.allocate(size);
}

/// Allocate `size` bytes with specified alignment.
[[nodiscard]] inline void* kmalloc_aligned(size_t size, size_t alignment) {
    return g_heap.allocate_aligned(size, alignment);
}

/// Free memory previously allocated with kmalloc.
inline void kfree(void* ptr) {
    g_heap.deallocate(ptr);
}

} // namespace bminos::mm

// =============================================================================
// Global operator new/delete overloads
// =============================================================================
// In a freestanding (bare-metal) environment, there's no default operator new.
// We provide these global overloads so that C++ `new`/`delete` expressions
// work and route through our kernel heap.

[[nodiscard]] void* operator new(size_t size);
[[nodiscard]] void* operator new[](size_t size);
[[nodiscard]] void* operator new(size_t size, std::align_val_t alignment);
[[nodiscard]] void* operator new[](size_t size, std::align_val_t alignment);

void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;
void operator delete(void* ptr, size_t size) noexcept;
void operator delete[](void* ptr, size_t size) noexcept;
void operator delete(void* ptr, std::align_val_t alignment) noexcept;
void operator delete[](void* ptr, std::align_val_t alignment) noexcept;

// Placement new — doesn't allocate, just constructs at a given address.
// Must be provided in freestanding environments.
[[nodiscard]] inline void* operator new(size_t, void* ptr) noexcept { return ptr; }
[[nodiscard]] inline void* operator new[](size_t, void* ptr) noexcept { return ptr; }
