// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Kernel Heap Allocator Implementation
// =====================================

#include "heap.hpp"
#include "pmm.hpp"
#include "vmm.hpp"

namespace bminos::mm {

KernelHeap g_heap;

void KernelHeap::lock() {
    while (__atomic_test_and_set(&m_lock, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__)
        asm volatile("pause");
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }
}

void KernelHeap::unlock() {
    __atomic_clear(&m_lock, __ATOMIC_RELEASE);
}

size_t KernelHeap::find_slab_class(size_t size) const {
    for (size_t i = 0; i < SLAB_CLASS_COUNT; ++i) {
        if (size <= SLAB_SIZES[i]) {
            return i;
        }
    }
    return SLAB_CLASS_COUNT;
}

bool KernelHeap::grow_heap(size_t pages) {
    if (!m_pmm || !m_vmm) return false;

    for (size_t i = 0; i < pages; ++i) {
        PhysAddr phys = m_pmm->alloc_page();
        if (!phys) return false;

        if (!m_vmm->map_page(m_heap_end, phys, PageFlags::KernelData)) {
            m_pmm->free_page(phys);
            return false;
        }

        m_heap_end += PAGE_SIZE;
        m_stats.total_heap_size += PAGE_SIZE;
    }

    return true;
}

SlabPage* KernelHeap::create_slab_page(size_t class_index) {
    if (class_index >= SLAB_CLASS_COUNT) return nullptr;

    // Allocate 1 page for the slab
    PhysAddr phys = m_pmm->alloc_page();
    if (!phys) return nullptr;

    VirtAddr virt = VirtualMemoryManager::phys_to_virt(phys);
    auto* page = reinterpret_cast<SlabPage*>(virt);

    page->next = nullptr;
    page->object_size = static_cast<uint16_t>(SLAB_SIZES[class_index]);
    page->object_count = static_cast<uint16_t>(SLAB_OBJECTS_PER_PAGE[class_index]);
    page->free_count = page->object_count;
    page->_pad = 0;

    for (int i = 0; i < 4; ++i) {
        page->bitmap[i] = 0;
    }

    // Objects start after the SlabPage header, aligned to 16 bytes
    uintptr_t data_offset = sizeof(SlabPage);
    data_offset = (data_offset + 15) & ~15;
    page->data_start = reinterpret_cast<uint8_t*>(virt + data_offset);

    return page;
}

void KernelHeap::init(PhysicalMemoryManager& pmm, VirtualMemoryManager& vmm) {
    lock();
    m_pmm = &pmm;
    m_vmm = &vmm;
    m_heap_end = KERNEL_HEAP_BASE;

    // Initialize slab caches
    for (size_t i = 0; i < SLAB_CLASS_COUNT; ++i) {
        m_slab_caches[i].partial_slabs = nullptr;
        m_slab_caches[i].full_slabs = nullptr;
        m_slab_caches[i].object_size = static_cast<uint16_t>(SLAB_SIZES[i]);
        m_slab_caches[i].objects_per_slab = static_cast<uint16_t>(SLAB_OBJECTS_PER_PAGE[i]);
        m_slab_caches[i].total_allocated = 0;
        m_slab_caches[i].total_freed = 0;
    }

    // Map initial heap pages
    grow_heap(INITIAL_HEAP_PAGES);

    // Initial free block from the mapped heap space
    uint64_t initial_bytes = INITIAL_HEAP_PAGES * PAGE_SIZE;
    m_free_list = reinterpret_cast<BlockHeader*>(KERNEL_HEAP_BASE);
    m_free_list->size = initial_bytes - sizeof(BlockHeader);
    m_free_list->magic = BLOCK_MAGIC_FREE;
    m_free_list->next = nullptr;
    m_free_list->prev = nullptr;

    m_stats.freelist_bytes_free = m_free_list->size;
    m_initialized = true;
    unlock();
}

void* KernelHeap::slab_allocate(size_t class_index) {
    SlabCache& cache = m_slab_caches[class_index];

    // Check partial slabs first
    SlabPage* page = cache.partial_slabs;
    if (!page) {
        page = create_slab_page(class_index);
        if (!page) return nullptr;
        page->next = cache.partial_slabs;
        cache.partial_slabs = page;
    }

    // Find free object in slab bitmap
    for (uint16_t i = 0; i < page->object_count; ++i) {
        uint16_t b_idx = i / 64;
        uint16_t b_bit = i % 64;

        if (!(page->bitmap[b_idx] & (1ULL << b_bit))) {
            page->bitmap[b_idx] |= (1ULL << b_bit);
            page->free_count--;

            // If page is now full, move to full_slabs
            if (page->free_count == 0) {
                cache.partial_slabs = page->next;
                page->next = cache.full_slabs;
                cache.full_slabs = page;
            }

            cache.total_allocated++;
            m_stats.slab_bytes_allocated += page->object_size;
            m_stats.allocation_count++;
            m_stats.total_allocations++;

            return page->data_start + (i * page->object_size);
        }
    }

    return nullptr;
}

bool KernelHeap::slab_deallocate(void* ptr) {
    if (!ptr) return false;

    // Find which page this belongs to
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t page_addr = addr & ~(PAGE_SIZE - 1);
    auto* page = reinterpret_cast<SlabPage*>(page_addr);

    // Validate object size class
    size_t class_idx = find_slab_class(page->object_size);
    if (class_idx >= SLAB_CLASS_COUNT) return false;

    SlabCache& cache = m_slab_caches[class_idx];

    uintptr_t offset = addr - reinterpret_cast<uintptr_t>(page->data_start);
    uint16_t obj_index = static_cast<uint16_t>(offset / page->object_size);
    if (obj_index >= page->object_count) return false;

    uint16_t b_idx = obj_index / 64;
    uint16_t b_bit = obj_index % 64;

    if (!(page->bitmap[b_idx] & (1ULL << b_bit))) {
        return false; // Double free or corruption
    }

    page->bitmap[b_idx] &= ~(1ULL << b_bit);
    page->free_count++;
    cache.total_freed++;
    m_stats.slab_bytes_allocated -= page->object_size;
    m_stats.total_frees++;
    if (m_stats.allocation_count > 0) m_stats.allocation_count--;

    // Move from full_slabs to partial_slabs if needed
    if (page->free_count == 1) {
        // Remove from full_slabs
        SlabPage** curr = &cache.full_slabs;
        while (*curr && *curr != page) {
            curr = &(*curr)->next;
        }
        if (*curr) {
            *curr = page->next;
            page->next = cache.partial_slabs;
            cache.partial_slabs = page;
        }
    }

    return true;
}

void* KernelHeap::freelist_allocate(size_t size, size_t alignment) {
    if (!m_free_list) return nullptr;

    // Align size to 16 bytes
    size = (size + 15) & ~15;

    BlockHeader* curr = m_free_list;
    while (curr) {
        if (curr->magic != BLOCK_MAGIC_FREE) {
            return nullptr; // Corrupted heap!
        }

        uintptr_t data_addr = reinterpret_cast<uintptr_t>(curr) + sizeof(BlockHeader);
        uintptr_t aligned_addr = (data_addr + (alignment - 1)) & ~(alignment - 1);
        size_t pad = aligned_addr - data_addr;

        if (curr->size >= size + pad) {
            // Can satisfy allocation
            size_t remaining = curr->size - (size + pad);

            // If remaining space is large enough for a new block, split it
            if (remaining > sizeof(BlockHeader) + 32) {
                auto* next_block = reinterpret_cast<BlockHeader*>(aligned_addr + size);
                next_block->size = remaining - sizeof(BlockHeader);
                next_block->magic = BLOCK_MAGIC_FREE;
                next_block->next = curr->next;
                next_block->prev = curr->prev;

                if (curr->prev) curr->prev->next = next_block;
                else m_free_list = next_block;
                if (curr->next) curr->next->prev = next_block;

                curr->size = size + pad;
            } else {
                // Remove entire block from free list
                if (curr->prev) curr->prev->next = curr->next;
                else m_free_list = curr->next;
                if (curr->next) curr->next->prev = curr->prev;
            }

            curr->magic = BLOCK_MAGIC_ALLOCATED;
            m_stats.freelist_bytes_allocated += curr->size;
            m_stats.allocation_count++;
            m_stats.total_allocations++;

            return reinterpret_cast<void*>(aligned_addr);
        }

        curr = curr->next;
    }

    // Try growing heap
    size_t needed_pages = (size + sizeof(BlockHeader) + PAGE_SIZE - 1) / PAGE_SIZE;
    if (grow_heap(needed_pages > 16 ? needed_pages : 16)) {
        return freelist_allocate(size, alignment);
    }

    return nullptr;
}

void KernelHeap::freelist_deallocate(void* ptr) {
    if (!ptr) return;

    auto* block = reinterpret_cast<BlockHeader*>(reinterpret_cast<uintptr_t>(ptr) - sizeof(BlockHeader));
    if (block->magic != BLOCK_MAGIC_ALLOCATED) {
        return; // Invalid pointer or double free
    }

    block->magic = BLOCK_MAGIC_FREE;
    m_stats.freelist_bytes_allocated -= block->size;
    m_stats.total_frees++;
    if (m_stats.allocation_count > 0) m_stats.allocation_count--;

    // Insert at front of free list
    block->next = m_free_list;
    block->prev = nullptr;
    if (m_free_list) {
        m_free_list->prev = block;
    }
    m_free_list = block;
}

void* KernelHeap::allocate(size_t size) {
    return allocate_aligned(size, MIN_ALIGNMENT);
}

void* KernelHeap::allocate_aligned(size_t size, size_t alignment) {
    if (size == 0) return nullptr;

    lock();
    void* result = nullptr;
    size_t slab_idx = find_slab_class(size);

    if (slab_idx < SLAB_CLASS_COUNT && alignment <= MIN_ALIGNMENT) {
        result = slab_allocate(slab_idx);
    } else {
        result = freelist_allocate(size, alignment);
    }

    unlock();
    return result;
}

void KernelHeap::deallocate(void* ptr) {
    if (!ptr) return;

    lock();
    // Try slab first
    if (!slab_deallocate(ptr)) {
        freelist_deallocate(ptr);
    }
    unlock();
}

HeapStatistics KernelHeap::get_statistics() const {
    return m_stats;
}

bool KernelHeap::verify_integrity() const {
    BlockHeader* curr = m_free_list;
    while (curr) {
        if (curr->magic != BLOCK_MAGIC_FREE) return false;
        curr = curr->next;
    }
    return true;
}

} // namespace bminos::mm

// =============================================================================
// Global operator new/delete implementations
// =============================================================================

void* operator new(size_t size) {
    void* p = bminos::mm::kmalloc(size);
    return p;
}

void* operator new[](size_t size) {
    return bminos::mm::kmalloc(size);
}

void* operator new(size_t size, std::align_val_t alignment) {
    return bminos::mm::kmalloc_aligned(size, static_cast<size_t>(alignment));
}

void* operator new[](size_t size, std::align_val_t alignment) {
    return bminos::mm::kmalloc_aligned(size, static_cast<size_t>(alignment));
}

void operator delete(void* ptr) noexcept {
    bminos::mm::kfree(ptr);
}

void operator delete[](void* ptr) noexcept {
    bminos::mm::kfree(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
    bminos::mm::kfree(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
    bminos::mm::kfree(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
    bminos::mm::kfree(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept {
    bminos::mm::kfree(ptr);
}

