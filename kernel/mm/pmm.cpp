// BMinOS - Unified Multi-OS Hypervisor Platform
// Copyright (c) 2026 BMinOS Contributors. MIT License.
//
// Physical Memory Manager (PMM) Implementation
// ============================================

#include "pmm.hpp"

namespace bminos::mm {

// Global PMM instance
PhysicalMemoryManager g_pmm;

void PhysicalMemoryManager::lock() {
    while (__atomic_test_and_set(&m_lock, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__)
        asm volatile("pause");
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }
}

void PhysicalMemoryManager::unlock() {
    __atomic_clear(&m_lock, __ATOMIC_RELEASE);
}

void PhysicalMemoryManager::set_bit(uint64_t frame) {
    if (frame >= MAX_PAGE_FRAMES) return;
    uint64_t idx = frame / BITS_PER_ELEMENT;
    uint64_t bit = frame % BITS_PER_ELEMENT;
    m_bitmap[idx] |= (1ULL << bit);
}

void PhysicalMemoryManager::clear_bit(uint64_t frame) {
    if (frame >= MAX_PAGE_FRAMES) return;
    uint64_t idx = frame / BITS_PER_ELEMENT;
    uint64_t bit = frame % BITS_PER_ELEMENT;
    m_bitmap[idx] &= ~(1ULL << bit);
}

bool PhysicalMemoryManager::test_bit(uint64_t frame) const {
    if (frame >= MAX_PAGE_FRAMES) return true; // Treat out-of-range as used
    uint64_t idx = frame / BITS_PER_ELEMENT;
    uint64_t bit = frame % BITS_PER_ELEMENT;
    return (m_bitmap[idx] & (1ULL << bit)) != 0;
}

uint64_t PhysicalMemoryManager::find_first_free(uint64_t start_frame) const {
    for (uint64_t frame = start_frame; frame < m_max_frame; ++frame) {
        uint64_t idx = frame / BITS_PER_ELEMENT;
        // Optimization: skip entire 64-bit blocks if all bits are 1
        if (m_bitmap[idx] == ~0ULL && (frame % BITS_PER_ELEMENT == 0)) {
            frame += BITS_PER_ELEMENT - 1;
            continue;
        }
        if (!test_bit(frame)) {
            return frame;
        }
    }
    return static_cast<uint64_t>(-1);
}

uint64_t PhysicalMemoryManager::find_contiguous_free(uint64_t count) const {
    if (count == 0) return static_cast<uint64_t>(-1);
    if (count == 1) return find_first_free(0);

    uint64_t start = 0;
    while (start + count <= m_max_frame) {
        start = find_first_free(start);
        if (start == static_cast<uint64_t>(-1) || start + count > m_max_frame) {
            return static_cast<uint64_t>(-1);
        }

        bool contiguous = true;
        for (uint64_t i = 1; i < count; ++i) {
            if (test_bit(start + i)) {
                start = start + i + 1;
                contiguous = false;
                break;
            }
        }

        if (contiguous) {
            return start;
        }
    }
    return static_cast<uint64_t>(-1);
}

void PhysicalMemoryManager::init(const MemoryMap& memory_map) {
    lock();

    // 1. Mark all memory as used initially
    for (uint64_t i = 0; i < BITMAP_SIZE; ++i) {
        m_bitmap[i] = ~0ULL;
    }

    m_total_frames = memory_map.total_memory / PAGE_SIZE;
    if (m_total_frames > MAX_PAGE_FRAMES) {
        m_total_frames = MAX_PAGE_FRAMES;
    }
    m_max_frame = m_total_frames;
    m_used_frames = m_total_frames;
    m_reserved_frames = m_total_frames;
    m_hypervisor_frames = 0;

    // 2. Mark available regions as free
    for (uint64_t i = 0; i < memory_map.region_count; ++i) {
        const auto& region = memory_map.regions[i];
        if (region.type == MemoryRegionType::Available) {
            uint64_t start_frame = addr_to_frame(region.base);
            uint64_t frame_count = region.length / PAGE_SIZE;

            for (uint64_t f = 0; f < frame_count; ++f) {
                uint64_t frame = start_frame + f;
                if (frame < m_max_frame && test_bit(frame)) {
                    clear_bit(frame);
                    m_used_frames--;
                    m_reserved_frames--;
                }
            }
        }
    }

    // Reserve first 1MB page frames (BIOS/IVT/BDA/EBDA compatibility)
    uint64_t low_frames = (1024 * 1024) / PAGE_SIZE;
    for (uint64_t f = 0; f < low_frames; ++f) {
        if (!test_bit(f)) {
            set_bit(f);
            m_used_frames++;
            m_reserved_frames++;
        }
    }

    m_initialized = true;
    unlock();
}

PhysAddr PhysicalMemoryManager::alloc_page() {
    lock();
    uint64_t frame = find_first_free(0);
    if (frame == static_cast<uint64_t>(-1)) {
        unlock();
        return 0;
    }

    set_bit(frame);
    m_used_frames++;
    unlock();

    return frame_to_addr(frame);
}

PhysAddr PhysicalMemoryManager::alloc_pages(uint64_t count) {
    if (count == 0) return 0;
    if (count == 1) return alloc_page();

    lock();
    uint64_t start_frame = find_contiguous_free(count);
    if (start_frame == static_cast<uint64_t>(-1)) {
        unlock();
        return 0;
    }

    for (uint64_t i = 0; i < count; ++i) {
        set_bit(start_frame + i);
    }
    m_used_frames += count;
    unlock();

    return frame_to_addr(start_frame);
}

void PhysicalMemoryManager::free_page(PhysAddr addr) {
    if (addr % PAGE_SIZE != 0) return;
    uint64_t frame = addr_to_frame(addr);

    lock();
    if (test_bit(frame)) {
        clear_bit(frame);
        if (m_used_frames > 0) m_used_frames--;
    }
    unlock();
}

void PhysicalMemoryManager::free_pages(PhysAddr addr, uint64_t count) {
    if (addr % PAGE_SIZE != 0 || count == 0) return;
    uint64_t start_frame = addr_to_frame(addr);

    lock();
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t frame = start_frame + i;
        if (test_bit(frame)) {
            clear_bit(frame);
            if (m_used_frames > 0) m_used_frames--;
        }
    }
    unlock();
}

bool PhysicalMemoryManager::is_page_used(PhysAddr addr) const {
    uint64_t frame = addr_to_frame(addr);
    return test_bit(frame);
}

PmmStatistics PhysicalMemoryManager::get_statistics() const {
    PmmStatistics stats{};
    stats.total_pages = m_total_frames;
    stats.used_pages = m_used_frames;
    stats.free_pages = (m_total_frames > m_used_frames) ? (m_total_frames - m_used_frames) : 0;
    stats.reserved_pages = m_reserved_frames;
    stats.hypervisor_pages = m_hypervisor_frames;
    stats.total_bytes = stats.total_pages * PAGE_SIZE;
    stats.free_bytes = stats.free_pages * PAGE_SIZE;
    return stats;
}

void PhysicalMemoryManager::mark_region_reserved(PhysAddr base, uint64_t length) {
    uint64_t start_frame = addr_to_frame(base);
    uint64_t frame_count = (length + PAGE_SIZE - 1) / PAGE_SIZE;

    lock();
    for (uint64_t i = 0; i < frame_count; ++i) {
        uint64_t frame = start_frame + i;
        if (!test_bit(frame)) {
            set_bit(frame);
            m_used_frames++;
            m_reserved_frames++;
        }
    }
    unlock();
}

void PhysicalMemoryManager::mark_region_available(PhysAddr base, uint64_t length) {
    uint64_t start_frame = addr_to_frame(base);
    uint64_t frame_count = length / PAGE_SIZE;

    lock();
    for (uint64_t i = 0; i < frame_count; ++i) {
        uint64_t frame = start_frame + i;
        if (test_bit(frame)) {
            clear_bit(frame);
            if (m_used_frames > 0) m_used_frames--;
            if (m_reserved_frames > 0) m_reserved_frames--;
        }
    }
    unlock();
}

} // namespace bminos::mm

